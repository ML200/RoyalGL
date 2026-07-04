// Wavefront camera RIS pass: per-pixel path-state layout and the deferred
// candidate absorb (docs/RESTIR_BDPT_PLAN.md Phase 7). The megakernel
// restir_camera.comp is split at every ray boundary:
//
//   caminit    seeds per-pixel state at the V-buffer hit, empties the
//              output reservoir, enqueues shade round 1
//   camshade k absorbs round k-1's shadow-tested candidates into the RIS
//              stream (deferred draws - same RestirRand order as the
//              megakernel), advances to vertex k from the trace result,
//              runs the vertex logic (rc detection, s=0 emitter, NEE and
//              LVC candidate CREATION incl. their MIS weights), samples the
//              BSDF and enqueues the extend/shadow jobs + shade round k+1
//   camtrace   closest-hit for the sampled bounce (ray + BVH stack only)
//   camshadow  occlusion test per candidate; zeroes the slot's RIS weight
//   camfinal   LRM merge + caustic reservoir + W finalization
//
// The candidate acceptance draw is deferred one round (visibility must be
// known first), but the per-pixel draw ORDER is exactly the megakernel's:
// [k-1: NEE, CONN] then [k: emitter | env], with occluded candidates
// consuming no draw - so the wavefront estimator equals the megakernel's
// realization for realization.
//
// Requires common.glsl + light_tree.glsl + bdpt_common.glsl +
// restir_common.glsl + restir_wf_common.glsl included first.

// ------------------------------------------------- per-pixel state map ----
// base = idx * 26 vec4s inside wfArena:
//  0 posMat   xyz current vertex position, w uintBits(mat<<24|tri)
//  1 nrmFlags xyz shading normal, w uintBits: bit0 rcFound,
//             bit1 prevConnectable, bits 8-15 deltaMask, bits 16-19 rcIdx,
//             bits 20-23 rcInst
//  2 wiPdf    xyz wi (toward the previous vertex; the pending bounce ray
//             direction is -wi), w pdfPrevArrival
//  3 fCurProd xyz fCur, w pProd
//  4 misA     x dVCM, y dVC, z dT1, w wSum
//  5 misB     x misKA, y misKB, z uintBits(risRng), w uintBits(camSeed)
//  6 rcPos    xyz reconnection vertex, w uintBits(rcMat)
//  7 rcNrm    xyz oriented rc normal, w replayPdfBase
//  8 fArrive  xyz suffix-cache divisor: fCur at rc arrival, times rho_r
//             once the rc scatter executes (rcLsuf excludes the rc BSDF -
//             shifts re-evaluate it), w rcBaseCos
//  9 rcExt    x rcBaseDist2, y octa(object-space suffix dir at x_r, set by
//             the rc scatter), z misKC, w dvcmB
// 10 hitA     xyz hit position, w hit t (<0 = miss)   [camtrace writes]
// 11 hitB     xyz hit normal, w uintBits(mat<<24|tri) [camtrace writes]
// 12..18 candidate slot 0 (NEE), 19..25 candidate slot 1 (LVC connection)
uint WfCamBase(uint idx) { return idx * 26u; }

const uint WF_CAM_CND0 = 12u;
const uint WF_CAM_CND_STRIDE = 7u;

// Candidate slot layout (+off from the slot base):
//  +0 fCand.xyz, w = RIS weight (0 = empty; camshadow zeroes it when the
//     connection is occluded; the absorb zeroes it after consuming)
//  +1 x omega_tau, y techBits (floatBits), z pProd snapshot (rcInfo.w for
//     the light-point / LIGHTRC cases), w uintBits(kind)
//  +2 misCache: verbatim SEL.misCache for SHARED / NEE_LIGHT; for LIGHTRC
//     yzw carry lyTput (x unused)
//  +3 shadowDir.xyz, w = surface distance (occlusion tMax = ShadowTMax(dist))
//  +4 NEE_LIGHT: sampled light point.xyz (exact), w uintBits(lightIdx)
//     LIGHTRC:   oriented lyNormal.xyz, w uintBits(lyMat<<8 | lyInst)
//  +5 NEE_LIGHT: x cosAtLight, y dist2 (exact); LIGHTRC: lyPos.xyz (exact)
//  +6 SHARED_RC at-rc (r==t-1): xyz precomputed rcLsuf (candidate's own
//     rc BSDF stripped), w octa(object-space suffix dir = connection dir)
//     LIGHTRC: x dVCM_ly, y dVC_ly, z octa(object-space ly incoming dir)
const uint WF_CND_EMPTY     = 0u;
const uint WF_CND_SHARED_RC = 1u; // reconnection data = shared rc block
const uint WF_CND_NEE_LIGHT = 2u; // rc vertex = the sampled NEE light point
const uint WF_CND_LIGHTRC   = 3u; // s>=2 without a camera rc pair

// state.nrmFlags.w bits
const uint WF_CAMF_RCFOUND  = 1u;
const uint WF_CAMF_PREVCONN = 2u;
uint WfCamFlagsDeltaMask(uint f) { return (f >> 8) & 0xFFu; }
uint WfCamFlagsRcIdx(uint f) { return (f >> 16) & 0xFu; }
uint WfCamFlagsRcInst(uint f) { return (f >> 20) & 0xFu; }
uint WfCamFlagsVolMask(uint f) { return (f >> 24) & 0xFFu; }

// Shade queue entries: pixel index | absorb-only flag in the top bit (the
// path died at the BSDF sample but round k's candidates still await their
// visibility results).
const uint WF_SHADE_ABSORB_ONLY = 0x80000000u;

vec3 SafeDiv(vec3 num, vec3 den)
{
    return vec3(den.x > 0.0 ? num.x / den.x : 0.0,
                den.y > 0.0 ? num.y / den.y : 0.0,
                den.z > 0.0 ? num.z / den.z : 0.0);
}

// Absorbs one shadow-tested candidate slot into the pixel's RIS stream:
// adds its weight, runs the (deferred) reservoir draw, and on a win writes
// the winner's fields into the output reservoir - byte-identical to the
// megakernel's inline writes. Mirrors restir_camera.comp; see the field
// comments there.
void WfCamAbsorb(uint base, uint cnd, uint outSlot, inout float wSum, inout uint risRng)
{
    uint cb = base + WF_CAM_CND0 + cnd * WF_CAM_CND_STRIDE;
    vec4 fw = wfArena[cb];
    float w = fw.w;
    if (w <= 0.0 || isnan(w) || isinf(w)) return;
    wfArena[cb].w = 0.0; // consumed
    wSum += w;
    if (RestirRand(risRng) * wSum > w) return;

    vec4 meta = wfArena[cb + 1u];
    uint kind = floatBitsToUint(meta.w);
    vec3 fCand = fw.xyz;
    float omega = meta.x;

    pixelRes[outSlot].path.core.w = meta.y; // techBits
    pixelRes[outSlot].path.core.y = omega * RestirLum(fCand);
    pixelRes[outSlot].path.fSeed = vec4(fCand, wfArena[base + 5u].w); // camSeed

    if (kind == WF_CND_SHARED_RC)
    {
        // rc vertex is stored in OBJECT space + instance id (state slots
        // 6/7 + flags bits 20-23, see restir_common.glsl's object-space
        // storage notes).
        vec4 rcPos = wfArena[base + 6u];
        vec4 rcNrm = wfArena[base + 7u];
        vec4 fArr = wfArena[base + 8u];
        vec4 rcExt = wfArena[base + 9u];
        uint rcInst = WfCamFlagsRcInst(floatBitsToUint(wfArena[base + 1u].w));
        uint tech = floatBitsToUint(meta.y);
        uint rr = RestirFlagsRcIndex(RestirTechFlags(tech));
        uint tt = RestirTechT(tech);
        pixelRes[outSlot].path.rcInfo = vec4(fArr.w, rcExt.x, omega, rcNrm.w);
        pixelRes[outSlot].path.rcPosMat = rcPos;
        pixelRes[outSlot].path.rcNormal = vec4(rcNrm.xyz, uintBitsToFloat(rcInst));
        if (rr == tt - 1u)
        {
            // rc at the candidate vertex: Lsuf precomputed at creation
            // (its own connection BSDF stripped, connection dir cached).
            pixelRes[outSlot].path.rcLsuf = wfArena[cb + 6u];
        }
        else
        {
            // Interior rc: the shared divisor already carries rho_r (the
            // rc scatter ran before this deferred absorb).
            pixelRes[outSlot].path.rcLsuf = vec4(SafeDiv(fCand, fArr.xyz), rcExt.y);
        }
        pixelRes[outSlot].path.misCache = wfArena[cb + 2u];
    }
    else if (kind == WF_CND_NEE_LIGHT)
    {
        // Light point stored in object space; normal converted here.
        vec4 lp = wfArena[cb + 4u];
        vec4 ex = wfArena[cb + 5u];
        uint lightIdx = floatBitsToUint(lp.w);
        uint liInst = InstanceOfTriangle(uint(lightTris[lightIdx].p0.w + 0.5));
        pixelRes[outSlot].path.rcInfo = vec4(ex.x, ex.y, omega, meta.z);
        pixelRes[outSlot].path.rcPosMat = vec4(lp.xyz, uintBitsToFloat(NO_MATERIAL));
        pixelRes[outSlot].path.rcNormal = vec4(
            RestirWorldToObjNormal(liInst, lightTris[lightIdx].normalArea.xyz),
            uintBitsToFloat(liInst));
        pixelRes[outSlot].path.rcLsuf = vec4(lightTris[lightIdx].emissionWeight.rgb, 0.0);
        pixelRes[outSlot].path.misCache = wfArena[cb + 2u];
    }
    else // WF_CND_LIGHTRC
    {
        vec4 mc = wfArena[cb + 2u];
        vec4 ext = wfArena[cb + 6u];
        uint mi = floatBitsToUint(wfArena[cb + 4u].w); // lyMat<<8 | lyInst
        pixelRes[outSlot].path.rcInfo = vec4(0.0, 0.0, omega, meta.z);
        pixelRes[outSlot].path.lyPosMat = vec4(wfArena[cb + 5u].xyz, uintBitsToFloat(mi >> 8));
        pixelRes[outSlot].path.lyNormal = vec4(wfArena[cb + 4u].xyz, uintBitsToFloat(mi & 0xFu));
        pixelRes[outSlot].path.lyTput = vec4(mc.yzw, ext.z); // + octa(ly wi)
        pixelRes[outSlot].path.misCache = vec4(ext.x, ext.y, 0.0, 0.0);
    }
}
