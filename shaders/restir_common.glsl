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

// One per-pixel path reservoir. 10 x vec4 = 160 bytes; must match
// kPixelReservoirsBytes in src/pathtracer/PathTracer.cpp.
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
    vec4 rcPosMat;// xyz=reconnection vertex x_r position in its instance's
                  // OBJECT SPACE (converted with the current matrices on
                  // load, so it tracks a moving instance), w=materialIndex
                  // (uint bits; NO_MATERIAL for an NEE light point)
    vec4 rcNormal;// xyz=OBJECT-SPACE shading normal at x_r, oriented toward
                  // the valid (observer) side: a shifted predecessor
                  // x'_{r-1} must have dot(worldNormal, dirToPrev) > 0,
                  // w=uintBits(instance id; >= instInfo.x = static)
    vec4 rcLsuf;  // xyz=L_suf: radiance leaving x_r toward x_{r-1} with the
                  // rc vertex's OWN BSDF value rho_r divided out (shifts
                  // re-evaluate it live between the new arrival direction
                  // and the cached suffix direction, so glossy rc lobes
                  // stay exact; emitter / NEE-light-point rc vertices have
                  // no BSDF and cache plain Le). w = octa-packed OBJECT-
                  // SPACE suffix direction at x_r (unused for emitter rc).
                  // For s>=2 paths with an interior reconnection vertex
                  // this covers the connection edge and the whole light
                  // subpath (the suffix is fixed).
    vec4 lyPosMat;// s>=2 LIGHTRC paths (camera prefix has no connectable
                  // pair): xyz = light subpath end y_{s-1} position in its
                  // instance's OBJECT SPACE, w=uintBits(y_{s-1} material)
    vec4 lyNormal;// xyz = OBJECT-SPACE shading normal at y_{s-1}, oriented
                  // toward the camera-side vertex (valid observers),
                  // w=uintBits(instance id)
    vec4 lyTput;  // xyz = fLightNum: every light-side factor of f except
                  // the connection geometry term AND the ly BSDF rho_y
                  // (shifts re-evaluate rho_y toward the new camera vertex
                  // - glossy ly lobes change with the connection
                  // direction), w = octa-packed OBJECT-SPACE incoming
                  // direction at y_{s-1}
    vec4 misCache;// Recursive reconnection MIS cache (Phase 5): the shifted
                  // path's technique-MIS denominator is affine in the
                  // prefix state at the reconnection vertex AND in the rc
                  // vertex's own crossing pdfs pFwd_r = p(suffix|arrival),
                  // pRev_r = p(arrival|suffix) - both re-evaluated by the
                  // shift (EvalBsdf at x_r), so glossy rc materials stay
                  // consistent. With X' = dVCM'_r + pRev_r * dVC'_r
                  // (lightweight set: pRev_r * dT1'_r):
                  //   interior rc (r < t-1): (A, B, C, -):
                  //     denom' = A + (B + C * X') / pFwd_r
                  //   rc at the candidate vertex (r == t-1, s >= 1):
                  //     (A, B, C, -): denom' = A + B * pFwd_r + C * X'
                  //   rc = terminal emitter (s=0, r=t-1): (1, directPdfA,
                  //     emissionPdfW, -): denom' = x + y*dVCM'_r + z*dVC'_r
                  //   rc = NEE light point (rcPosMat.w==NO_MATERIAL):
                  //     (-, directPdfA, emissionPdfW, -): full split-vertex
                  //     recompute at x'_{t-1}
                  //   LIGHTRC: (dVCM_y, dVC_y, -, -): the shift folds in
                  //     its own lightRevPdfW' at the new direction
                  // Pure-replay paths recompute omega during replay and
                  // cache nothing. Eval pdfs come from RestirLightPdfs (the
                  // frame-independent power CDF), so omega is a
                  // reproducible function of the path in every frame.
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

// Both reservoirs of one pixel in one buffer entry (224 bytes; must match
// kPixelReservoirsBytes in PathTracer.cpp). Buffer length: 3 x pixelCount.
struct PixelReservoirs
{
    PathReservoir path;
    CausticReservoir caustic;
};
layout(std430, binding = 15) restrict buffer ReservoirsSSBO { PixelReservoirs pixelRes[]; };

// Deterministic per-pixel primary hit (V-buffer): traced through the pixel
// CENTER, no jitter, so shifts have a stable anchor. posDepth.w < 0 marks a
// miss. Buffer length: 2 x pixelCount, halves swapped by parity.
struct GBufferPixel
{
    vec4 posDepth;  // xyz world position, w=ray t (<0 = miss)
    vec4 normalMat; // xyz shading normal, w=uint bits: materialIndex<<24 | triIndex
                    // (assumes <256 materials and <16M triangles)
};
layout(std430, binding = 0) restrict buffer GBufferSSBO { GBufferPixel gbuf[]; };

uint GBufMaterial(GBufferPixel g) { return floatBitsToUint(g.normalMat.w) >> 24; }
uint GBufTriangle(GBufferPixel g) { return floatBitsToUint(g.normalMat.w) & 0x00FFFFFFu; }
float PackGBufMatTri(uint matId, uint triIdx) { return uintBitsToFloat((matId << 24) | (triIdx & 0x00FFFFFFu)); }

// ------------------------------------- object-space surface storage ------
// Frame-persistent surface data (both G-buffer halves, reservoir
// reconnection vertices, cached light-subpath ends, NEE light points) is
// stored in instance OBJECT SPACE and converted with the CURRENT instance
// matrices on load. The same stored bytes therefore track a moving object:
// the previous G-buffer half read next frame reconstructs the surface
// point where the object is NOW, and reconnections target the vertex ON
// the moved instance instead of a phantom at its old placement (the
// "giga-brightening" transient). Static scenes round-trip exactly
// (identity matrices are uploaded until something moves). Instance ids >=
// instInfo.x are treated as static world space (also the >16-instance
// fallback: the host uploads count 0).
vec3 RestirObjToWorldPoint(uint inst, vec3 p)
{
    if (inst >= uFrame.instInfo.x) return p;
    return vec3(uFrame.instToWorld[inst] * vec4(p, 1.0));
}
vec3 RestirObjToWorldNormal(uint inst, vec3 n)
{
    if (inst >= uFrame.instInfo.x) return n;
    // Rigid + uniform scale (SceneInstance TRS): normalize instead of the
    // inverse transpose.
    return normalize(mat3(uFrame.instToWorld[inst]) * n);
}
vec3 RestirWorldToObjPoint(uint inst, vec3 p)
{
    if (inst >= uFrame.instInfo.x) return p;
    return vec3(uFrame.instToObject[inst] * vec4(p, 1.0));
}
vec3 RestirWorldToObjNormal(uint inst, vec3 n)
{
    if (inst >= uFrame.instInfo.x) return n;
    return normalize(mat3(uFrame.instToObject[inst]) * n);
}

// G-buffer load: positions/normals are STORED in object space
// (restir_gbuffer.comp) and converted here with the current matrices -
// use this instead of raw gbuf[] indexing everywhere.
GBufferPixel RestirLoadGBuf(uint slot)
{
    GBufferPixel g = gbuf[slot];
    if (g.posDepth.w >= 0.0)
    {
        uint inst = InstanceOfTriangle(GBufTriangle(g));
        g.posDepth.xyz = RestirObjToWorldPoint(inst, g.posDepth.xyz);
        g.normalMat.xyz = RestirObjToWorldNormal(inst, g.normalMat.xyz);
    }
    return g;
}

uint RestirPixelIndex(ivec2 p) { return uint(p.y) * uFrame.frameInfo.x + uint(p.x); }
uint RestirPixelCount() { return uFrame.frameInfo.x * uFrame.frameInfo.y; }

// Path vertex cap; must equal BDPT_MAX_LIGHT_VERTS in bdpt_common.glsl
// (duplicated because the reuse passes don't include the BDPT buffers).
const uint RESTIR_MAX_VERTS = 8u;

// --------------------------------------------------- regions & G-buffer ---
// restirParams: x=debug view, y=flags (bit0 active, bit1 temporal reuse,
// bit2 spatial reuse, bit3 accumulate frames - see AccumulateFrames() in
// common.glsl, bit4 light tracing, bit5 s>=2 vertex connections),
// z=frame counter, w=parity.
bool RestirTemporalEnabled()  { return (uFrame.restirParams.y & 2u) != 0u; }
bool RestirSpatialEnabled()   { return (uFrame.restirParams.y & 4u) != 0u; }
bool RestirLightTracingEnabled() { return (uFrame.restirParams.y & 16u) != 0u; }
bool RestirConnectionsEnabled()  { return (uFrame.restirParams.y & 32u) != 0u; }
// Phase 5: recompute omega_tau for shifted paths (unbiased) instead of
// copying it from the base path (paper Sec. 6.4's bounded darkening).
bool RestirMisRecomputeEnabled() { return (uFrame.restirParams.y & 64u) != 0u; }
// Off (ROYALGL_RESTIR_STRAT=0): uniform random spatial candidate picks
// instead of the antithetic stratified pattern - A/B isolation of the
// Salaün 2025 stratification benefit; the estimator is unbiased either way.
bool RestirStratifiedEnabled()   { return (uFrame.restirParams.y & 128u) != 0u; }
// Duplication-map temporal decorrelation (ReSTIR PT Enhanced sec. 5).
// Introduces a small bias in correlated regions; ROYALGL_RESTIR_DECORR=0
// restores exact unbiasedness for soaks.
bool RestirDecorrEnabled()       { return (uFrame.restirParams.y & 256u) != 0u; }
// Volumetric shift modes (ROYALGL_RESTIR_VOLMODE): VolRc = volume vertices
// qualify as reconnection anchors (mode 2, ours); VolShift = volume paths
// are shiftable at all via distance replay + classification masks (modes
// 1-2). With both off (mode 0), a path containing any volume vertex fails
// every shift - the naive "ReSTIR PT ported to a medium" baseline. All
// modes are unbiased; they differ in reuse efficiency.
bool RestirVolRcEnabled()        { return (uFrame.restirParams.y & 512u) != 0u; }
bool RestirVolShiftEnabled()     { return (uFrame.restirParams.y & 1024u) != 0u; }
// Fog-parallax temporal pairing (ROYALGL_RESTIR_FOGPAIR, default on):
// when the surface-anchor validation fails (disocclusion/silhouette under
// camera motion), re-pair history by reprojecting the representative
// in-scatter depth and reuse the airlight family only. Off = fog history
// dies with the surface anchor (ablation baseline).
bool RestirFogPairingEnabled()   { return (uFrame.restirParams.y & 2048u) != 0u; }
// Spatial candidate selection & MIS family (ROYALGL_RESTIR_SPMODE, carried
// in the free prevCameraParams.z): 0 = pair mixture over rank-selected
// candidates (linear over rounds), 1 = SPMIS baseline (Hedstrom et al.
// 2026: with-replacement draws proportional to c*pHat(X)*W from the
// cluster-run CDF, one reservoir chain with stochastic pairwise MIS),
// 2 = history-guided (SPMIS weights x learned shift-survival ratio;
//     ablation arm - measured a wash on our scenes, see the research log).
uint RestirSpatialMode()         { return uint(uFrame.prevCameraParams.z + 0.5); }
// Mode 2 ablation (ROYALGL_RESTIR_VSCORE=0): drop the learned
// shift-survival factor - mode 2 then degenerates to exactly mode 1.
bool RestirShiftScoreEnabled()   { return (uFrame.restirParams.y & 4096u) != 0u; }
// SPMIS cell search (ROYALGL_RESTIR_CELLSEARCH, modes >= 1): growing-radius
// WRS over probed pixels' cluster runs, weighted by run confidence mass
// (Hedstrom et al. 2026 Sec. 5.1). Off = own-block run only.
bool RestirCellSearchEnabled()   { return (uFrame.restirParams.y & 8192u) != 0u; }
// Cell-search shape (ROYALGL_RESTIR_SEARCH_ITERS/_RADIUS), packed in the
// upper flag bits by PathTracer: probe count and initial radius in pixels.
uint  RestirSearchIters()        { return (uFrame.restirParams.y >> 16) & 31u; }
float RestirSearchRadius()       { return float((uFrame.restirParams.y >> 21) & 255u); }
// Starvation gate (ROYALGL_RESTIR_SEARCH_GATE): 0 = the paper's
// unconditional probe, 1 = probe only near-empty runs (count <= 2),
// 2 = additionally probe on the tinit no-usable-history flag, 3 = probe
// zero-selection-mass runs with probes weighted by THEIR selection mass -
// BIASED ablation arm (see restir_wf_sinit.comp for the branch-accounting
// argument), never a default. Measured: unconditional probing replaces
// close partners with distant ones and costs 20-45% whole-image noise
// under motion at steady state; even wide fresh disocclusion strips reuse
// better among themselves than from distant high-confidence runs, so only
// genuinely partnerless pixels should ever search.
uint RestirSearchGateMode()      { return (uFrame.restirParams.y >> 29) & 3u; }
// Mode-2 history-guided selection knobs (ROYALGL_RESTIR_EMA/_DEFMIX).
float RestirScoreEmaRate()       { return uFrame.spmisParams.x; }
float RestirScoreDefMix()        { return uFrame.spmisParams.y; }
// Quantized-normal cluster term (ROYALGL_RESTIR_CLUSTER_NORMAL): key
// cluster runs on the normal octant in addition to instance+material
// (paper Sec. 5.1 faithfulness; affects ALL spatial modes' clustering).
bool RestirClusterNormalEnabled(){ return (uFrame.restirParams.y & 0x80000000u) != 0u; }
// Dual-direction shift diagnostic (ROYALGL_RESTIR_SHIFTDIAG, mode 1,
// temporal off, K >= 5): round 1 re-uses the idle backward job slot to
// run the FORWARD shift of the SAME (canonical, z') pair round 0 shifted
// backward, and smerge tallies support/value asymmetries between the two
// directions into the learn region (never the estimator). Sub-modes pick
// the two accumulated quantities (see restir_wf_smerge.comp).
uint RestirShiftDiagMode()       { return uint(uFrame.spmisParams.z + 0.5); }
// Diagnostic involutive pair distance in pixels (0 = own-run pairs).
float RestirShiftDiagDist()      { return uFrame.spmisParams.w; }
// Probe-guided selection (ROYALGL_RESTIR_PSEL, mode 1): per-block probe
// shifts measure the ideal selection weight w^_r = S_r * rho* * m* per
// (target block, source run); candidates then draw from the union of the
// own and probed runs through a two-level CDF whose EXACT mixed
// probability rides jb.w - value dependence lives inside the compensated
// per-draw P, membership is value-independent (no gate-3 trap). Replaces
// the cell search while active. (Bit 16384 previously belonged to the
// removed two-stage experiment.)
bool RestirPselEnabled()         { return (uFrame.restirParams.y & 16384u) != 0u; }

// ---------------------------------------------- MIS eval light pdfs ------
// EVAL pick pdf for every ReSTIR MIS quantity: the power-proportional CDF
// (binding 10) - camera- and frame-INDEPENDENT, unlike the camera-anchored
// tree pdf classic BDPT evals with (bdpt_common.glsl). Frame independence
// makes the recomputed omega_tau (and hence the target pHat) a reproducible
// function of the path, which the temporal MIS partition needs; the
// sampled-vs-eval discrepancy costs only weight optimality, never bias.
// True SAMPLED pick pdfs (BdptSampleLightIndex) still divide contributions
// and drive replay Jacobians.
float RestirLightPickPdf(uint lightIdx)
{
    float lo = (lightIdx > 0u) ? lightCdf[lightIdx - 1u] : 0.0;
    return max(lightCdf[lightIdx] - lo, 1e-12);
}

void RestirLightPdfs(uint lightIdx, float cosTheta, out float directPdfA, out float emissionPdfW)
{
    float invArea = 1.0 / max(lightTris[lightIdx].normalArea.w, 1e-10);
    directPdfA = RestirLightPickPdf(lightIdx) * invArea;
    emissionPdfW = directPdfA * max(cosTheta, 0.0) / PI;
}

// SAMPLED light pick for ReSTIR light subpaths: binary search of the same
// power CDF, ONE uniform draw. The classic camera-anchored tree descent
// (BdptSampleLightIndex) is better importance sampling per frame, but its
// replay is anchored at the DESTINATION camera: under camera motion the
// same seed descends to a different light, turning every caustic-class /
// airlight history replay into an unrelated path (rejected by the J-guard
// or accepted with an arbitrary ratio) - reservoirs churn exactly where
// fog is brightest. The power CDF is camera- and frame-independent, so
// replay reproduces the pick under ANY motion, and the sampled pdf now
// EQUALS the MIS eval pdf (RestirLightPdfs). Camera-side NEE keeps the
// tree: its light points are stored, never re-picked by shifts.
uint RestirSampleLightIndex(out float pickPdf)
{
    uint n = uFrame.lightInfo.x;
    float u = RandomFloat();
    uint lo = 0u, hi = n - 1u;
    while (lo < hi)
    {
        uint mid = (lo + hi) >> 1;
        if (lightCdf[mid] < u) lo = mid + 1u; else hi = mid;
    }
    pickPdf = RestirLightPickPdf(lo);
    return lo;
}

// Confidence (M) cap for every reuse pass, user-adjustable (RenderSettings
// ::restirConfidenceCap, uploaded in prevCamPos.w - the prev-camera vec4 w
// components are otherwise unused). Bounds how many frames a temporal
// chain effectively accumulates: higher = more reuse but longer-lived
// correlation (a stale/outlier sample needs ~cap frames to wash out).
float RestirConfidenceCap() { return max(uFrame.prevCamPos.w, 1.0); }

// Temporal reuse fades with the primary vertex's glossiness. At a glossy
// x_1 the target-function ratio between different lobe samples is huge, so
// a history reservoir keeps winning the merge far longer than the
// confidence cap suggests ("sticky" reservoirs): each pixel locks onto one
// lobe sample and glossy reflections render as hard-edged partitions
// instead of lobe averages. That is correlation, not bias - the mean is
// right but finite-sample images look wrong. Scaling history confidence by
// roughness restores per-pixel turnover where the lobe is sharp and keeps
// full reuse on rough/diffuse surfaces.
float RestirReuseRoughFade(uint matId)
{
    Material m = materials[matId];
    int t = int(m.params.w + 0.5);
    float rough = 1.0;                  // diffuse / emissive / delta glass
                                        // (delta replays are exact - the
                                        // lobe-partition issue is a ROUGH
                                        // lobe phenomenon)
    if (t == 2 || t == 3) rough = m.params.y;
    else if (t == 4) rough = m.coat.x;  // the top interface shapes the view lobe
    return clamp(rough * (1.0 / 0.35), 0.15, 1.0);
}

// Symmetric support restriction on shift Jacobians: a shift whose |dT/dX|
// falls outside [1/K, K] is treated as undefined. Unbiased - the forward
// and inverse Jacobians are reciprocals, so both evaluations of a merge
// (candidate weight and MIS backward term) restrict the SAME shift map
// consistently. This kills the grazing-angle explosions where the
// reconnection ratio (cos'/cos)*(d^2/d'^2) produces rare huge resampling
// weights that then persist for ~confidence-cap frames as bright blotches.
const float RESTIR_MAX_SHIFT_JACOBIAN = 10.0;
bool RestirJacobianValid(float J)
{
    return J > (1.0 / RESTIR_MAX_SHIFT_JACOBIAN) && J < RESTIR_MAX_SHIFT_JACOBIAN;
}

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
// tech word: s bits 0-3 | t bits 4-7 | flags bits 8+ (s, t <= 8 = the
// path-length cap, so 4 bits each; the freed byte carries the volume mask).
// Flag bits: bit0 rcValid, bit1 envEnd, bit2 caustic (t=1 path whose
// y_{s-2} is delta OR whose y_{s-1} is a volume vertex - both are pure-
// replay free-landing classes), bits 4-7 rcIndex (vertex index of the
// reconnection vertex, 1-based), bits 8-15 deltaMask (bit j-1 set = path
// vertex x_j is a delta material; for t=1 the mask covers the light
// subpath vertices y_1..y_8 instead), bits 16-23 volMask (bit j-1 set =
// vertex j is a VOLUME scattering vertex - same indexing as deltaMask;
// replay invertibility requires the offset path to reproduce the
// scatter/pass classification exactly).
const uint RESTIR_FLAG_RCVALID = 1u;
const uint RESTIR_FLAG_ENVEND  = 2u;
const uint RESTIR_FLAG_CAUSTIC = 4u;
// s>=2 path whose camera prefix has no connectable pair: the shift replays
// the prefix to x'_{t-1} and re-evaluates the connection edge to the cached
// light subpath end (lyPosMat/lyNormal/lyTput).
const uint RESTIR_FLAG_LIGHTRC = 8u;

uint RestirPackTech(uint s, uint t, uint flags) { return (s & 0xFu) | ((t & 0xFu) << 4) | (flags << 8); }
uint RestirTechS(uint tech) { return tech & 0xFu; }
uint RestirTechT(uint tech) { return (tech >> 4) & 0xFu; }
uint RestirTechFlags(uint tech) { return tech >> 8; }
uint RestirFlagsRcIndex(uint flags) { return (flags >> 4) & 0xFu; }
uint RestirFlagsDeltaMask(uint flags) { return (flags >> 8) & 0xFFu; }
uint RestirFlagsVolMask(uint flags) { return (flags >> 16) & 0xFFu; }

PathReservoir RestirEmptyReservoir()
{
    PathReservoir r;
    r.core = vec4(0.0, 0.0, 1.0, 0.0); // W=0, pHat=0, confidence=1 (paper 7.1)
    r.fSeed = vec4(0.0);
    r.rcInfo = vec4(0.0);
    r.rcPosMat = vec4(0.0);
    r.rcNormal = vec4(0.0);
    r.rcLsuf = vec4(0.0);
    r.lyPosMat = vec4(0.0);
    r.lyNormal = vec4(0.0);
    r.lyTput = vec4(0.0);
    r.misCache = vec4(0.0);
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

// ------------ footprint-based reconnection criteria (ReSTIR PT Enhanced,
// sec. 4). A pair (x_{k-1}, x_k) qualifies as the reconnection pair when
// both ray footprints clear a fixed fraction of the pixel's PRIMARY ray
// footprint (eq. 5; scene-independent, adapts to distance and roughness):
//   fwd: 1/(p_{k-1}(w) G(x_{k-1}->x_k)) >= T   (area footprint at x_k)
//   inv: 1/(p_k(w') G(x_k->x_{k-1}))    >= T   (reverse footprint;
//        evaluated with the GGX peak-pdf proxy p_max ~ 1/(pi a^2) and
//        cos ~ 1, i.e. d^2 pi a^2 >= T, so it is known before the scatter
//        at x_k is sampled; skipped for diffuse/emissive x_k - their
//        continuation pdf is arrival-independent, paper footnote 6)
// plus a single-vertex roughness threshold alpha_{k-1} >= 0.2, folded into
// the callers' prevConnectable state. Replaces the historical
// material-class-only criterion (which reconnected at ANY non-delta pair,
// hurting glossy reuse and corner-adjacent shifts).
const float RESTIR_RC_FOOT_C = 2e-4; // c/100 with the paper's c = 0.02
const float RESTIR_RC_ALPHA_MIN = 0.2;

// (c/100) x primary footprint area ||x0-x1||^2 / (<n1,d>/(4pi)).
float RestirRcFootThreshold(float primaryDist, float cosPrimary)
{
    return RESTIR_RC_FOOT_C * primaryDist * primaryDist * 4.0 * PI
           / max(cosPrimary, 1e-3);
}

// dist2/pdfEdge/cosAtK describe the pair edge: pdfEdge is the solid-angle
// pdf of the edge direction at x_{k-1} (sampled at creation, evaluated at
// reconnection), cosAtK = |n_k . edge|.
bool RestirRcFootprintOk(float dist2, float pdfEdge, float cosAtK,
                         Material rcMat, bool rcEmissive, float T)
{
    if (pdfEdge <= 0.0) return false;
    if (dist2 < T * pdfEdge * cosAtK) return false; // fwd footprint
    if (!rcEmissive)
    {
        int mt = int(rcMat.params.w + 0.5);
        if (mt == 2 || mt == 3) // glossy x_k: inverse footprint (proxy)
        {
            float a = rcMat.params.y;
            if (dist2 * PI * a * a < T) return false;
        }
    }
    return true;
}

// Volume analog (our volumetric shift): a VOLUME vertex as the
// reconnection anchor x_k. No cosine at x_k (cosAtK = 1); the inverse
// footprint uses the HG peak pdf - for isotropic-to-moderate g the phase
// lobe is broad, making volume vertices excellent anchors (this is what
// the volume-anchored shift exploits).
bool RestirRcFootprintOkVolume(float dist2, float pdfEdge, float T)
{
    if (!RestirVolRcEnabled()) return false;
    if (pdfEdge <= 0.0) return false;
    if (dist2 < T * pdfEdge) return false;             // fwd footprint
    if (dist2 < T * PhaseHGPeak(FogG())) return false; // inverse (peak proxy)
    return true;
}

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
