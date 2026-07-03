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
// draws) is IDENTICAL to the megakernel's, so the wavefront path produces
// the same estimator, realization for realization.
//
// Bindings 16-17 exceed the 16-binding portability budget documented in
// restir_common.glsl - wavefront mode needs
// GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS >= 18 (NVIDIA exposes 96, AMD 64;
// Intel iGPUs cap at 16). PathTracer.cpp falls back to the megakernel path
// via ROYALGL_RESTIR_WAVEFRONT=0 or when the limit is too small. A second
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
// *QBase helpers). Allocated as WF_CTRL_UINTS + 6 x pixelCount uints.
layout(std430, binding = 17) restrict buffer WfQueueSSBO { uint wfQueue[]; };

const uint WF_GROUP_SIZE = 64u;
const uint WF_ARENA_VEC4_PER_PIXEL = 26u;
const uint WF_CTRL_UINTS = 128u; // 32 slots x 4 uints

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
uint WfShiftStepQBase(uint r) { return WF_CTRL_UINTS + (r & 1u) * 2u * RestirPixelCount(); }
uint WfShiftShadowQBase()     { return WF_CTRL_UINTS + 4u * RestirPixelCount(); } // 2N entries

// Round index, set by the host before each indirect dispatch.
uniform uint uWfRound;
