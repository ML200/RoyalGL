// Shared GLSL declarations for the path tracer. GLSL has no native
// #include; RoyalGL::Shader textually splices this file in wherever a
// shader writes `#include "common.glsl"` (see src/gfx/Shader.cpp).
//
// Struct field order/types here MUST match src/gfx/GPUTypes.h exactly -
// see docs/ARCHITECTURE.md for why every field is a 16-byte-aligned
// vec4/uvec4.

const float PI = 3.14159265358979323846;

// tinybvh::BVH::BVHNode ("Wald" 32-byte layout), uploaded to the GPU
// unmodified by BVHBuilder - its C++ layout already matches this std430
// layout byte-for-byte.
struct BVHNode
{
    vec3 aabbMin; uint leftFirst;
    vec3 aabbMax; uint triCount;
};

struct Triangle
{
    vec4 p0, p1, p2;        // world-space position, .w unused
    vec4 n0, n1, n2;        // shading normal, .w unused
    vec4 uv0_uv1;           // uv0.xy, uv1.xy
    vec4 uv2_material;      // uv2.xy, materialIndex (exact float), unused
};

struct Material
{
    vec4 baseColor; // .rgb albedo (diffuse/layered base) / tint (glass/rough dielectric) / F0 (conductor)
    vec4 emissive;  // .rgb emissive radiance
    vec4 params;    // x=metallic (layered base blend), y=roughness, z=ior,
                    // w=type (0=diffuse 1=glass 2=conductor 3=rough dielectric 4=layered)
    vec4 coat;      // layered: x=coat roughness, y=coat IOR, z=optical depth tau, w=HG g
    vec4 coatTint;  // layered: .rgb medium single-scattering albedo
};

layout(std140, binding = 0) uniform FrameUBO
{
    vec4 camPos;       // xyz
    vec4 camForward;   // xyz
    vec4 camRight;     // xyz
    vec4 camUp;        // xyz
    vec4 cameraParams; // x=tanHalfFovY, y=aspect
    vec4 background;   // .rgb sky color, .a intensity
    uvec4 frameInfo;   // x=width, y=height, z=sampleIndex, w=maxBounces
    vec4 renderParams; // x=exposure, y=total light power
    uvec4 lightInfo;   // x=light triangle count, y=NEE enabled (0/1), z=BDPT light path count, w=lens flare samples
    vec4 lensParams;   // x=sensor half width mm, y=sensor half height mm, z=front vertex z mm, w=pupil plane z mm
    vec4 lensParams2;  // x=camera mode (0=pinhole 1=lens), y=flare enabled, z=rear semi-diameter mm, w=front semi-diameter mm
    vec4 lensParams3;  // x=diffraction enabled, y=diffraction intensity, z=diffraction edge width mm
    vec4 prevCamPos;       // previous frame's camera (ReSTIR reprojection)
    vec4 prevCamForward;
    vec4 prevCamRight;
    vec4 prevCamUp;
    vec4 prevCameraParams; // x=tanHalfFovY, y=aspect
    uvec4 restirParams;    // x=debug view, y=flags (bit0 restir, bit1 temporal, bit2 spatial,
                           //   bit3 accumulate frames), z=frame counter, w=ping-pong parity
    vec4 fogParams;        // homogeneous medium: x=sigma_s, y=sigma_a, z=HG g, w=enabled
    vec4 spmisParams;      // x=mode-2 score EMA rate, y=mode-2 defensive mix, z/w diagnostics
    // Instance transforms for object-space surface storage (see
    // GPUTypes.h): frame-persistent positions (G-buffer, reservoir
    // reconnection data) are stored in instance object space and converted
    // with the CURRENT matrices on load, so they track moving objects.
    // instInfo.x = instance count (0 = machinery disabled).
    uvec4 instInfo;
    uvec4 instTable[4]; // firstTriangle per instance, 16 entries
    mat4 instToWorld[16];
    mat4 instToObject[16];
} uFrame;

// Maps a scene-triangle index to its instance (instances tile the triangle
// array in ascending order; linear scan over <= 16 entries). Returns the
// instance count when the table is empty (callers treat as static).
uint InstanceOfTriangle(uint triIdx)
{
    uint n = uFrame.instInfo.x;
    uint inst = n;
    for (uint i = 0u; i < n; ++i)
        if (triIdx >= uFrame.instTable[i >> 2][i & 3u]) inst = i;
    return inst;
}

// Global accumulation toggle: off = every pipeline overwrites the image
// with its latest sample instead of averaging, for live per-frame quality
// comparisons (naive PT / NEE / BDPT / ReSTIR).
bool AccumulateFrames() { return (uFrame.restirParams.y & 8u) != 0u; }

layout(std430, binding = 1) readonly buffer BVHNodesSSBO   { BVHNode bvhNodes[]; };
layout(std430, binding = 2) readonly buffer TriIndicesSSBO { uint triIndices[]; };
layout(std430, binding = 3) readonly buffer TrianglesSSBO  { Triangle triangles[]; };
layout(std430, binding = 4) readonly buffer MaterialsSSBO  { Material materials[]; };

// Emissive triangles in light-tree leaf-list order (see
// src/pathtracer/LightTree.cpp), shared by the unidirectional NEE path
// (light-tree descent, shaders/light_tree.glsl) and the bidirectional
// kernels (power-CDF sampling, shaders/bdpt_common.glsl).
struct LightTri
{
    vec4 p0, p1, p2;     // world-space position; p0.w = source scene-triangle
                         // index (exact float), p1.w/p2.w unused
    vec4 normalArea;     // xyz=geometric normal, w=area
    vec4 emissionWeight; // rgb=emitted radiance, w=selection weight (area * luminance)
};
layout(std430, binding = 6) readonly buffer LightTrianglesSSBO { LightTri lightTris[]; };

// Scene-triangle index -> lightTris index (LT_SENTINEL if not emissive).
layout(std430, binding = 7) readonly buffer TriToLightSSBO { uint triToLight[]; };

// Normalized power-proportional CDF over lightTris (last entry 1.0).
layout(std430, binding = 10) readonly buffer LightCdfSSBO { float lightCdf[]; };

const uint  LT_SENTINEL = 0xFFFFFFFFu;
const float LT_ONE_MINUS_EPSILON = 0.99999994;

// ---------------------------------------------------------------- RNG ----
// xorshift32, seeded per-invocation with a Wang hash. Not cryptographically
// anything - just decorrelated enough for path tracing.
uint g_rngState;

uint WangHash(uint seed)
{
    seed = (seed ^ 61u) ^ (seed >> 16u);
    seed *= 9u;
    seed = seed ^ (seed >> 4u);
    seed *= 0x27d4eb2du;
    seed = seed ^ (seed >> 15u);
    return seed;
}

float RandomFloat()
{
    g_rngState ^= (g_rngState << 13u);
    g_rngState ^= (g_rngState >> 17u);
    g_rngState ^= (g_rngState << 5u);
    return float(g_rngState) * (1.0 / 4294967296.0);
}

// Replayable streams (ReSTIR, docs/RESTIR_BDPT_PLAN.md): a subpath is fully
// determined by one base seed, and every (vertex, purpose) pair gets its own
// xorshift stream derived from it. Restarting at a purpose boundary means the
// number of draws one purpose makes can never shift another purpose's values -
// so a shift mapping can replay, say, vertex 3's BSDF sample of a stored path
// without re-executing the NEE sampling that preceded it (whose draw count
// varies with the light-tree descent depth). The unidirectional kernel keeps
// seeding g_rngState directly; these streams are the bidirectional/ReSTIR
// convention.
uint g_rngSeed = 0u;

const uint RNG_PIXEL   = 0u; // subpixel jitter + lens eye-ray sampling (vertex 0)
const uint RNG_EMIT    = 1u; // light pick + point + emission direction (vertex 0)
const uint RNG_BSDF    = 2u; // BSDF direction sampling (incl. the Fresnel choice)
const uint RNG_NEE     = 3u; // s=1 light sampling (tree descent + point)
const uint RNG_CONNECT = 4u; // s>=2 light-vertex pick
const uint RNG_CAMCONN = 5u; // t=1 lens-connection sampling (pupil point + lambdas)
const uint RNG_RIS     = 6u; // ReSTIR reservoir selection draws
const uint RNG_REUSE   = 7u; // ReSTIR neighbor-pixel selection draws
const uint RNG_EVAL    = 8u; // stochastic BSDF *evaluation* draws at the s=1
                             // NEE candidate (layered walk). A dedicated
                             // purpose lets shift mappings re-seed and
                             // reproduce creation's realization exactly -
                             // the light sampling that precedes the eval
                             // consumes a variable number of RNG_NEE draws
                             // (tree descent) that shifts don't replay.
const uint RNG_DIST    = 9u; // free-flight distance sampling in the fog
                             // (one draw per path segment, replayable so
                             // shift mappings reproduce the scatter/pass
                             // classification along replayed prefixes).

void RngStream(uint vertex, uint purpose)
{
    uint h = g_rngSeed + WangHash(vertex * 6971u + purpose * 0x9E3779B9u);
    h = WangHash(h);
    g_rngState = (h != 0u) ? h : 0x9E3779B9u; // xorshift32 must not start at 0
}

vec2 RandomFloat2() { return vec2(RandomFloat(), RandomFloat()); }

vec3 CosineSampleHemisphere(vec3 normal)
{
    vec2 u = RandomFloat2();
    float r = sqrt(u.x);
    float theta = 6.28318530718 * u.y;
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(max(0.0, 1.0 - u.x));

    vec3 up = (abs(normal.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * x + bitangent * y + normal * z);
}

// -------------------------------------------------------------- BSDF -----
// Shared by the unidirectional kernel and both bidirectional subpath
// kernels. Material models:
//   0 diffuse          two-sided Lambertian
//   1 glass            delta dielectric, exact Fresnel reflect/refract
//   2 conductor        GGX microfacet conductor (VNDF sampling), F0 = baseColor
//   3 rough dielectric GGX dielectric (Walter 2007), reflection + transmission
//   4 layered          position-free layered slab (Guo, Hasan, Zhao 2018):
//                      rough dielectric coat over an HG-scattering medium over
//                      a metallic-blended conductor/diffuse base. Sampling is
//                      a stochastic forward walk through the slab; evaluation
//                      is an analytic top-reflection lobe plus a stochastic
//                      internal walk with NEE across the coat interface,
//                      locally MIS-combined with continuation (paper Fig 7ab).
//                      Reported pdfs for layered are a deterministic analytic
//                      APPROXIMATION (paper 5.3.2) - valid for MIS weighting
//                      because every estimator ratio in this codebase pairs
//                      the same reported pdf in the f- and p-products, so the
//                      nominal factors cancel and true throughput remains.
//
// Direction convention: `wi` points from the surface toward the previous
// path vertex, `wo` toward the next one; `n` is the interpolated shading
// normal with its true geometric orientation (NOT pre-flipped - glass and
// rough dielectric need the sign to tell entering from exiting).
//
// Stochastic eval/sampling (layered) draws from the ACTIVE RNG stream, so
// ReSTIR replays reproduce realizations exactly (RngStream re-seeds per
// vertex/purpose); unbiasedness holds in the extended space of path x
// eval-noise, same as the random-replay treatment.

bool MatIsDelta(Material m) { return m.params.w > 0.5 && m.params.w < 1.5; }

// Eligible as an INTERIOR ReSTIR reconnection vertex. Layered is excluded:
// its sampled scatters carry a stochastic-weight/approx-pdf factor that a
// shift's raw re-evaluation cannot reproduce, so caching its outgoing
// suffix would drift under temporal reuse. Paths through layered vertices
// instead use light-point rc / LIGHTRC / pure replay - no candidate or
// MIS technique is lost. (Evaluation-based caches are fine for layered:
// both sides use EvalBsdf realizations.)
bool MatRcCacheable(Material m)
{
    int t = int(m.params.w + 0.5);
    return t != 1 && t != 4;
}

// Scalar roughness proxy for the reconnection criteria (ReSTIR PT
// Enhanced sec. 4.2): diffuse counts as fully rough, delta as perfectly
// smooth. Layered is structurally excluded from rc pairs regardless
// (MatRcCacheable), so its value here never matters.
float MatRcRoughness(Material m)
{
    int t = int(m.params.w + 0.5);
    if (t == 0) return 1.0;
    if (t == 2 || t == 3) return m.params.y;
    return 0.0;
}

// -------------------------------------------- homogeneous medium (fog) ----
// Global homogeneous medium with a Henyey-Greenstein phase function. For a
// homogeneous medium everything is ANALYTIC and the estimator is exact in
// the distance dimension: Beer-Lambert transmittance in closed form (no
// delta/ratio tracking), exponential free-flight sampling whose pdf
// sigma_t*Tr(t) cancels the medium factors of the throughput exactly
// (scatter events contribute the single-scattering albedo, pass-throughs
// contribute 1), and exact HG importance sampling. Volume scattering
// vertices have NO cosine terms; in the recursive MIS quantities the
// per-vertex measure factor "cos" is replaced by sigma_t (the volume event
// density: p_vol = p_omega * sigma_t / d^2 vs p_area = p_omega * cos /
// d^2). Transmittance is kept OUT of the MIS ratios (a consistent,
// partition-preserving convention across every technique - exact
// unbiasedness, approximate optimality in dense media).
bool  FogEnabled() { return uFrame.fogParams.w > 0.5; }
float FogSigmaS()  { return uFrame.fogParams.x; }
float FogSigmaT()  { return uFrame.fogParams.x + uFrame.fogParams.y; }
float FogG()       { return uFrame.fogParams.z; }
float FogAlbedo()  { return uFrame.fogParams.x / max(FogSigmaT(), 1e-8); }
float FogTr(float d) { return FogEnabled() ? exp(-FogSigmaT() * d) : 1.0; }
// Free-flight distance for a uniform u (exponential, pdf sigma_t*e^(-t)).
float FogSampleDist(float u) { return -log(max(1.0 - u, 1e-7)) / max(FogSigmaT(), 1e-8); }

// The camera walk's primary-segment in-scatter technique (airlight) exists
// only when the medium actually scatters.
bool FogScatterEnabled() { return FogEnabled() && FogSigmaS() > 0.0; }

// P(scatter before the surface at distance dSurf) = 1 - Tr(dSurf); dSurf < 0
// marks a primary miss (infinite medium -> certain scatter).
float FogScatterProb(float dSurf)
{
    return (dSurf < 0.0) ? 1.0 : 1.0 - FogTr(dSurf);
}

// TRUNCATED free-flight distance on [0, dSurf): the ReSTIR camera pass
// samples the primary in-scatter point CONDITIONED on scattering before the
// anchor (pdf sigma_t*Tr(t)/pScat with pScat = FogScatterProb(dSurf)), so
// the surface family keeps its deterministic probability-1 anchor and
// fog-off behavior is bit-for-bit unchanged. Exact inversion.
float FogSampleDistTrunc(float u, float pScat)
{
    return -log(max(1.0 - min(u, 0.99999994) * pScat, 1e-7)) / max(FogSigmaT(), 1e-8);
}

// Representative primary in-scatter depth for FOG MOTION VECTORS: the mean
// of the truncated free-flight distribution on [0, dSurf],
//   E[t] = 1/sigma_t - dSurf * Tr(dSurf) / (1 - Tr(dSurf)),
// (-> 1/sigma_t for a primary miss). A SAMPLE-INDEPENDENT function of the
// G-buffer alone - required so temporal history pairing never depends on
// realized reservoir content (the same rule as proxy confidence).
float FogRepDistance(float dSurf)
{
    float sT = max(FogSigmaT(), 1e-8);
    if (dSurf < 0.0) return 1.0 / sT;
    float tr = FogTr(dSurf);
    float pScat = max(1.0 - tr, 1e-7);
    return max(1.0 / sT - dSurf * tr / pScat, 0.0);
}

// HG phase function: cosTheta between the PROPAGATION direction of the
// incoming ray and the outgoing direction (g > 0 = forward scattering).
// Normalized over the sphere; symmetric (pdfRev == pdfDir).
float PhaseHG(float cosTheta, float g)
{
    float d = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 - g * g) / (4.0 * PI * d * sqrt(max(d, 1e-8)));
}

// Sample the HG phase around propagation direction wProp; returns the
// outgoing direction, with pdf = PhaseHG(dot(wProp, out), g).
vec3 SamplePhaseHG(vec3 wProp, float g, vec2 u, out float pdf)
{
    float cosTheta;
    if (abs(g) < 1e-3)
    {
        cosTheta = 1.0 - 2.0 * u.x;
    }
    else
    {
        float sq = (1.0 - g * g) / (1.0 + g - 2.0 * g * u.x);
        cosTheta = (1.0 + g * g - sq * sq) / (2.0 * g);
    }
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = 2.0 * PI * u.y;
    // Orthonormal basis around wProp.
    vec3 a = (abs(wProp.x) > 0.9) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t1 = normalize(cross(a, wProp));
    vec3 t2 = cross(wProp, t1);
    pdf = PhaseHG(cosTheta, g);
    return normalize(wProp * cosTheta + t1 * (sinTheta * cos(phi)) + t2 * (sinTheta * sin(phi)));
}

// Peak of the HG pdf (at cosTheta = sign-of-g extreme), for the volume
// analog of the inverse-footprint reconnection criterion.
float PhaseHGPeak(float g)
{
    float ag = abs(g);
    return (1.0 + ag) / (4.0 * PI * (1.0 - ag) * (1.0 - ag));
}

// Exact unpolarized dielectric Fresnel reflectance; returns 1 on total
// internal reflection.
float FresnelDielectric(float cosI, float etaI, float etaT)
{
    float r = etaI / etaT;
    float sinT2 = r * r * max(0.0, 1.0 - cosI * cosI);
    if (sinT2 >= 1.0) return 1.0;
    float cosT = sqrt(1.0 - sinT2);
    float rs = (etaI * cosI - etaT * cosT) / (etaI * cosI + etaT * cosT);
    float rp = (etaT * cosI - etaI * cosT) / (etaT * cosI + etaI * cosT);
    return 0.5 * (rs * rs + rp * rp);
}

vec3 FresnelSchlick3(vec3 f0, float cosI)
{
    float m = clamp(1.0 - cosI, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

// ------------------------------------------------- GGX microfacet core ---
// All lobe math runs in a local frame with the oriented normal at +z.
// Perceptual roughness -> alpha; the clamp keeps every rough lobe non-delta
// (true deltas remain the exclusive business of the glass material).
float BsdfAlpha(float rough) { return clamp(rough * rough, 1.0e-3, 1.0); }

mat3 BsdfBasis(vec3 n)
{
    vec3 up = (abs(n.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    return mat3(t, b, n); // world = M * local, local = transpose(M) * world
}

float GgxD(vec3 m, float a)
{
    if (m.z <= 0.0) return 0.0;
    float a2 = a * a;
    float t = (m.x * m.x + m.y * m.y) / a2 + m.z * m.z;
    return 1.0 / (PI * a2 * t * t);
}

float GgxLambda(vec3 w, float a)
{
    float c2 = w.z * w.z;
    float s2 = max(1.0 - c2, 0.0);
    if (c2 <= 1.0e-9) return 1.0e9;
    return 0.5 * (sqrt(1.0 + a * a * s2 / c2) - 1.0);
}
float GgxG1(vec3 w, float a) { return 1.0 / (1.0 + GgxLambda(w, a)); }
float GgxG2(vec3 wi, vec3 wo, float a) { return 1.0 / (1.0 + GgxLambda(wi, a) + GgxLambda(wo, a)); }

// Heitz 2018 VNDF sampling; requires w.z > 0, returns m with m.z > 0.
vec3 GgxSampleVndf(vec3 w, float a, vec2 u)
{
    vec3 vh = normalize(vec3(a * w.x, a * w.y, w.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 T1 = (lensq > 1.0e-9) ? vec3(-vh.y, vh.x, 0.0) / sqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 T2 = cross(vh, T1);
    float r = sqrt(u.x);
    float phi = 2.0 * PI * u.y;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    t2 = (1.0 - s) * sqrt(max(0.0, 1.0 - t1 * t1)) + s * t2;
    vec3 nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * vh;
    return normalize(vec3(a * nh.x, a * nh.y, max(nh.z, 1.0e-6)));
}

// pdf of a VNDF-sampled microfacet normal m given view w (both .z > 0).
float GgxVndfPdf(vec3 w, vec3 m, float a)
{
    return GgxG1(w, a) * max(dot(w, m), 0.0) * GgxD(m, a) / max(w.z, 1.0e-6);
}

// --------------------------------------------- rough dielectric interface -
// Local frame, +z side has etaA, -z side etaB. Directions point AWAY from
// the interface ("view" convention). Used by the rough dielectric material
// and as the layered slab's coat interface.

// Scatter: given incident view wV (either side), samples reflection or
// transmission by Fresnel choice. Returns the outgoing view direction,
// the throughput multiplier (eta-free G2/G1 - the Fresnel choice cancels;
// radiance-compression eta^2 is the CALLER's business because it cancels
// over enter/exit pairs inside the layered slab), the full sampling pdf
// (choice * VNDF * jacobian) and whether the sample crossed the interface.
bool DielScatter(vec3 wV, float a, float etaA, float etaB,
                 out vec3 wOut, out float weight, out float pdf, out bool crossed)
{
    wOut = vec3(0.0); weight = 0.0; pdf = 0.0; crossed = false;
    bool below = wV.z < 0.0;
    vec3 wl = below ? vec3(wV.xy, -wV.z) : wV; // mirror so wl.z > 0
    float etaI = below ? etaB : etaA;
    float etaT = below ? etaA : etaB;
    if (wl.z <= 1.0e-6) return false;

    vec3 m = GgxSampleVndf(wl, a, RandomFloat2());
    float dotWM = dot(wl, m);
    if (dotWM <= 1.0e-6) return false;
    float F = FresnelDielectric(dotWM, etaI, etaT);
    if (RandomFloat() < F)
    {
        vec3 wo = normalize(2.0 * dotWM * m - wl);
        if (wo.z <= 1.0e-6) return false; // below-horizon reflection: absorbed
        weight = GgxG2(wl, wo, a) / GgxG1(wl, a);
        pdf = F * GgxVndfPdf(wl, m, a) / (4.0 * dotWM);
        wOut = below ? vec3(wo.xy, -wo.z) : wo;
    }
    else
    {
        float etaRel = etaI / etaT;
        vec3 wt = refract(-wl, m, etaRel);
        if (dot(wt, wt) < 1.0e-9 || wt.z >= -1.0e-6) return false;
        wt = normalize(wt);
        float dotTM = dot(wt, m);
        float denom = etaI * dotWM + etaT * dotTM;
        denom *= denom;
        float J = etaT * etaT * abs(dotTM) / max(denom, 1.0e-9);
        weight = GgxG2(wl, wt, a) / GgxG1(wl, a);
        pdf = (1.0 - F) * GgxVndfPdf(wl, m, a) * J;
        crossed = true;
        wOut = below ? vec3(wt.xy, -wt.z) : wt; // wt.z < 0 in mirrored frame
    }
    return weight > 0.0 && pdf > 0.0;
}

// Reflection eval at the interface, both views on the +z(etaA) side or both
// mirrored by the caller. Colorless. pdf matches DielScatter's convention.
float DielEvalR(vec3 wi, vec3 wo, float a, float etaI, float etaT, out float pdf)
{
    pdf = 0.0;
    if (wi.z <= 1.0e-6 || wo.z <= 1.0e-6) return 0.0;
    vec3 m = normalize(wi + wo);
    float dotWiM = dot(wi, m);
    if (dotWiM <= 1.0e-6) return 0.0;
    float D = GgxD(m, a);
    if (D <= 0.0) return 0.0;
    float F = FresnelDielectric(dotWiM, etaI, etaT);
    pdf = F * GgxVndfPdf(wi, m, a) / (4.0 * dotWiM);
    return F * D * GgxG2(wi, wo, a) / (4.0 * wi.z * wo.z);
}

// Transmission eval (Walter 2007 eq. 21, radiance form - includes the
// etaT^2 compression): wi.z > 0 (etaI side), wo.z < 0 (etaT side). Also
// returns the pdf of sampling wo from wi via DielScatter (choice*VNDF*J).
float DielEvalT(vec3 wi, vec3 wo, float a, float etaI, float etaT, out float pdf)
{
    pdf = 0.0;
    if (wi.z <= 1.0e-6 || wo.z >= -1.0e-6) return 0.0;
    vec3 m = etaI * wi + etaT * wo;
    if (dot(m, m) < 1.0e-12) return 0.0;
    m = normalize(m);
    if (m.z < 0.0) m = -m;
    float dotWiM = dot(wi, m);
    float dotWoM = dot(wo, m);
    if (dotWiM <= 1.0e-6 || dotWoM >= 0.0) return 0.0;
    float D = GgxD(m, a);
    if (D <= 0.0) return 0.0;
    float F = FresnelDielectric(dotWiM, etaI, etaT);
    if (F >= 1.0) return 0.0;
    float G = GgxG2(wi, wo, a);
    float denom = etaI * dotWiM + etaT * dotWoM;
    denom *= denom;
    float J = etaT * etaT * abs(dotWoM) / max(denom, 1.0e-9);
    pdf = (1.0 - F) * GgxVndfPdf(wi, m, a) * J;
    return dotWiM * J / (abs(wi.z) * abs(wo.z)) * (1.0 - F) * D * G;
}

// ------------------------------------------------- Henyey-Greenstein -----
float HgPhase(float cosT, float g)
{
    float d = 1.0 + g * g - 2.0 * g * cosT;
    return (1.0 - g * g) / (4.0 * PI * d * sqrt(max(d, 1.0e-9)));
}

vec3 HgSample(vec3 w, float g)
{
    vec2 u = RandomFloat2();
    float cosT;
    if (abs(g) < 1.0e-3) cosT = 1.0 - 2.0 * u.x;
    else
    {
        float s = (1.0 - g * g) / (1.0 + g - 2.0 * g * u.x);
        cosT = clamp((1.0 + g * g - s * s) / (2.0 * g), -1.0, 1.0);
    }
    float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
    float phi = 2.0 * PI * u.y;
    mat3 B = BsdfBasis(w);
    return normalize(B * vec3(sinT * cos(phi), sinT * sin(phi), cosT));
}

// ------------------------------------------------------ conductor lobe ---
// Local frame, wi.z > 0 and wo.z > 0 required.
vec3 CondEval(vec3 f0, float a, vec3 wi, vec3 wo, out float pdfDir, out float pdfRev)
{
    pdfDir = 0.0; pdfRev = 0.0;
    if (wi.z <= 1.0e-6 || wo.z <= 1.0e-6) return vec3(0.0);
    vec3 m = normalize(wi + wo);
    float dotWiM = max(dot(wi, m), 1.0e-6);
    float D = GgxD(m, a);
    if (D <= 0.0) return vec3(0.0);
    pdfDir = GgxVndfPdf(wi, m, a) / (4.0 * dotWiM);
    pdfRev = GgxVndfPdf(wo, m, a) / (4.0 * dotWiM);
    return FresnelSchlick3(f0, dotWiM) * (D * GgxG2(wi, wo, a) / (4.0 * wi.z * wo.z));
}

// ------------------------------------------- layered base (bottom) lobe --
// Metallic-blended conductor/diffuse mixture sharing baseColor. Local
// frame, both views .z > 0. Returns f; pdf is the mixture pdf.
vec3 LayerBaseEval(Material mt, vec3 wi, vec3 wo, out float pdf)
{
    pdf = 0.0;
    if (wi.z <= 1.0e-6 || wo.z <= 1.0e-6) return vec3(0.0);
    float metal = clamp(mt.params.x, 0.0, 1.0);
    float pdfC = 0.0, pdfCRev;
    vec3 fC = (metal > 0.0)
        ? CondEval(mt.baseColor.rgb, BsdfAlpha(mt.params.y), wi, wo, pdfC, pdfCRev)
        : vec3(0.0);
    vec3 fD = mt.baseColor.rgb / PI;
    float pdfD = wo.z / PI;
    pdf = mix(pdfD, pdfC, metal);
    return mix(fD, fC, metal);
}

// Sample the base mixture; returns f of the FULL mixture and its pdf so the
// walk can apply f * cos / pdf.
vec3 LayerBaseSample(Material mt, vec3 wi, out vec3 wo, out float pdf)
{
    float metal = clamp(mt.params.x, 0.0, 1.0);
    if (RandomFloat() < metal)
    {
        vec3 m = GgxSampleVndf(wi, BsdfAlpha(mt.params.y), RandomFloat2());
        wo = normalize(2.0 * dot(wi, m) * m - wi);
    }
    else
    {
        wo = CosineSampleHemisphere(vec3(0.0, 0.0, 1.0));
    }
    if (wo.z <= 1.0e-6) { pdf = 0.0; return vec3(0.0); }
    return LayerBaseEval(mt, wi, wo, pdf);
}

// --------------------------------------------------- layered slab walk ---
// Guo et al. 2018 position-free simulation. Local frame: +z = outside on
// the query side (the slab is treated two-sided, mirrored onto whichever
// side the query hits). Depth zeta runs from 1 (coat interface) down to 0
// (opaque base interface). The walk IS standard path tracing in slab
// coordinates - free-flight distances are sampled along the ray, so no
// geometry terms and no cosine bookkeeping appear in continuation weights:
// interfaces multiply eta-free VNDF weights (G2/G1), volume scatters
// multiply the single-scattering albedo. All refraction eta^2 factors
// cancel over the enter/exit pair (air both sides), so the walk never
// tracks transport mode.
// Runtime-opaque bounce cap: always 16, but phrased so the compiler cannot
// prove it constant - a fully unrolled walk at every inlined EvalBsdf call
// site overflows the NVIDIA assembler's instruction limit in the big
// megakernels ("too many instructions").
int LayerMaxBounces() { return int(min(16u, 16u + uFrame.frameInfo.z)); }

// Backward NEE connection through the coat: given the external direction
// wiL (.z>0), force-sample the refraction branch, producing the internal
// light propagation direction dIn (.z<0), the connection factor
// fT(wi->dIn) (Walter radiance form, includes the (1-F)), and the pdf of
// dIn in solid angle (for local MIS). Returns false on a failed sample.
bool LayerConnSample(vec3 wiL, float a, float etaC, out vec3 dIn, out float fT, out float pdfConn)
{
    dIn = vec3(0.0); fT = 0.0; pdfConn = 0.0;
    vec3 m = GgxSampleVndf(wiL, a, RandomFloat2());
    float dotWM = dot(wiL, m);
    if (dotWM <= 1.0e-6) return false;
    vec3 wt = refract(-wiL, m, 1.0 / etaC);
    if (dot(wt, wt) < 1.0e-9 || wt.z >= -1.0e-6) return false;
    dIn = normalize(wt);
    float pdfT;
    fT = DielEvalT(wiL, dIn, a, 1.0, etaC, pdfT);
    pdfConn = pdfT; // full sampling pdf incl. (1-F) choice and jacobian
    return fT > 0.0 && pdfConn > 0.0;
}

// pdf that LayerConnSample(wiL) produces internal direction dIn (.z<0).
float LayerConnPdf(vec3 wiL, vec3 dIn, float a, float etaC)
{
    float pdfT;
    DielEvalT(wiL, dIn, a, 1.0, etaC, pdfT);
    return pdfT;
}

// Forward sampling walk: full simulation from external wiL (.z>0) to an
// exit direction. Returns weight = f*cos/pdf realization.
bool LayeredWalkSample(Material mt, vec3 wiL, out vec3 dirOut, out vec3 weight)
{
    dirOut = vec3(0.0, 0.0, 1.0);
    weight = vec3(0.0);
    float aC = BsdfAlpha(mt.coat.x);
    float etaC = max(mt.coat.y, 1.0001);
    float tau = max(mt.coat.z, 0.0);
    float g = clamp(mt.coat.w, -0.99, 0.99);
    vec3 medAlb = clamp(mt.coatTint.rgb, vec3(0.0), vec3(1.0));

    vec3 w; float wSc, pdfSc; bool crossed;
    if (!DielScatter(wiL, aC, 1.0, etaC, w, wSc, pdfSc, crossed)) return false;
    vec3 beta = vec3(wSc);
    if (!crossed) { dirOut = w; weight = beta; return true; } // top reflection

    float zeta = 1.0; // just inside the coat, heading down (w.z < 0)
    int maxBounce = LayerMaxBounces();
    for (int bounce = 0; bounce < maxBounce; ++bounce)
    {
        bool volume = false;
        if (tau > 0.0 && abs(w.z) > 1.0e-6)
        {
            float s = -log(max(1.0 - RandomFloat(), 1.0e-7)) / tau;
            float zNew = zeta + s * w.z;
            if (zNew > 0.0 && zNew < 1.0) { volume = true; zeta = zNew; }
            else zeta = (w.z > 0.0) ? 1.0 : 0.0;
        }
        else zeta = (w.z > 0.0) ? 1.0 : 0.0;

        if (volume)
        {
            beta *= medAlb;
            w = HgSample(w, g);
        }
        else if (zeta <= 0.0)
        {
            vec3 wo; float pdfB;
            vec3 fB = LayerBaseSample(mt, -w, wo, pdfB);
            if (pdfB <= 0.0) return false;
            beta *= fB * (wo.z / pdfB);
            w = wo; // heading up
        }
        else
        {
            if (!DielScatter(-w, aC, 1.0, etaC, w, wSc, pdfSc, crossed)) return false;
            beta *= wSc;
            if (crossed) // refracted out into air
            {
                if (w.z <= 1.0e-6) return false;
                dirOut = w;
                weight = beta;
                return true;
            }
            // else TIR / internal reflection, heading down again
        }

        if (bounce >= 3)
        {
            float q = clamp(max(beta.r, max(beta.g, beta.b)), 0.05, 1.0);
            if (RandomFloat() >= q) return false;
            beta /= q;
        }
    }
    return false;
}

// Evaluation walk: unidirectional estimator with NEE (paper 5.2.1).
// Estimates f_l(wi,wo) for both directions on the +z side: the analytic
// coat reflection lobe, plus an internal walk entered from the wo side
// whose vertices connect back to wi through the coat. NEE-only: the
// backward connection (refracting wi through a GGX interface) has full
// support over internal directions, so every configuration is reachable
// and the estimator is unbiased without the paper's local-MIS partner
// strategy. (The continuation-eval partner (Fig 7b) was implemented and
// measured, but its code inlines at every EvalBsdf call site and pushed
// the big megakernels past the NVIDIA assembler's instruction limit -
// NEE-only trades some variance on glossy coats for compilability.)
vec3 LayeredWalkEval(Material mt, vec3 wiL, vec3 woL)
{
    float aC = BsdfAlpha(mt.coat.x);
    float etaC = max(mt.coat.y, 1.0001);
    float tau = max(mt.coat.z, 0.0);
    float g = clamp(mt.coat.w, -0.99, 0.99);
    vec3 medAlb = clamp(mt.coatTint.rgb, vec3(0.0), vec3(1.0));

    float pdfTopR;
    vec3 f = vec3(DielEvalR(wiL, woL, aC, 1.0, etaC, pdfTopR));

    // Enter the slab from the wo side: force the refraction branch,
    // weighted by its true sampling probability ((1-F) folded into fT/pdf).
    vec3 mEnter = GgxSampleVndf(woL, aC, RandomFloat2());
    float dotWoM = dot(woL, mEnter);
    if (dotWoM <= 1.0e-6) return f;
    vec3 w = refract(-woL, mEnter, 1.0 / etaC);
    if (dot(w, w) < 1.0e-9 || w.z >= -1.0e-6) return f;
    w = normalize(w);
    // Forced-refraction weight: (1-F) * G2/G1 (the eta-free VNDF weight).
    float Fent = FresnelDielectric(dotWoM, 1.0, etaC);
    vec3 beta = vec3((1.0 - Fent) * GgxG2(woL, w, aC) / GgxG1(woL, aC));
    if (beta.r <= 0.0) return f;

    float zeta = 1.0;
    int maxBounce = LayerMaxBounces();
    for (int bounce = 0; bounce < maxBounce; ++bounce)
    {
        bool volume = false;
        if (tau > 0.0 && abs(w.z) > 1.0e-6)
        {
            float s = -log(max(1.0 - RandomFloat(), 1.0e-7)) / tau;
            float zNew = zeta + s * w.z;
            if (zNew > 0.0 && zNew < 1.0) { volume = true; zeta = zNew; }
            else zeta = (w.z > 0.0) ? 1.0 : 0.0;
        }
        else zeta = (w.z > 0.0) ? 1.0 : 0.0;

        if (volume)
        {
            // NEE from a volume vertex: light propagates down along dIn to
            // the vertex, scatters into -w (back along the walk). Volume
            // in-scattering has no cosine; the connection weight fT/pdfConn
            // needs the 1/|cos| stripped off the exchange (see derivation).
            vec3 dIn; float fT, pdfConn;
            if (LayerConnSample(wiL, aC, etaC, dIn, fT, pdfConn))
            {
                float ph = HgPhase(dot(dIn, -w), g);
                float Tr = exp(-tau * (1.0 - zeta) / max(-dIn.z, 1.0e-6));
                f += beta * medAlb * ph * Tr * (fT / pdfConn);
            }
            beta *= medAlb;
            w = HgSample(w, g);
        }
        else if (zeta <= 0.0)
        {
            // NEE from the base interface (the surface RE carries the
            // |cos| of the connection segment).
            vec3 dIn; float fT, pdfConn;
            if (LayerConnSample(wiL, aC, etaC, dIn, fT, pdfConn))
            {
                vec3 vConnUp = vec3(-dIn.x, -dIn.y, -dIn.z); // toward coat
                float pdfBase;
                vec3 fB = LayerBaseEval(mt, vConnUp, -w, pdfBase);
                float Tr = exp(-tau / max(vConnUp.z, 1.0e-6));
                f += beta * fB * vConnUp.z * Tr * (fT / pdfConn);
            }
            vec3 wo; float pdfB;
            vec3 fB2 = LayerBaseSample(mt, -w, wo, pdfB);
            if (pdfB <= 0.0) return f;
            beta *= fB2 * (wo.z / pdfB);
            w = wo;
        }
        else
        {
            // Walk reached the coat from inside: internal reflection
            // continues, refraction exits (contributes nothing to eval).
            float wSc, pdfSc; bool crossed; vec3 wNew;
            if (!DielScatter(-w, aC, 1.0, etaC, wNew, wSc, pdfSc, crossed)) return f;
            if (crossed) return f;
            beta *= wSc;
            w = wNew;
        }

        if (bounce >= 3)
        {
            float q = clamp(max(beta.r, max(beta.g, beta.b)), 0.05, 1.0);
            if (RandomFloat() >= q) return f;
            beta /= q;
        }
    }
    return f;
}

// Deterministic analytic pdf approximation for the layered BSDF (paper
// 5.3.2): coat reflection lobe + a roughened base lobe + a Lambertian
// floor. Only ever used for MIS weighting - never to divide estimates.
float LayeredApproxPdf(Material mt, vec3 wiL, vec3 woL)
{
    if (wiL.z <= 1.0e-6 || woL.z <= 1.0e-6) return 0.0;
    float aC = BsdfAlpha(mt.coat.x);
    float etaC = max(mt.coat.y, 1.0001);
    float F = FresnelDielectric(wiL.z, 1.0, etaC);
    vec3 m = normalize(wiL + woL);
    float dotWiM = max(dot(wiL, m), 1.0e-6);
    float pTop = GgxVndfPdf(wiL, m, aC) / (4.0 * dotWiM);
    float metal = clamp(mt.params.x, 0.0, 1.0);
    float aB = BsdfAlpha(mt.params.y);
    float aEff = min(1.0, sqrt(aB * aB + aC * aC)); // refraction-roughened base
    float pBase = GgxVndfPdf(wiL, m, aEff) / (4.0 * dotWiM);
    float pDiff = woL.z / PI;
    float p = F * pTop + (1.0 - F) * mix(pDiff, pBase, metal);
    return (p + 0.1 * pDiff) / 1.1; // constant floor keeps full support
}

// f(wi,wo) for connections and NEE, plus both direction pdfs (pdfDir =
// pdf of sampling wo given wi, pdfRev = the reverse) - the reverse pdf
// feeds the recursive MIS quantities. Delta materials return black: a
// delta lobe can never be hit by a connection. Rough dielectric evaluates
// its reflection lobe only (transmission connections are not supported -
// same as glass, the transmitted field is carried by sampled paths whose
// technique partner here reports pdf 0, keeping the MIS partition valid).
vec3 EvalBsdf(Material mat, vec3 n, vec3 wi, vec3 wo, out float pdfDir, out float pdfRev, out float cosOut)
{
    pdfDir = 0.0;
    pdfRev = 0.0;
    cosOut = 0.0;
    if (MatIsDelta(mat)) return vec3(0.0);

    int type = int(mat.params.w + 0.5);
    vec3 nf = (dot(n, wi) >= 0.0) ? n : -n;
    float cosI = dot(nf, wi);
    float cosO = dot(nf, wo);
    if (cosI <= 1e-6 || cosO <= 1e-6) return vec3(0.0); // reflection side only
    cosOut = cosO;

    if (type == 2) // conductor
    {
        mat3 B = BsdfBasis(nf);
        vec3 wiL = wi * B;
        vec3 woL = wo * B;
        return CondEval(mat.baseColor.rgb, BsdfAlpha(mat.params.y), wiL, woL, pdfDir, pdfRev);
    }
    if (type == 3) // rough dielectric, reflection lobe
    {
        mat3 B = BsdfBasis(nf);
        vec3 wiL = wi * B;
        vec3 woL = wo * B;
        float eta = max(mat.params.z, 1.0001);
        bool entering = dot(n, wi) >= 0.0;
        float etaI = entering ? 1.0 : eta;
        float etaT = entering ? eta : 1.0;
        float a = BsdfAlpha(mat.params.y);
        float fR = DielEvalR(wiL, woL, a, etaI, etaT, pdfDir);
        float pdfRevR;
        DielEvalR(woL, wiL, a, etaI, etaT, pdfRevR);
        pdfRev = pdfRevR;
        return mat.baseColor.rgb * fR;
    }
    if (type == 4) // layered (stochastic value, deterministic approx pdfs)
    {
        mat3 B = BsdfBasis(nf);
        vec3 wiL = wi * B;
        vec3 woL = wo * B;
        pdfDir = LayeredApproxPdf(mat, wiL, woL);
        pdfRev = LayeredApproxPdf(mat, woL, wiL);
        return LayeredWalkEval(mat, wiL, woL);
    }

    pdfDir = cosO / PI;
    pdfRev = cosI / PI;
    return mat.baseColor.rgb / PI;
}

struct BsdfSample
{
    vec3 dir;       // sampled wo
    vec3 weight;    // f * cosOut / pdf - the throughput multiplier (tint for delta)
    float pdfDir;   // solid-angle pdf of dir (0 marks a delta event)
    float pdfRev;   // pdf of sampling wi from dir (0 for delta)
    float cosOut;   // |cos| between the oriented normal and dir
    float choicePdf;// discrete lobe-choice probability (Fresnel F or 1-F for
                    // glass, 1 for diffuse) - enters ReSTIR random-replay
                    // Jacobians, where F changes with incident angle
    bool specular;
};

// `isLightPath` selects the transport mode: refraction compresses solid
// angle, which scales *radiance* (eye paths) by (etaI/etaT)^2 but leaves
// *importance* (light paths) untouched - the classic non-symmetric
// scattering adjoint fix (Veach 5.3.2). Getting this wrong shows up as
// glass that brightens or darkens depending on which subpath crossed it.
BsdfSample SampleBsdf(Material mat, vec3 n, vec3 wi, bool isLightPath)
{
    BsdfSample bs;
    bs.dir = vec3(0.0, 0.0, 1.0);
    bs.weight = vec3(0.0);
    bs.pdfDir = 0.0;
    bs.pdfRev = 0.0;
    bs.cosOut = 0.0;
    bs.choicePdf = 1.0;
    bs.specular = false;

    if (MatIsDelta(mat))
    {
        bs.specular = true;
        float eta = max(mat.params.z, 1.0001);
        bool entering = dot(n, wi) > 0.0;
        vec3 nf = entering ? n : -n;
        float etaI = entering ? 1.0 : eta;
        float etaT = entering ? eta : 1.0;
        float cosI = dot(nf, wi);
        if (cosI <= 1e-6) return bs;

        // Choose reflect/refract by the Fresnel reflectance itself: the
        // F (or 1-F) in the BSDF cancels against the choice probability,
        // leaving just the tint.
        float F = FresnelDielectric(cosI, etaI, etaT);
        if (RandomFloat() < F)
        {
            bs.dir = normalize(2.0 * cosI * nf - wi);
            bs.weight = mat.baseColor.rgb;
            bs.cosOut = cosI;
            bs.choicePdf = F;
        }
        else
        {
            float etaRel = etaI / etaT;
            bs.dir = normalize(refract(-wi, nf, etaRel));
            bs.weight = mat.baseColor.rgb * (isLightPath ? 1.0 : etaRel * etaRel);
            bs.cosOut = abs(dot(nf, bs.dir));
            bs.choicePdf = 1.0 - F;
        }
        return bs;
    }

    int type = int(mat.params.w + 0.5);

    if (type == 2) // GGX conductor
    {
        vec3 nf = (dot(n, wi) >= 0.0) ? n : -n;
        mat3 B = BsdfBasis(nf);
        vec3 wiL = wi * B;
        if (wiL.z <= 1e-6) return bs;
        float a = BsdfAlpha(mat.params.y);
        vec3 m = GgxSampleVndf(wiL, a, RandomFloat2());
        float dotWM = dot(wiL, m);
        if (dotWM <= 1e-6) return bs;
        vec3 woL = normalize(2.0 * dotWM * m - wiL);
        if (woL.z <= 1e-6) return bs;
        float pdfDir, pdfRev;
        vec3 fC = CondEval(mat.baseColor.rgb, a, wiL, woL, pdfDir, pdfRev);
        if (pdfDir <= 0.0) return bs;
        bs.dir = B * woL;
        bs.cosOut = woL.z;
        bs.pdfDir = pdfDir;
        bs.pdfRev = pdfRev;
        bs.weight = fC * (woL.z / pdfDir);
        return bs;
    }

    if (type == 3) // GGX rough dielectric (Walter 2007)
    {
        float eta = max(mat.params.z, 1.0001);
        bool entering = dot(n, wi) > 0.0;
        vec3 nf = entering ? n : -n;
        float etaI = entering ? 1.0 : eta;
        float etaT = entering ? eta : 1.0;
        mat3 B = BsdfBasis(nf);
        vec3 wiL = wi * B;
        if (wiL.z <= 1e-6) return bs;
        float a = BsdfAlpha(mat.params.y);
        vec3 woL; float wSc, pdfSc; bool crossed;
        if (!DielScatter(wiL, a, etaI, etaT, woL, wSc, pdfSc, crossed)) return bs;
        bs.dir = B * woL;
        bs.cosOut = abs(woL.z);
        bs.pdfDir = pdfSc;
        // eta-free VNDF weight; refraction gets the radiance-compression
        // factor on eye paths only (same adjoint convention as glass).
        float etaScale = crossed ? (isLightPath ? 1.0 : (etaI * etaI) / (etaT * etaT)) : 1.0;
        bs.weight = mat.baseColor.rgb * (wSc * etaScale);
        // reverse pdf: pdf of sampling wiL back from woL across the lobe
        if (crossed)
        {
            // reverse transmission pdf: mirror the frame so woL becomes the
            // +z-side incident view; etas swap accordingly
            float pdfRevT;
            vec3 wiRev = vec3(woL.xy, -woL.z);
            vec3 woRev = vec3(wiL.xy, -wiL.z);
            DielEvalT(wiRev, woRev, a, etaT, etaI, pdfRevT);
            bs.pdfRev = pdfRevT;
        }
        else
        {
            float pdfRevR;
            DielEvalR(woL, wiL, a, etaI, etaT, pdfRevR);
            bs.pdfRev = pdfRevR;
        }
        return bs;
    }

    if (type == 4) // layered slab (Guo et al. 2018)
    {
        vec3 nf = (dot(n, wi) >= 0.0) ? n : -n;
        mat3 B = BsdfBasis(nf);
        vec3 wiL = wi * B;
        if (wiL.z <= 1e-6) return bs;
        vec3 dirL, wgt;
        if (!LayeredWalkSample(mat, wiL, dirL, wgt)) return bs;
        bs.dir = B * dirL;
        bs.cosOut = dirL.z;
        bs.pdfDir = LayeredApproxPdf(mat, wiL, dirL);
        bs.pdfRev = LayeredApproxPdf(mat, dirL, wiL);
        bs.weight = wgt;
        return bs;
    }

    vec3 nf = (dot(n, wi) >= 0.0) ? n : -n;
    vec3 dir = CosineSampleHemisphere(nf);
    float cosO = dot(nf, dir);
    if (cosO <= 1e-6) return bs;
    bs.dir = dir;
    bs.cosOut = cosO;
    bs.pdfDir = cosO / PI;
    bs.pdfRev = max(dot(nf, wi), 0.0) / PI;
    bs.weight = mat.baseColor.rgb; // (albedo/PI) * cos / (cos/PI)
    return bs;
}

// ------------------------------------------------- octahedral packing ----
// Unit direction <-> single float (2x16 bit snorm octahedral). Used to
// cache reconnection-suffix directions in reservoir spare fields so shifts
// can re-evaluate glossy BSDFs at cached vertices.
float OctaEncode(vec3 d)
{
    vec2 p = d.xy / (abs(d.x) + abs(d.y) + abs(d.z));
    if (d.z < 0.0) p = (1.0 - abs(p.yx)) * vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return uintBitsToFloat(packSnorm2x16(p));
}
vec3 OctaDecode(float f)
{
    vec2 p = unpackSnorm2x16(floatBitsToUint(f));
    vec3 d = vec3(p.xy, 1.0 - abs(p.x) - abs(p.y));
    if (d.z < 0.0) d.xy = (1.0 - abs(d.yx)) * vec2(d.x >= 0.0 ? 1.0 : -1.0, d.y >= 0.0 ? 1.0 : -1.0);
    return normalize(d);
}

// --------------------------------------------------------------- Ray -----
struct Ray { vec3 origin; vec3 dir; vec3 invDir; };

// Occlusion tMax for a shadow/connection segment of length d. The margin
// must dominate the ray-origin's 1e-4 normal offset in ABSOLUTE terms: a
// purely relative margin (d * 0.999 = d/1000) shrinks below that offset on
// short segments, and the offset-perturbed ray then clips the TARGET's own
// surface just before the endpoint - falsely occluding every connection
// across a tight gap (black patches where surfaces nearly touch). Segments
// shorter than the floor count as visible (near-contact).
float ShadowTMax(float d) { return d * 0.999 - 2.0e-4; }

Ray MakeRay(vec3 origin, vec3 dir)
{
    Ray r;
    r.origin = origin;
    r.dir = dir;
    r.invDir = 1.0 / dir;
    return r;
}

struct Hit
{
    float t;
    vec3 normal;
    vec2 uv;
    uint materialIndex;
    uint triIndex; // original scene-triangle index (pre-BVH-permutation)
};

const float RAY_TMAX = 1.0e30;
const uint  NO_MATERIAL = 0xFFFFFFFFu;
// Sentinel material id marking a VOLUME scattering vertex (fog): the
// vertex has no BSDF/normal - scatter evaluation goes through the phase
// function and MIS measure factors use sigma_t instead of cosines.
const uint  MAT_VOLUME  = 0xFFFFFFFEu;

// Scatter evaluation at a volume vertex: f = sigma_s * phase (per solid
// angle), symmetric pdfs. wProp = propagation direction of the INCOMING
// ray (i.e. -wi in the walk convention); the contribution cosine is 1 -
// callers use FogSigmaT() as the MIS measure factor instead.
vec3 EvalPhase(vec3 wProp, vec3 wo, out float pdfDir, out float pdfRev)
{
    float p = PhaseHG(dot(wProp, wo), FogG());
    pdfDir = p;
    pdfRev = p;
    return vec3(FogSigmaS() * p);
}

float IntersectAABB(Ray ray, vec3 bmin, vec3 bmax, float tMax)
{
    vec3 t0 = (bmin - ray.origin) * ray.invDir;
    vec3 t1 = (bmax - ray.origin) * ray.invDir;
    vec3 tsmall = min(t0, t1);
    vec3 tbig = max(t0, t1);
    float tmin = max(max(tsmall.x, tsmall.y), max(tsmall.z, 0.0));
    float tmax = min(min(tbig.x, tbig.y), min(tbig.z, tMax));
    return (tmax >= tmin) ? tmin : RAY_TMAX;
}

// Moller-Trumbore. Updates `hit` in place if this is the closest hit so far.
void IntersectTriangle(Ray ray, Triangle tri, uint triIdx, inout Hit hit)
{
    vec3 v0 = tri.p0.xyz, v1 = tri.p1.xyz, v2 = tri.p2.xyz;
    vec3 e1 = v1 - v0;
    vec3 e2 = v2 - v0;
    vec3 pvec = cross(ray.dir, e2);
    float det = dot(e1, pvec);
    if (abs(det) < 1e-9) return;
    float invDet = 1.0 / det;
    vec3 tvec = ray.origin - v0;
    float u = dot(tvec, pvec) * invDet;
    if (u < 0.0 || u > 1.0) return;
    vec3 qvec = cross(tvec, e1);
    float v = dot(ray.dir, qvec) * invDet;
    if (v < 0.0 || u + v > 1.0) return;
    float t = dot(e2, qvec) * invDet;
    if (t <= 1e-4 || t >= hit.t) return;

    hit.t = t;
    float w = 1.0 - u - v;
    hit.normal = normalize(tri.n0.xyz * w + tri.n1.xyz * u + tri.n2.xyz * v);
    hit.uv = tri.uv0_uv1.xy * w + tri.uv0_uv1.zw * u + tri.uv2_material.xy * v;
    hit.materialIndex = uint(tri.uv2_material.z + 0.5);
    hit.triIndex = triIdx;
}

// Boolean Moller-Trumbore: does the ray hit `tri` with t in (1e-4, tMax)?
bool IntersectTriangleAny(Ray ray, Triangle tri, float tMax)
{
    vec3 v0 = tri.p0.xyz;
    vec3 e1 = tri.p1.xyz - v0;
    vec3 e2 = tri.p2.xyz - v0;
    vec3 pvec = cross(ray.dir, e2);
    float det = dot(e1, pvec);
    if (abs(det) < 1e-9) return false;
    float invDet = 1.0 / det;
    vec3 tvec = ray.origin - v0;
    float u = dot(tvec, pvec) * invDet;
    if (u < 0.0 || u > 1.0) return false;
    vec3 qvec = cross(tvec, e1);
    float v = dot(ray.dir, qvec) * invDet;
    if (v < 0.0 || u + v > 1.0) return false;
    float t = dot(e2, qvec) * invDet;
    return t > 1e-4 && t < tMax;
}

// Traversal stack depth. tinybvh's SAH BVH2 stays well under this for our
// scene sizes (tens of thousands of triangles => depth ~2*log2(N) < 32);
// overflowing entries are dropped (missed geometry), so bump it if scenes
// grow into the millions of triangles. Kept small deliberately: the stack
// lives in per-thread local memory and its footprint is hot cache traffic
// in every ray cast.
#define BVH_STACK_SIZE 32

// Iterative, stack-based traversal of the tinybvh "Wald" BVH2. Root is
// always node 0 (tinybvh guarantee). Internal nodes store their left child
// index in `leftFirst`; the right child is always `leftFirst + 1`. Leaf
// nodes (triCount > 0) store their first triangle-index-buffer slot in
// `leftFirst`.
bool IntersectScene(Ray ray, out Hit hit)
{
    hit.t = RAY_TMAX;
    hit.materialIndex = NO_MATERIAL;

    int stack[BVH_STACK_SIZE];
    int stackPtr = 0;
    int nodeIdx = 0;

    while (true)
    {
        BVHNode node = bvhNodes[nodeIdx];
        if (node.triCount > 0u)
        {
            for (uint i = 0u; i < node.triCount; i++)
            {
                uint triIdx = triIndices[node.leftFirst + i];
                IntersectTriangle(ray, triangles[triIdx], triIdx, hit);
            }
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
            continue;
        }

        int left = int(node.leftFirst);
        int right = left + 1;
        BVHNode leftNode = bvhNodes[left];
        BVHNode rightNode = bvhNodes[right];
        float tLeft = IntersectAABB(ray, leftNode.aabbMin, leftNode.aabbMax, hit.t);
        float tRight = IntersectAABB(ray, rightNode.aabbMin, rightNode.aabbMax, hit.t);

        if (tLeft > tRight)
        {
            float tmpT = tLeft; tLeft = tRight; tRight = tmpT;
            int tmpN = left; left = right; right = tmpN;
        }

        if (tLeft == RAY_TMAX)
        {
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
        }
        else
        {
            nodeIdx = left;
            if (tRight < RAY_TMAX && stackPtr < BVH_STACK_SIZE) stack[stackPtr++] = right;
        }
    }

    return hit.materialIndex != NO_MATERIAL;
}

// Any-hit (shadow ray) variant of IntersectScene: returns true as soon as
// anything blocks the ray before tMax. No closest-hit bookkeeping, no
// front-to-back child ordering - any intersection ends the query.
bool IntersectSceneOccluded(Ray ray, float tMax)
{
    int stack[BVH_STACK_SIZE];
    int stackPtr = 0;
    int nodeIdx = 0;

    while (true)
    {
        BVHNode node = bvhNodes[nodeIdx];
        if (IntersectAABB(ray, node.aabbMin, node.aabbMax, tMax) == RAY_TMAX)
        {
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
            continue;
        }

        if (node.triCount > 0u)
        {
            for (uint i = 0u; i < node.triCount; i++)
            {
                uint triIdx = triIndices[node.leftFirst + i];
                if (IntersectTriangleAny(ray, triangles[triIdx], tMax)) return true;
            }
            if (stackPtr == 0) break;
            nodeIdx = stack[--stackPtr];
            continue;
        }

        nodeIdx = int(node.leftFirst);
        if (stackPtr < BVH_STACK_SIZE) stack[stackPtr++] = nodeIdx + 1;
    }

    return false;
}
