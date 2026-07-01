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
    vec4 baseColor; // .rgb albedo
    vec4 emissive;  // .rgb emissive radiance
    vec4 params;    // x=metallic, y=roughness
};

layout(std140, binding = 0) uniform FrameUBO
{
    vec4 camPos;       // xyz
    vec4 camForward;   // xyz
    vec4 camRight;     // xyz
    vec4 camUp;        // xyz
    vec4 lensParams;   // pinhole mode: x=tanHalfFovY, y=aspect
                        // lens mode:    x=tanHalfFovY, y=aspect, z=sensorWidthMm, w=lensModeFlag(0/1)
    vec4 background;   // .rgb sky color, .a intensity
    uvec4 frameInfo;   // x=width, y=height, z=sampleIndex, w=maxBounces
    vec4 renderParams; // x=exposure
} uFrame;

layout(std430, binding = 1) readonly buffer BVHNodesSSBO   { BVHNode bvhNodes[]; };
layout(std430, binding = 2) readonly buffer TriIndicesSSBO { uint triIndices[]; };
layout(std430, binding = 3) readonly buffer TrianglesSSBO  { Triangle triangles[]; };
layout(std430, binding = 4) readonly buffer MaterialsSSBO  { Material materials[]; };

// One lens surface (see src/optics/LensSystem.h / src/gfx/GPUTypes.h
// GPULensSurface for the authoritative field-by-field description).
// `surfaces` is ordered front element (index 0) -> sensor-side (index
// N-1); the primary (eye) ray tracer walks it back-to-front, the flare/
// ghost light tracer walks it front-to-back.
struct LensSurface
{
    vec4 geometry;     // x=signed radius mm, y=thickness-to-next mm, z=semiDiameter mm, w=isAperture(0/1)
    vec4 iorRGB_z;      // xyz=IOR(R,G,B) of the medium after this surface; w=cumulative axial position (mm) from the sensor
    vec4 coatingRGB;    // xyz=per-channel Fresnel-reflectance multiplier from AR coating (1,1,1=none)
    vec4 apertureData;  // x=bladeCount(0=circular), y=bladeRotationRad, z=apertureRadiusMm (aperture row only)
};
layout(std430, binding = 5) readonly buffer LensSurfacesSSBO { LensSurface lensSurfaces[]; };

// One emissive triangle, used only by the flare/ghost light-tracing pass
// (shaders/lens_flare.comp).
struct LightTriangle
{
    vec4 p0, p1, p2;    // world-space position, .w unused
    vec4 normal_area;   // xyz geometric normal, w=triangle area
    vec4 emissive;      // .rgb radiance
};
layout(std430, binding = 6) readonly buffer LightsSSBO   { LightTriangle lights[]; };
layout(std430, binding = 7) readonly buffer LightCDFSSBO { float lightCDF[]; }; // power-proportional CDF, normalized to [0,1]

// One flare/ghost splat record: written by lens_flare.comp, read by
// lens_flare_splat.vert.
struct Splat
{
    vec4 pixelRadianceRG; // x=pixelX, y=pixelY, z=radiance.r, w=radiance.g
    vec4 radianceBValid;  // x=radiance.b, y=validFlag(0/1)
};
layout(std430, binding = 8) buffer SplatBufferSSBO  { Splat splats[]; };
layout(std430, binding = 9) buffer SplatCounterSSBO { uint splatCount; };

// ---------------------------------------------------------------- RNG ----
// xorshift32, seeded per-invocation with a Wang hash. Not cryptographically
// anything - just decorrelated enough for path tracing. Shared by every
// compute shader in this project (each has its own g_rngState instance,
// since GLSL globals are per-invocation, not actually shared state).
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
