// Shared declarations for the ReSTIR BDPT passes (restir_gbuffer.comp,
// restir_debug.comp, and the candidate/reuse passes of later phases).
// Requires common.glsl to be included first. See docs/RESTIR_BDPT_PLAN.md.
//
// Reservoirs store PATH-TECHNIQUE PAIRS (Hedstrom et al. 2025): the path is
// identified by its BDPT technique (s,t) plus the replay seeds and cached
// reconnection data needed to shift it into another pixel's domain.
//
// Binding budget: GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS is 16 on common
// hardware (Intel), and bindings 1-14 are taken by the scene/BDPT/lens
// buffers. ReSTIR therefore uses only bindings 15 (reservoirs) and 0
// (G-buffer), and ping-pongs WITHIN each buffer: the front/back halves swap
// roles every frame, selected by the parity in uFrame.restirParams.w
// (current = parity * pixelCount, previous = the other half).

// One per-pixel path reservoir (regular, i.e. everything except caustic
// t<=1 paths). 13 x vec4 = 208 bytes.
struct PathReservoir
{
    vec4 core;        // x=W (UCW), y=pHat (omega_tau * lum(f)), z=confidence,
                      // w=techFlags (uint bits: s | t<<8 | flags<<16)
    vec4 fSeed;       // xyz=f RGB contribution of the selected path, w=camSeed (uint bits)
    vec4 lightMeta;   // x=lightSeed (uint bits), y=lightPathIdx (uint bits),
                      // z=light selection pdf terms, w=rcIndex (uint bits)
    vec4 rcPosMat;    // reconnection vertex x_r: xyz position, w=materialIndex (uint bits)
    vec4 rcNormal;    // xyz shading normal at x_r
    vec4 rcWoPdf;     // xyz outgoing direction on the base path, w=forward pdf
    vec4 tputToRc;    // rgb camera-prefix throughput up to x_r
    vec4 tputAfterRc; // rgb suffix contribution arriving at x_r via rcWo
    vec4 lyPosMat;    // light subpath end y_{s-1}: xyz position, w=materialIndex (uint bits)
    vec4 lyNormalLen; // xyz shading normal at y_{s-1}, w=s (light vertex count)
    vec4 lyTput;      // rgb light-side throughput at y_{s-1}
    vec4 misA;        // recursive reconnection MIS cache (paper sec. 6.2):
                      // x=dP_r, y=dVC_r, z=geomRatio (eq. 46), w=gammaBar (eq. 42)
    vec4 misB;        // x=lambdaVC (eq. 43), y=lambdaP (eq. 44), z=sigmaBar (eq. 45),
                      // w=omega_tau of the base path
};

// One per-pixel caustic reservoir (t<=1 paths whose secondary hit is
// non-rough): pure random replay, so only the light subpath seed and the
// landing subpixel are needed. 4 x vec4 = 64 bytes.
struct CausticReservoir
{
    vec4 core;    // x=W, y=pHat, z=confidence, w=flags (uint bits)
    vec4 fSeed;   // xyz=f RGB contribution, w=lightSeed (uint bits)
    vec4 meta;    // x=s (light vertex count), y=omega_tau, zw=landing subpixel
    vec4 spare;   // reserved
};

// Both reservoirs of one pixel, in one buffer so a single binding serves
// them. 272 bytes; must match kPixelReservoirsBytes in
// src/pathtracer/PathTracer.cpp. Buffer length is 2 * pixelCount (front and
// back halves for the frame ping-pong).
struct PixelReservoirs
{
    PathReservoir path;
    CausticReservoir caustic;
};
layout(std430, binding = 15) buffer ReservoirsSSBO { PixelReservoirs pixelRes[]; };

// Deterministic per-pixel primary hit (V-buffer): traced through the pixel
// CENTER, no jitter, so temporal shifts and the t<=1 rejection heuristic
// have a stable anchor. posDepth.w < 0 marks a miss. Buffer length is
// 2 * pixelCount, ping-ponged like the reservoirs.
struct GBufferPixel
{
    vec4 posDepth;  // xyz world position, w=ray t (<0 = miss)
    vec4 normalMat; // xyz shading normal, w=materialIndex (uint bits)
};
layout(std430, binding = 0) buffer GBufferSSBO { GBufferPixel gbuf[]; };

uint RestirPixelIndex(ivec2 p) { return uint(p.y) * uFrame.frameInfo.x + uint(p.x); }
uint RestirPixelCount() { return uFrame.frameInfo.x * uFrame.frameInfo.y; }

// Front/back half selection for the intra-buffer ping-pong.
uint RestirCurOffset()  { return uFrame.restirParams.w * RestirPixelCount(); }
uint RestirPrevOffset() { return (1u - uFrame.restirParams.w) * RestirPixelCount(); }

// Technique packing in PathReservoir.core.w.
uint RestirPackTech(uint s, uint t, uint flags) { return (s & 0xFFu) | ((t & 0xFFu) << 8) | (flags << 16); }
uint RestirTechS(uint tech) { return tech & 0xFFu; }
uint RestirTechT(uint tech) { return (tech >> 8) & 0xFFu; }

// Deterministic primary ray through the pixel center - the pinhole formula
// from bdpt_eye.comp minus the jitter.
Ray RestirPrimaryRay(ivec2 pixel)
{
    vec2 ndc = (vec2(pixel) + 0.5) / vec2(uFrame.frameInfo.xy) * 2.0 - 1.0;
    float tanFovY = uFrame.cameraParams.x;
    float aspect = uFrame.cameraParams.y;
    vec3 dir = normalize(uFrame.camForward.xyz
        + uFrame.camRight.xyz * (ndc.x * tanFovY * aspect)
        - uFrame.camUp.xyz * (ndc.y * tanFovY));
    return MakeRay(uFrame.camPos.xyz, dir);
}

// Projects a world position through the PREVIOUS frame's pinhole camera to
// continuous pixel coordinates. Inverse of the primary-ray formula above;
// the ndcY negation matches BdptDirToPixel. False if behind or off-screen.
bool RestirPrevProject(vec3 worldPos, out vec2 pixel)
{
    vec3 v = worldPos - uFrame.prevCamPos.xyz;
    float z = dot(v, uFrame.prevCamForward.xyz);
    if (z <= 1e-6) return false;
    float tanY = uFrame.prevCameraParams.x;
    float aspect = uFrame.prevCameraParams.y;
    float ndcX = dot(v, uFrame.prevCamRight.xyz) / (z * tanY * aspect);
    float ndcY = -dot(v, uFrame.prevCamUp.xyz) / (z * tanY);
    if (abs(ndcX) >= 1.0 || abs(ndcY) >= 1.0) return false;
    pixel = (vec2(ndcX, ndcY) * 0.5 + 0.5) * vec2(uFrame.frameInfo.xy);
    return true;
}
