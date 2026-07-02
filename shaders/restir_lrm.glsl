// Light Reservoir Map (LRM) for the ReSTIR BDPT t=1 strategy (paper
// Alg. 1; docs/RESTIR_BDPT_PLAN.md Phase 2). restir_light.comp bins one
// candidate reservoir per successful light-vertex -> camera connection into
// the pixel the connection lands in; restir_camera.comp merges each pixel's
// entries into that pixel's path reservoir (non-caustic) or caustic
// reservoir.
//
// Structure: per-pixel LINKED LIST via atomic head exchange - the plan's
// sanctioned fallback (sec. 3) for the paper's count+prefix-sum+scatter
// sort. Correctness-equivalent, worse traversal coherence; revisit in
// Phase 6. Bindings 13/14 belong to the lens buffers everywhere else, but
// no ReSTIR pass includes lens_common.glsl and ReSTIR mode never dispatches
// the lens shaders (pinhole only), so reclaiming them stays inside the
// 16-binding budget documented in restir_common.glsl.
//
// Requires common.glsl + restir_common.glsl included first.

struct LrmEntry
{
    vec4 fW;     // xyz = f of the path in pixel-measurement units (includes
                 // the deterministic imageToSurface camera factor),
                 // w = RIS weight m * pHat / p = omega * lum(f) / (p * N_L)
                 // (paper Eq. 26: m = 1/N_L for t<=1 candidates)
    vec4 rcInfo; // becomes the winning reservoir's rcInfo verbatim (see
                 // restir_common.glsl): x = cos at y_{s-1} toward y_{s-2},
                 // y = dist^2(y_{s-2}, y_{s-1}), z = omega_tau,
                 // w = replayed-pdf product
    uvec4 meta;  // x = tech bits (RestirPackTech), y = light subpath base
                 // seed, z = next entry index (LRM_SENTINEL ends the list),
                 // w = extra payload: unused for light-pass entries, history
                 // confidence (float bits) for caustic re-bin entries
};
layout(std430, binding = 13) buffer LrmEntriesSSBO { LrmEntry lrmEntries[]; };

// [0] = entry allocator, [1 + pixelIndex] = list head. Cleared by
// PathTracer.cpp (allocator to 0, heads to LRM_SENTINEL) before each
// restir_light dispatch, and again before restir_caustic_shift.comp, which
// reuses the same structure to re-bin shifted caustic history reservoirs
// (safe: restir_camera.comp has consumed the light-pass entries by then).
layout(std430, binding = 14) buffer LrmHeadsSSBO { uint lrmHeads[]; };

const uint LRM_SENTINEL = 0xFFFFFFFFu;

// Entries per light path; must match kMaxLrmEntriesPerPath in
// PathTracer.cpp. A light subpath makes at most maxLen-1 <= 7 camera
// connections, so the buffer cannot overflow; the guard in LrmAppend is
// pure paranoia.
uint LrmCapacity() { return 8u * uFrame.lightInfo.z; }

void LrmAppend(uint pixelIdx, vec4 fW, vec4 rcInfo, uint tech, uint seed, uint extra)
{
    uint slot = atomicAdd(lrmHeads[0], 1u);
    if (slot >= LrmCapacity()) return;
    uint prev = atomicExchange(lrmHeads[1u + pixelIdx], slot);
    LrmEntry e;
    e.fW = fW;
    e.rcInfo = rcInfo;
    e.meta = uvec4(tech, seed, prev, extra);
    lrmEntries[slot] = e;
}
