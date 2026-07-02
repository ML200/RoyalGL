// Shared declarations for the bidirectional path tracer's compute passes
// (bdpt_light.comp, bdpt_eye.comp, bdpt_resolve.comp). Requires common.glsl
// to be included first.
//
// The MIS scheme is the recursive per-vertex formulation of Veach's
// bidirectional weights (thesis ch. 10) in the dVCM/dVC form popularized by
// SmallVCM / "Implementing Vertex Connection and Merging" and the
// agraphicsguynotes.com BDPT MIS write-up: each subpath vertex carries two
// running quantities from which the balance-heuristic weight of ANY
// connection is O(1):
//
//   dVCM: after sampling a direction with (forward) pdf p at a vertex,
//         dVCM = 1/p; on arriving at the next vertex it picks up the
//         solid-angle->area conversion (dist^2 / cos). It represents the
//         pdf ratio of the neighboring strategy that splits the path one
//         edge closer to this subpath's origin.
//   dVC:  the recursive sum over ALL strategies further toward the origin,
//         dVC' = (cos/p) * (dVCM + pdfRev * dVC), where pdfRev is the pdf
//         of sampling the incoming direction in reverse - the factor that
//         re-expresses this vertex's generation as if the path had been
//         built from the other side.
//
// Delta (glass) vertices zero dVCM and scale dVC by cos: their reverse and
// forward delta pdfs cancel, and no strategy can connect at them, which is
// exactly what the recursion then encodes. Balance heuristic throughout,
// matching the unidirectional kernel.

// One stored light-subpath vertex (only non-delta vertices are stored -
// delta vertices cannot be connected to).
struct LightVertex
{
    vec4 posDvcm;   // xyz world position, w=dVCM
    vec4 normalDvc; // xyz shading normal (unflipped), w=dVC
    vec4 tputMat;   // xyz throughput arriving at the vertex, w=materialIndex (floatBitsToUint)
    vec4 wiLen;     // xyz direction toward the previous vertex, w=path length in segments
};
layout(std430, binding = 8) buffer LightVerticesSSBO { LightVertex lightVerts[]; };

// t=1 (light tracing) splats: fixed-point RGB accumulator per pixel,
// written with atomicAdd by bdpt_light.comp, drained into the accumulation
// image and cleared by bdpt_resolve.comp each frame. Fixed point because
// core GL has no portable float image atomics.
layout(std430, binding = 9) buffer SplatSSBO { uint splatBuf[]; };

// Number of stored vertices per light subpath.
layout(std430, binding = 11) buffer LightVertCountSSBO { uint lightVertCount[]; };

// Camera-anchored light-selection pdf per light, cached once per frame by
// bdpt_lightsel.comp (the anchor is fixed, so the value is pixel-independent).
layout(std430, binding = 12) buffer LightSelPdfSSBO { float lightSelPdf[]; };

const float BDPT_SPLAT_SCALE = 4096.0;
const uint  BDPT_MAX_LIGHT_VERTS = 8u;

// Total path length cap in segments, matching the unidirectional kernel's
// maximum (maxBounces+1) up to the vertex-storage limit.
uint BdptMaxPathLength() { return min(uFrame.frameInfo.w + 1u, BDPT_MAX_LIGHT_VERTS); }

uint BdptNumLightPaths() { return uFrame.lightInfo.z; }

// ------------------------------------------------------------ camera -----
// Image-plane distance in pixel units: with 1 sample per pixel the camera
// direction pdf w.r.t. image area is ipd^2 / cos^3(theta) (SmallVCM's
// imageToSolidAngleFactor); the same quantity drives both the eye-path MIS
// initialization and the t=1 camera connection, so they stay consistent.
//
// In lens mode this is the EFFECTIVE-PINHOLE approximation (paraxial EFL,
// cameraParams.z): the true per-path lens pdf has no closed form, but MIS
// weights only need one consistent, path-deterministic function on both
// strategies - both sides evaluate these formulas from the path's first
// scene vertex, so the partition of unity is exact and the approximation
// costs only weight optimality.
float BdptImagePlaneDist()
{
    if (uFrame.lensParams2.x > 0.5)
        return (float(uFrame.frameInfo.y) * 0.5) * (uFrame.cameraParams.z / uFrame.lensParams.y);
    return (float(uFrame.frameInfo.y) * 0.5) / uFrame.cameraParams.x;
}

// Projects a world direction leaving the camera onto integer pixel
// coordinates. Returns false if it points behind or outside the image.
bool BdptDirToPixel(vec3 dir, out ivec2 pixel, out float cosAtCam)
{
    cosAtCam = dot(uFrame.camForward.xyz, dir);
    if (cosAtCam <= 1e-6) return false;

    float tanY = uFrame.cameraParams.x;
    float aspect = uFrame.cameraParams.y;
    vec3 t = dir / cosAtCam;
    float ndcX = dot(t, uFrame.camRight.xyz) / (tanY * aspect);
    float ndcY = -dot(t, uFrame.camUp.xyz) / tanY;
    if (abs(ndcX) >= 1.0 || abs(ndcY) >= 1.0) return false;

    vec2 size = vec2(uFrame.frameInfo.xy);
    vec2 p = (vec2(ndcX, ndcY) * 0.5 + 0.5) * size;
    pixel = ivec2(p);
    pixel = clamp(pixel, ivec2(0), ivec2(size) - 1);
    return true;
}

void BdptSplat(ivec2 pixel, vec3 rgb)
{
    uint base = (uint(pixel.y) * uFrame.frameInfo.x + uint(pixel.x)) * 3u;
    atomicAdd(splatBuf[base + 0u], uint(rgb.r * BDPT_SPLAT_SCALE));
    atomicAdd(splatBuf[base + 1u], uint(rgb.g * BDPT_SPLAT_SCALE));
    atomicAdd(splatBuf[base + 2u], uint(rgb.b * BDPT_SPLAT_SCALE));
}

// ------------------------------------------------------ light sampling ----
// BDPT samples emitters through the light tree, anchored at a FIXED query
// point (the camera): the recursive MIS quantities bake the light selection
// pdf into every vertex at generation time, which is only exact when that
// pdf doesn't depend on the (yet unknown) receiving vertex - a fixed anchor
// makes the pdf a deterministic per-frame function of the light index while
// still concentrating light subpaths on emitters that matter to the view
// (the point of the tree in huge scenes).
//
// Two pdf roles are deliberately separated:
//  - the SAMPLED pdf (returned by BdptSampleLightIndex) divides the
//    strategy's own contribution - it must be the true sampling density;
//  - the EVAL pdf (BdptLightPickPdf, the deterministic tree re-descent)
//    feeds every MIS quantity on all strategies. MIS weights only need one
//    consistent function to form a partition of unity, so any residual
//    sampler/eval discrepancy costs a little weight optimality, never bias.

uint BdptSampleLightIndex(out float pickPdf)
{
    float pdfDescent, pdfLeaf;
    uint leaf = LT_Descend(uFrame.camPos.xyz, uFrame.camForward.xyz, pdfDescent);
    if (pdfDescent <= 0.0) { pickPdf = 0.0; return 0u; }
    uint lightIdx = LT_SampleLeafTriangle(leaf, pdfLeaf);
    pickPdf = pdfDescent * pdfLeaf;
    return lightIdx;
}

float BdptLightPickPdf(uint lightIdx)
{
    return lightSelPdf[lightIdx];
}

// Area pdf of s=1 light sampling landing on this light triangle, and the
// area x solid-angle product pdf of the emission sampler starting there
// with cosTheta between the light normal and the outgoing direction.
// Emitters are one-sided (the winding-defined normal), diffuse (cos/PI).
// Eval-pdf based: for MIS quantities only, never for dividing contributions.
void BdptLightPdfs(uint lightIdx, float cosTheta, out float directPdfA, out float emissionPdfW)
{
    float pickPdf = BdptLightPickPdf(lightIdx);
    float invArea = 1.0 / max(lightTris[lightIdx].normalArea.w, 1e-10);
    directPdfA = pickPdf * invArea;
    emissionPdfW = directPdfA * max(cosTheta, 0.0) / PI;
}

vec3 BdptSampleLightPoint(uint lightIdx, vec2 r)
{
    LightTri lt = lightTris[lightIdx];
    float s1 = sqrt(r.x);
    float u = 1.0 - s1;
    float v = r.y * s1;
    return (1.0 - u - v) * lt.p0.xyz + u * lt.p1.xyz + v * lt.p2.xyz;
}
