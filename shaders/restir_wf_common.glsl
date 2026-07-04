// Wavefront ReSTIR: shared arena / queue / dispatch-control declarations
// (docs/RESTIR_BDPT_PLAN.md Phase 7). The camera RIS pass and the
// temporal/spatial replay passes are split into small kernels around every
// ray cast so the BVH traversal never shares a kernel (and therefore never
// shares registers) with the RIS / shift bookkeeping:
//
//   camera RIS:  caminit -> [camshade -> camtrace + camshadow] x maxLen
//                -> camfinal
//   shifts:      tinit/sinit -> [shiftstep -> shifttrace + shiftshadow]
//                x maxLen -> tmerge/smerge
//
// Streaming compaction: every stage appends the indices of surviving work
// items to the next stage's queue with an atomic counter; dead paths and
// completed shifts simply never re-enqueue, so later rounds shrink and no
// lane ever idles on a terminated path. Dispatches are GPU-driven
// (glDispatchComputeIndirect) - the appenders maintain the group count with
// an atomicMax, the host never reads anything back.
//
// RNG determinism: all path sampling uses the counter-based
// RngStream(vertex, purpose) streams, so a kernel can resume a path at any
// vertex from just the base seed - no RNG state crosses kernels except the
// reservoir-selection chain (risRng / spatial rng), which is persisted
// explicitly. Per pixel, the RIS candidate stream (order of RestirRand
// draws) was verified IDENTICAL to the historical megakernel's estimator,
// realization for realization, before the megakernels were removed.
//
// Bindings 16-17 exceed the 16-binding portability budget documented in
// restir_common.glsl - wavefront mode needs
// GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS >= 18 (NVIDIA exposes 96, AMD 64;
// Intel iGPUs cap at 16). PathTracer.cpp falls back to plain BDPT when the
// limit is too small. A second
// hard limit shapes this file: GL_MAX_COMPUTE_SHADER_STORAGE_BLOCKS is 16
// on NVIDIA, and the ray-tracing kernels' include graph already declares 14
// blocks - hence exactly TWO wavefront blocks (arena + queues, with the
// dispatch control folded into the queue buffer), and the non-tracing
// kernels drop the light_tree/bdpt_common includes they don't need.
//
// Requires common.glsl + restir_common.glsl included first.

// Arena: per-pixel/per-job scratch state, raw vec4s with pass-specific
// layouts (the camera pass and the shift passes never run concurrently, so
// they alias the same storage). Allocated as WF_ARENA_VEC4_PER_PIXEL x
// pixelCount vec4s.
layout(std430, binding = 16) restrict buffer WfArenaSSBO { vec4 wfArena[]; };

// Queues + dispatch control in ONE block (NVIDIA caps a compute shader at
// 16 storage blocks and the include graph already sits at the limit).
// The first WF_CTRL_UINTS uints are 32 dispatch-control entries of 16 bytes
// each: (groupsX, 1, 1, count) - the first 12 bytes are the
// glDispatchComputeIndirect args (the host binds this buffer as
// GL_DISPATCH_INDIRECT_BUFFER; byte offset = slot * 16), count is the
// appender's atomic cursor. Cleared by the host to the pattern (0,1,1,0)
// per pass. The work-item id queues follow, in fixed regions (see the
// *QBase helpers). Allocated as WF_CTRL_UINTS + 3 x WF_JOBS_PER_PIXEL x
// pixelCount uints (step ping-pong + shadow, sized for the batched
// spatial jobs; the camera queues fit inside the first regions).
layout(std430, binding = 17) restrict buffer WfQueueSSBO { uint wfQueue[]; };

const uint WF_GROUP_SIZE = 64u;
// Spatial candidate batch: modes >= 1 trace up to WF_SPATIAL_BATCH forward
// candidates through ONE shift-pipeline sweep (plus the sweep-0 backward
// job in slot WF_SPATIAL_BATCH), so the per-sweep fixed cost (maxLen step/
// trace/shadow dispatches and barriers) amortizes over the batch instead
// of repeating per candidate. Job capacity: WF_SPATIAL_BATCH + 1 per pixel.
const uint WF_SPATIAL_BATCH = 4u;
const uint WF_JOBS_PER_PIXEL = WF_SPATIAL_BATCH + 1u;
const uint WF_ARENA_VEC4_PER_PIXEL = 65u;
const uint WF_CTRL_UINTS = 128u; // 32 slots x 4 uints
// Probe-guided selection (ROYALGL_RESTIR_PSEL): probed source runs per
// 16x16 block per frame. The actual probe count is RestirSearchIters(),
// clamped to this capacity (2 table vec4s per probe, 2*capacity per
// block in region [64N, 65N)).
const uint WF_PSEL_MAX_PROBES = 16u;

// Arena region [62N, 63N): one persistent vec4 per pixel, OUTSIDE the
// camera pass's idx*26 strided layout and the shift passes' header/job/sort
// regions ([0, 62N)), so nothing clobbers it between frames.
//   .xyz = marginalized RGB shading estimate (ReSTIR PT Enhanced sec. 6.3:
//          the sum of VECTOR resampling weights m_i*F(Y_i)*W_i*J_i over the
//          spatial mixture; frame-local - zeroed by sinit round 0, filled
//          by smerge, consumed by resolve)
//   .w   = sample duplication score D in [0,1] (sec. 5: fraction of the
//          17x17 neighborhood sharing this pixel's replay seed; written by
//          restir_wf_dupmap.comp at END of frame, read by the NEXT frame's
//          temporal merge to adaptively lower the confidence cap)
uint WfShadeAccSlot(uint idx) { return 62u * RestirPixelCount() + idx; }

// Arena region [27N, 28N): frame-persistent per-pixel learning state for
// the shift-aware spatial candidate selection (spatial mode 2, "ours").
//   .x = shift-success score EMA in (0, 1]: how well paths shift between
//        this pixel and its cluster, learned online from the realized
//        survival ratios pHat(T(X))*|J| / pHat(X) of the shifts this
//        pixel REQUESTS (backward z' shift = its own sample going out,
//        forward candidate shifts = neighbor samples coming in; shift
//        failure is approximately symmetric, so both signals estimate the
//        same score). 0 = unlearned (buffer cleared on resize) -> treat
//        as 1. Selection only: any positive floor keeps P(i) > 0 where
//        contributions exist, so learning errors NEVER bias the estimate.
//   .y = disocclusion flag: 1 if this frame's temporal pass found no
//        usable history for the pixel (written by restir_wf_tinit.comp;
//        stale when temporal reuse is off). Read back by the host for
//        the ROYALGL_STATS_MASK disocclusion-restricted noise stats.
//   .zw = reserved for future learned selection features
uint WfShiftScoreSlot(uint idx) { return 63u * RestirPixelCount() + idx; }

// ---- Probe-guided selection (PSEL) table, arena region [64N, 65N) -------
// Per sort BLOCK, once per frame: up to WF_PSEL_MAX_PROBES probed source
// runs with their measured ideal-weight estimates. Entry r of block b
// occupies two vec4s at WfPselSlot(b, r):
//   A.x = uintBits(runPix + 1 | count << 21)   (0 = empty entry)
//   A.y = w^_r  (probe-measured S_r * rho* * m*; pfin writes, psel seeds
//                with -1 = probe pending)
//   A.z = cSigma_r (run confidence mass, unscaled)
//   A.w = S_r     (run selection mass)
//   B.x = uintBits(cluster key of the run)
//   B.y = uintBits(representative source entry z* + 1)
//   B.z = pHat(X*) of the representative (source-domain target value)
//   B.w = unused
// The block linear index uses a jitter-safe grid one block wider per axis
// (matches the host's psel/pfin dispatch math).
uint WfPselBlocksX()  { return uFrame.frameInfo.x / 16u + 2u; }
uint WfPselBlocksY()  { return uFrame.frameInfo.y / 16u + 2u; }
uint WfPselBlockLinear(ivec2 blockCoord)
{
    return uint(blockCoord.y) * WfPselBlocksX() + uint(blockCoord.x);
}
uint WfPselSlot(uint blockLinear, uint r)
{
    return 64u * RestirPixelCount() + (blockLinear * WF_PSEL_MAX_PROBES + r) * 2u;
}

// The pixel's SPMIS reuse run for this frame: the index of a pixel whose
// sort slot DESCRIBES the run (its own, or the cell-search winner stored
// by sinit round 0 into the learning region's .z as uintBits(pix + 1)).
uint WfSpmisRunPixel(uint idx)
{
    uint enc = floatBitsToUint(wfArena[WfShiftScoreSlot(idx)].z);
    return (enc == 0u) ? idx : enc - 1u;
}

// Read side (restir_wf_ssort.comp): sanitized score with a positivity
// floor. The floor bounds the worst-case 1/P inflation of the stochastic
// MIS weight for mispredicted-but-contributing candidates.
// History-guided selection policy: .x holds an EMA of this pixel's
// REALIZED OUTGOING shifted contribution pHat(T(X))*|J|*W, observed on
// the canonical backward shift each frame - a direct sample of the ideal
// selection density m*pHat(Y)*W*|J| that SPMIS approximates with source
// luminance. Reprojected with temporal history, reset on disocclusion
// (restir_wf_tinit.comp). 0 = unlearned.
float WfShiftScore(uint idx)
{
    float v = wfArena[WfShiftScoreSlot(idx)].x;
    return (isnan(v) || v < 0.0) ? 0.0 : v;
}

// Write side (restir_wf_smerge.comp, own pixel only - no races).
void WfShiftScoreUpdate(uint idx, float o)
{
    if (isnan(o) || isinf(o) || o < 0.0) return;
    float v = wfArena[WfShiftScoreSlot(idx)].x;
    if (isnan(v) || v <= 0.0) v = o;
    wfArena[WfShiftScoreSlot(idx)].x = mix(v, o, RestirScoreEmaRate());
}

// Adaptive temporal confidence cap (ReSTIR PT Enhanced eq. in sec. 5):
// cCap' = lerp(cCap, cCapMin=1, D^alpha) with alpha = 0.1. Trades a small,
// bounded bias for a large reduction in correlation blobs/streaks.
float WfDecorrCap(uint prevIdx)
{
    float cap = RestirConfidenceCap();
    if (!RestirDecorrEnabled()) return cap;
    float D = wfArena[WfShadeAccSlot(prevIdx)].w;
    if (isnan(D)) D = 0.0;
    return mix(cap, 1.0, pow(clamp(D, 0.0, 1.0), 0.1));
}

// Ctrl slot map (shared by both pass families; they are cleared between):
//   slot r        : shade round r (camera: r = 1..maxLen) /
//                   step round r (shifts: r = 0..maxLen-1)
//   slot 10 + r   : extend/trace queue of round r
//   slot 20 + r   : shadow queue of round r
uint WfCtrlShade(uint r)  { return r; }
uint WfCtrlExtend(uint r) { return 10u + r; }
uint WfCtrlShadow(uint r) { return 20u + r; }

uint WfCount(uint slot) { return wfQueue[slot * 4u + 3u]; }

// Queue append. Per-thread atomics measured indistinguishable from a
// subgroup-aggregated variant at 1600x900 (the ctrl cachelines are not the
// bottleneck; state traffic and round barriers are) - kept simple.
void WfAppend(uint slot, uint qbase, uint item)
{
    uint at = atomicAdd(wfQueue[slot * 4u + 3u], 1u);
    wfQueue[qbase + at] = item;
    atomicMax(wfQueue[slot * 4u], at / WF_GROUP_SIZE + 1u);
}

// Queue regions. Camera: the shade queue ping-pongs by round parity (round
// r's consumers run while round r+1's entries are appended); the extend and
// shadow queues are produced and consumed within one round, between
// barriers. Shifts: same shape with job ids (2 per pixel).
uint WfCamShadeQBase(uint r)  { return WF_CTRL_UINTS + (r & 1u) * RestirPixelCount(); }
uint WfCamExtendQBase()       { return WF_CTRL_UINTS + 2u * RestirPixelCount(); }
uint WfCamShadowQBase()       { return WF_CTRL_UINTS + 3u * RestirPixelCount(); } // 2N entries
uint WfShiftStepQBase(uint r) { return WF_CTRL_UINTS + (r & 1u) * WF_JOBS_PER_PIXEL * RestirPixelCount(); }
uint WfShiftShadowQBase()     { return WF_CTRL_UINTS + 2u * WF_JOBS_PER_PIXEL * RestirPixelCount(); }

// Round index, set by the host before each indirect dispatch.
uniform uint uWfRound;
// Spatial pass index (iterated spatial reuse; 0 unless the host loops the
// spatial pass family - defaults to 0 for every other kernel).
uniform uint uWfPass;

// ---------------- histogram-stratified spatial reuse (Salaün 2025) --------
// "Histogram Stratification for Spatio-Temporal Reservoir Sampling",
// SIGGRAPH '25. The image is tiled into 16x16 blocks (grid origin jittered
// per frame to break up block artifacts, sec. 5.3); restir_wf_ssort.comp
// sorts each block's post-temporal candidates by (cluster key, target
// luminance) - the sorted list IS the inverse CDF of the block's luminance
// histogram (Heitz & Belcour 2019 equivalence), and the cluster key
// (primary-hit instance + material) is the paper's object-id spatial mask.
// sinit then draws stratified ANTITHETIC ranks from the pixel's cluster run
// instead of uniform disk neighbors.
//
// The per-pixel sort output lives in the spare arena region [61N, 62N)
// (the shift-pass layout uses header [0,N) + jobs [N,61N)). Rank-indexed
// components live at the slot of the block's rank-th valid pixel (scanline
// order); pixel-indexed components at the pixel's own slot:
//   .x = uintBits(pixel index at this pixel's rank slot)  - rank r of a
//        block maps to the block's r-th valid pixel in scanline order, so
//        the rank->pixel permutation is stored distributed over the
//        block's own pixels                                 [rank-indexed]
//   .y = uintBits(run start | run length << 16) of this pixel's cluster
//        run (length 0 = pixel has no candidate list)      [pixel-indexed]
//   .z = run confidence sum: sum of min(c_i, cap) over the run's pixels
//        (the SPMIS c_Sigma; modes >= 1 only)              [pixel-indexed]
//   .w = inclusive segmented CDF of the SPMIS selection weights
//        s_i = min(c_i,cap) * pHat(X_i) * W_i, reset per cluster run
//        (modes >= 1 only; mode 2 swaps in shift-aware weights)
//                                                           [rank-indexed]
const int WF_SORT_BLOCK = 16;
uint WfSortSlotBase() { return 61u * RestirPixelCount(); }

uint WfSortPackRun(uint start, uint count) { return start | (count << 16u); }
uint WfSortRunStart(uint runPacked) { return runPacked & 0xFFFFu; }
uint WfSortRunCount(uint runPacked) { return runPacked >> 16u; }

ivec2 WfSortGridOffset()
{
    uint h = WangHash(uFrame.restirParams.z * 0x9E3779B9u + 0x85EBCA6Bu);
    return ivec2(int(h & 15u), int((h >> 4u) & 15u));
}

// Valid-pixel rectangle of a block: block-local coords [lo, lo+sz) of the
// pixels inside the image (blocks at the jittered grid's border are
// partial; sz components can be <= 0 for fully-outside blocks).
void WfSortBlockRect(ivec2 blockCoord, out ivec2 origin, out ivec2 lo, out ivec2 sz)
{
    origin = blockCoord * WF_SORT_BLOCK - WfSortGridOffset();
    ivec2 size = ivec2(uFrame.frameInfo.xy);
    lo = max(-origin, ivec2(0));
    sz = min(ivec2(WF_SORT_BLOCK), size - origin) - lo;
}

ivec2 WfSortBlockOf(ivec2 pixel)
{
    return (pixel + WfSortGridOffset()) / WF_SORT_BLOCK;
}

// Deterministic probe position i for a block (value-independent: hashed
// from frame + block only). Growing radius from the block center with a
// golden-angle direction sweep - the same reach schedule as the cell
// search so env sweeps stay comparable.
ivec2 WfPselProbePos(ivec2 blockCoord, uint i)
{
    ivec2 origin, lo, sz;
    WfSortBlockRect(blockCoord, origin, lo, sz);
    vec2 center = vec2(origin + lo) + 0.5 * vec2(sz);
    uint h = WangHash(uFrame.restirParams.z * 24593u
                      + WfPselBlockLinear(blockCoord) * 769u + 0x51ED270Bu);
    float phi0 = 6.2831853 * (float(h & 0xFFFFu) / 65536.0);
    float phi = phi0 + 2.3999632 * float(i); // golden angle
    float radius = RestirSearchRadius() * pow(1.25, float(i));
    vec2 p = center + radius * vec2(cos(phi), sin(phi));
    return ivec2(p);
}

// The block-center pixel index (the probe receiver representative):
// center of the block's VALID rectangle, so border blocks stay in-image.
uint WfPselReceiver(ivec2 blockCoord)
{
    ivec2 origin, lo, sz;
    WfSortBlockRect(blockCoord, origin, lo, sz);
    ivec2 p = origin + lo + max(sz / 2 - 1, ivec2(0));
    return RestirPixelIndex(clamp(p, ivec2(0), ivec2(uFrame.frameInfo.xy) - 1));
}

// Cluster key of a G-buffer pixel - must match restir_wf_ssort.comp's
// sort key exactly (primary-hit instance + material + quantized normal
// octant; one dedicated fog cluster for miss pixels under a scattering
// medium; invalid otherwise).
uint WfClusterKey(GBufferPixel g)
{
    bool isMiss = g.posDepth.w < 0.0;
    if (isMiss && !(FogScatterEnabled() && RestirVolShiftEnabled()))
        return 0xFFFFFFFFu; // not in any candidate list
    if (isMiss) return 0xFFFFFF00u;
    uint key = (InstanceOfTriangle(GBufTriangle(g)) << 8u) | GBufMaterial(g);
    // Paper Sec. 5.1 keys cells on objectID + QUANTIZED NORMAL: split
    // same-object clusters by orientation so curved geometry reuses among
    // like-oriented pixels (reconnection shifts survive far better between
    // aligned anchors). 3 sign bits = octant of the world-space shading
    // normal; bits 24-26 stay clear of the fog key's low byte pattern.
    if (RestirClusterNormalEnabled())
    {
        uint oct = (g.normalMat.x < 0.0 ? 1u : 0u)
                 | (g.normalMat.y < 0.0 ? 2u : 0u)
                 | (g.normalMat.z < 0.0 ? 4u : 0u);
        key |= oct << 24u;
    }
    return key;
}

// rank -> arena slot of the rank-indexed components (.x permutation and
// .w segmented CDF live at the slot of the block's rank-th valid pixel).
uint WfSortRankSlot(ivec2 origin, ivec2 lo, ivec2 sz, uint rank)
{
    ivec2 slotLocal = lo + ivec2(int(rank) % sz.x, int(rank) / sz.x);
    return WfSortSlotBase() + RestirPixelIndex(origin + slotLocal);
}

// rank -> pixel index, through the distributed permutation slots.
uint WfSortRankPixel(ivec2 origin, ivec2 lo, ivec2 sz, uint rank)
{
    return floatBitsToUint(wfArena[WfSortRankSlot(origin, lo, sz, rank)].x);
}

// rank -> inclusive segmented-CDF value (SPMIS selection weights).
float WfSortRankCdf(ivec2 origin, ivec2 lo, ivec2 sz, uint rank)
{
    return wfArena[WfSortRankSlot(origin, lo, sz, rank)].w;
}
