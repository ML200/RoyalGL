// Shared declarations for the ReSTIR BDPT passes (restir_gbuffer.comp,
// restir_camera.comp, restir_temporal.comp, restir_spatial.comp,
// restir_resolve.comp, restir_debug.comp). Requires common.glsl included
// first. See docs/RESTIR_BDPT_PLAN.md.
//
// Reservoirs store PATH-TECHNIQUE PAIRS (Hedstrom et al. 2025): the path is
// identified by its BDPT technique (s,t) plus the replay seed and cached
// reconnection data needed to shift it into another pixel's domain. Phase 1
// covers camera-side techniques s=0 (BSDF hit on emitter/environment) and
// s=1 (NEE); Phase 2 adds t=1 light tracing (the lightweight-BDPT technique
// set {s=0, s=1, t=1}) with caustic t=1 paths routed into the per-pixel
// caustic reservoir. Everything is integrated in SOLID ANGLE at each vertex
// (paper sec. 7), so f is a pure product of local rho*cos terms and the
// shift Jacobians are Appendix B's pdf/geometry ratios. For t=1 paths, f is
// measured in the same pixel units as eye radiance via the deterministic
// camera-connection factor (imageToSurface), exactly like the classic BDPT
// splat.
//
// Binding budget: GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS is 16 on common
// hardware (Intel), and bindings 1-14 are taken by the scene/BDPT/lens
// buffers. ReSTIR therefore uses only bindings 15 (reservoirs) and 0
// (G-buffer).
//
// Region rotation (reservoir buffer holds 3 x pixelCount entries):
//   region 2         - scratch: initial candidates, then temporal output
//   region parity    - this frame's FINAL reservoirs (resolve reads these;
//                      next frame reads them as its temporal history)
//   region 1-parity  - previous frame's final reservoirs (temporal input)
// Each pass writes to region 2 unless it is the last reuse pass, in which
// case it writes to region parity (see RestirCandidateOutRegion() etc.).

// One per-pixel path reservoir. 13 x vec4 = 208 bytes; must match
// kPathReservoirBytes in src/pathtracer/PathTracer.cpp.
//
// Convention for f and pdfs: delta (glass) scattering contributes
// choicePdf (Fresnel F or 1-F) to pdf products and choicePdf*tint to f -
// the delta distributions cancel consistently in every ratio we form.
struct PathReservoir
{
    vec4 core;    // x=W (UCW), y=pHat (omega*lum(f), current domain), z=confidence,
                  // w=tech bits (see RestirPackTech)
    vec4 fSeed;   // xyz=f RGB of the selected path (current domain), w=subpath
                  // replay seed (uint bits): camSeed for t>=2, light subpath
                  // seed for t=1
    vec4 rcInfo;  // x=|n_rc . w_in| on this path, y=dist^2(x_{r-1}, x_r),
                  // z=omega_tau (technique MIS weight, copied through shifts),
                  // w=replayPdfProd: product of sampled pdfs over the REPLAYED
                  //   scatters (vertices 1..r-2; all scatters if no rc).
                  // For t=1 the "reconnection vertex" is the path's x_1 (the
                  // light subpath end y_{s-1}, re-anchored to the V-buffer hit
                  // by shifts) and x/y describe the edge (y_{s-2}, y_{s-1});
                  // w covers the replayed light samplers (pick*area for s=2,
                  // everything up to the arrival of y_{s-2} otherwise).
    vec4 rcPosMat;// xyz=reconnection vertex x_r world position, w=materialIndex
                  // (uint bits; NO_MATERIAL for an NEE light point)
    vec4 rcNormal;// xyz=shading normal at x_r, oriented toward the valid
                  // (observer) side: a shifted predecessor x'_{r-1} must have
                  // dot(rcNormal, dirToPrev) > 0, w unused
    vec4 rcLsuf;  // xyz=L_suf: radiance leaving x_r toward x_{r-1} (direction-
                  // independent for Lambertian x_r / one-sided emitters),
                  // w unused
    vec4 reservedA; // later phases: light subpath end y_{s-1}
    vec4 reservedB;
    vec4 reservedC;
    vec4 reservedD; // later phases: recursive reconnection MIS cache
    vec4 reservedE;
    vec4 reservedF;
    vec4 reservedG;
};

// One per-pixel caustic reservoir. 4 x vec4 = 64 bytes. Phase 2 populates
// it per-frame from caustic t=1 LRM entries (light -> ... -> glass ->
// diffuse -> camera); temporal replay/re-binning lands in Phase 3. Never
// spatially reused. The final image is regular + caustic estimate.
struct CausticReservoir
{
    vec4 core;  // x=W, y=pHat, z=confidence, w=tech bits
    vec4 fSeed; // xyz=f RGB (pixel-measurement units), w=light subpath seed (uint bits)
    vec4 meta;  // x=omega_tau, y=replayed-pdf product (all sampled light
                // pdfs - caustic shifts are pure replay), z/w reserved
                // (Phase 3: subpixel landing position for the pixel filter)
    vec4 spare;
};

// Both reservoirs of one pixel in one buffer entry (272 bytes; must match
// kPixelReservoirsBytes in PathTracer.cpp). Buffer length: 3 x pixelCount.
struct PixelReservoirs
{
    PathReservoir path;
    CausticReservoir caustic;
};
layout(std430, binding = 15) buffer ReservoirsSSBO { PixelReservoirs pixelRes[]; };

// Deterministic per-pixel primary hit (V-buffer): traced through the pixel
// CENTER, no jitter, so shifts have a stable anchor. posDepth.w < 0 marks a
// miss. Buffer length: 2 x pixelCount, halves swapped by parity.
struct GBufferPixel
{
    vec4 posDepth;  // xyz world position, w=ray t (<0 = miss)
    vec4 normalMat; // xyz shading normal, w=uint bits: materialIndex<<24 | triIndex
                    // (assumes <256 materials and <16M triangles)
};
layout(std430, binding = 0) buffer GBufferSSBO { GBufferPixel gbuf[]; };

uint GBufMaterial(GBufferPixel g) { return floatBitsToUint(g.normalMat.w) >> 24; }
uint GBufTriangle(GBufferPixel g) { return floatBitsToUint(g.normalMat.w) & 0x00FFFFFFu; }
float PackGBufMatTri(uint matId, uint triIdx) { return uintBitsToFloat((matId << 24) | (triIdx & 0x00FFFFFFu)); }

uint RestirPixelIndex(ivec2 p) { return uint(p.y) * uFrame.frameInfo.x + uint(p.x); }
uint RestirPixelCount() { return uFrame.frameInfo.x * uFrame.frameInfo.y; }

// Path vertex cap; must equal BDPT_MAX_LIGHT_VERTS in bdpt_common.glsl
// (duplicated because the reuse passes don't include the BDPT buffers).
const uint RESTIR_MAX_VERTS = 8u;

// --------------------------------------------------- regions & G-buffer ---
// restirParams: x=debug view, y=flags (bit0 active, bit1 temporal reuse,
// bit2 spatial reuse, bit3 accumulate frames - see AccumulateFrames() in
// common.glsl, bit4 light tracing), z=frame counter, w=parity.
bool RestirTemporalEnabled()  { return (uFrame.restirParams.y & 2u) != 0u; }
bool RestirSpatialEnabled()   { return (uFrame.restirParams.y & 4u) != 0u; }
bool RestirLightTracingEnabled() { return (uFrame.restirParams.y & 16u) != 0u; }

uint RestirRegionOffset(uint region) { return region * RestirPixelCount(); }
uint RestirFinalRegion() { return uFrame.restirParams.w; }        // parity
uint RestirHistoryRegion() { return 1u - uFrame.restirParams.w; } // prev frame's final
const uint RESTIR_SCRATCH_REGION = 2u;

// Which region each producer writes: the LAST reuse pass in the frame must
// land in the final region.
uint RestirCandidateOutRegion()
{
    return (RestirTemporalEnabled() || RestirSpatialEnabled()) ? RESTIR_SCRATCH_REGION
                                                               : RestirFinalRegion();
}
uint RestirTemporalOutRegion()
{
    return RestirSpatialEnabled() ? RESTIR_SCRATCH_REGION : RestirFinalRegion();
}

// G-buffer halves (2 x pixelCount, parity-swapped).
uint GBufCurOffset()  { return uFrame.restirParams.w * RestirPixelCount(); }
uint GBufPrevOffset() { return (1u - uFrame.restirParams.w) * RestirPixelCount(); }

// ------------------------------------------------------ technique bits ----
// tech word: s bits 0-7 | t bits 8-15 | flags bits 16+.
// Flag bits: bit0 rcValid, bit1 envEnd, bit2 caustic (t=1 path whose
// y_{s-2} is delta), bits 4-7 rcIndex (vertex index of the reconnection
// vertex, 1-based), bits 8-15 deltaMask (bit j-1 set = path vertex x_j is a
// delta material; for t=1 the mask covers the light subpath vertices
// y_1..y_8 instead).
const uint RESTIR_FLAG_RCVALID = 1u;
const uint RESTIR_FLAG_ENVEND  = 2u;
const uint RESTIR_FLAG_CAUSTIC = 4u;

uint RestirPackTech(uint s, uint t, uint flags) { return (s & 0xFFu) | ((t & 0xFFu) << 8) | (flags << 16); }
uint RestirTechS(uint tech) { return tech & 0xFFu; }
uint RestirTechT(uint tech) { return (tech >> 8) & 0xFFu; }
uint RestirTechFlags(uint tech) { return tech >> 16; }
uint RestirFlagsRcIndex(uint flags) { return (flags >> 4) & 0xFu; }
uint RestirFlagsDeltaMask(uint flags) { return (flags >> 8) & 0xFFu; }

PathReservoir RestirEmptyReservoir()
{
    PathReservoir r;
    r.core = vec4(0.0, 0.0, 1.0, 0.0); // W=0, pHat=0, confidence=1 (paper 7.1)
    r.fSeed = vec4(0.0);
    r.rcInfo = vec4(0.0);
    r.rcPosMat = vec4(0.0);
    r.rcNormal = vec4(0.0);
    r.rcLsuf = vec4(0.0);
    r.reservedA = vec4(0.0);
    r.reservedB = vec4(0.0);
    r.reservedC = vec4(0.0);
    r.reservedD = vec4(0.0);
    r.reservedE = vec4(0.0);
    r.reservedF = vec4(0.0);
    r.reservedG = vec4(0.0);
    return r;
}

CausticReservoir RestirEmptyCaustic()
{
    CausticReservoir c;
    c.core = vec4(0.0, 0.0, 1.0, 0.0); // W=0, pHat=0, confidence=1
    c.fSeed = vec4(0.0);
    c.meta = vec4(0.0);
    c.spare = vec4(0.0);
    return c;
}

float RestirLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Small standalone xorshift for reservoir-selection draws, independent of
// the path-replay streams in g_rngState.
float RestirRand(inout uint state)
{
    state ^= (state << 13u);
    state ^= (state >> 17u);
    state ^= (state << 5u);
    return float(state) * (1.0 / 4294967296.0);
}

// ------------------------------------------------------------- cameras ----
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

// Same, through the PREVIOUS frame's camera (for backward temporal shifts).
Ray RestirPrevPrimaryRay(ivec2 pixel)
{
    vec2 ndc = (vec2(pixel) + 0.5) / vec2(uFrame.frameInfo.xy) * 2.0 - 1.0;
    float tanFovY = uFrame.prevCameraParams.x;
    float aspect = uFrame.prevCameraParams.y;
    vec3 dir = normalize(uFrame.prevCamForward.xyz
        + uFrame.prevCamRight.xyz * (ndc.x * tanFovY * aspect)
        - uFrame.prevCamUp.xyz * (ndc.y * tanFovY));
    return MakeRay(uFrame.prevCamPos.xyz, dir);
}

// Projects a world position through the PREVIOUS frame's pinhole camera to
// continuous pixel coordinates. Inverse of RestirPrevPrimaryRay; the ndcY
// negation matches BdptDirToPixel. False if behind or off-screen.
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
