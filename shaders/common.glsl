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
    vec4 baseColor; // .rgb albedo (diffuse) / tint (glass)
    vec4 emissive;  // .rgb emissive radiance
    vec4 params;    // x=metallic, y=roughness, z=ior, w=type (0=diffuse, 1=glass)
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
    uvec4 restirParams;    // x=debug view, y=ReSTIR active (0/1), z=frame counter, w=ping-pong parity
} uFrame;

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
    vec4 p0, p1, p2;     // world-space position, .w unused
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
// kernels. Two material models: two-sided Lambertian diffuse, and a delta
// dielectric ("glass") with exact Fresnel reflection/refraction.
//
// Direction convention: `wi` points from the surface toward the previous
// path vertex, `wo` toward the next one; `n` is the interpolated shading
// normal with its true geometric orientation (NOT pre-flipped - glass needs
// the sign to tell entering from exiting).

bool MatIsDelta(Material m) { return m.params.w > 0.5; }

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

// f(wi,wo) for connections and NEE, plus both direction pdfs (pdfDir =
// pdf of sampling wo given wi, pdfRev = the reverse) - the reverse pdf
// feeds the recursive MIS quantities. Delta materials return black: a
// delta lobe can never be hit by a connection.
vec3 EvalBsdf(Material mat, vec3 n, vec3 wi, vec3 wo, out float pdfDir, out float pdfRev, out float cosOut)
{
    pdfDir = 0.0;
    pdfRev = 0.0;
    cosOut = 0.0;
    if (MatIsDelta(mat)) return vec3(0.0);

    vec3 nf = (dot(n, wi) >= 0.0) ? n : -n;
    float cosI = dot(nf, wi);
    float cosO = dot(nf, wo);
    if (cosI <= 1e-6 || cosO <= 1e-6) return vec3(0.0); // reflection side only
    cosOut = cosO;
    pdfDir = cosO / PI;
    pdfRev = cosI / PI;
    return mat.baseColor.rgb / PI;
}

struct BsdfSample
{
    vec3 dir;     // sampled wo
    vec3 weight;  // f * cosOut / pdf - the throughput multiplier (tint for delta)
    float pdfDir; // solid-angle pdf of dir (0 marks a delta event)
    float pdfRev; // pdf of sampling wi from dir (0 for delta)
    float cosOut; // |cos| between the oriented normal and dir
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
        }
        else
        {
            float etaRel = etaI / etaT;
            bs.dir = normalize(refract(-wi, nf, etaRel));
            bs.weight = mat.baseColor.rgb * (isLightPath ? 1.0 : etaRel * etaRel);
            bs.cosOut = abs(dot(nf, bs.dir));
        }
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

// --------------------------------------------------------------- Ray -----
struct Ray { vec3 origin; vec3 dir; vec3 invDir; };

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

// Iterative, stack-based traversal of the tinybvh "Wald" BVH2. Root is
// always node 0 (tinybvh guarantee). Internal nodes store their left child
// index in `leftFirst`; the right child is always `leftFirst + 1`. Leaf
// nodes (triCount > 0) store their first triangle-index-buffer slot in
// `leftFirst`.
bool IntersectScene(Ray ray, out Hit hit)
{
    hit.t = RAY_TMAX;
    hit.materialIndex = NO_MATERIAL;

    int stack[64];
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
            if (tRight < RAY_TMAX && stackPtr < 64) stack[stackPtr++] = right;
        }
    }

    return hit.materialIndex != NO_MATERIAL;
}

// Any-hit (shadow ray) variant of IntersectScene: returns true as soon as
// anything blocks the ray before tMax. No closest-hit bookkeeping, no
// front-to-back child ordering - any intersection ends the query.
bool IntersectSceneOccluded(Ray ray, float tMax)
{
    int stack[64];
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
        if (stackPtr < 64) stack[stackPtr++] = nodeIdx + 1;
    }

    return false;
}
