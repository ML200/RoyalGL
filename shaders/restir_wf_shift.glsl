// Wavefront shift jobs (docs/RESTIR_BDPT_PLAN.md Phase 7): the bidirectional
// hybrid shift of restir_shift.glsl restructured as a per-vertex state
// machine so no BVH traversal shares a kernel with the replay bookkeeping.
// One JOB = one RestirShiftPath evaluation (a base reservoir slot mapped
// onto a destination pixel's anchor). The temporal pass creates two jobs
// per pixel (forward: history -> current, backward: candidate -> previous),
// the spatial pass two per pixel per neighbor round (forward: neighbor ->
// current, backward: aggregate -> neighbor); both share these kernels:
//
//   tinit/sinit  create jobs (all no-ray prologue work, incl. the trivial
//                t=2 direct-emitter shift, evaluated inline)
//   shiftstep r  advances a job by one vertex: consume the trace result,
//                terminal checks, reconnection evaluation (occlusion
//                deferred), or one replayed BSDF scatter
//   shifttrace   closest-hit for the replayed scatter (lean)
//   shiftshadow  occlusion for a reconnection; flips the prewritten result
//                to OK or FAIL - reconnections always complete a job
//   tmerge/smerge  the GRIS balance-heuristic merges (no rays)
//
// Determinism: all sampling replays the stored counter-based streams from
// RS_BASE's seed, so a job can resume at any vertex; only queue membership
// and the job state below cross kernel boundaries. The math is the
// megakernel's, expression for expression.
//
// t=1 note: the reverse hybrid shift COPIES omega_tau even in recompute
// mode (see the rationale in restir_shift.glsl / plan Phase 5). The
// megakernel maintained the light-side MIS recursion state anyway for a
// future h_i-filter fix; since it never reaches the result, the wavefront
// port drops that dead arithmetic but keeps every SUPPORT gate (the
// emissionPdfW checks), so the mapped path set is identical.
//
// Requires common.glsl + restir_common.glsl + restir_wf_common.glsl
// included first (deliberately NOT light_tree/bdpt_common - the merge/init
// kernels stay under the 16-storage-block budget; only shiftstep, which
// replays samplers, pulls those in).

#define RS_BASE pixelRes[jobBaseSlot].path

// ------------------------------------------------------- job state map ----
// base = pixelCount + jobIdx * 12 vec4s inside wfArena (region [0, N) is
// the per-pixel pass header, see tinit/sinit):
//  0 jobMeta x uintBits(baseSlot), y uintBits(dstPixel | isPrev<<31),
//            z uintBits(status), w unused
//  1 cur     xyz current replay vertex, w uintBits(mat<<24|tri)
//  2 nrm     xyz shading normal at cur
//  3 wi      xyz direction toward the previous vertex (pending ray = -wi)
//  4 f       xyz running f of the shifted path, w running replay pdf
//  5 mis     x dVCM, y dVC, z dT1 (camera replay only), w J (reconnection
//            Jacobian factors accumulated so far)
//  6 hitA    xyz hit position, w hit t (<0 = miss)  [shifttrace writes]
//  7 hitB    xyz hit normal, w uintBits(mat<<24|tri)
//  8 res     x rcCos, y rcDist2, z omega, w final Jacobian
//  9 shadow  xyz reconnection direction, w surface distance
uint WfShiftJobBase(uint jobIdx) { return RestirPixelCount() + jobIdx * 12u; }

const uint WF_JOB_RUNNING        = 0u;
const uint WF_JOB_PENDING_SHADOW = 1u;
const uint WF_JOB_OK             = 2u;
const uint WF_JOB_FAIL           = 3u;

// -------------------------------------------- destination camera info ----
// A job's destination domain is fully described by (dstPixel, isPrev): the
// anchor comes from the matching G-buffer half, the camera from the
// matching UBO block. ReSTIR is pinhole-only, so the ipd formula is the
// pinhole branch of BdptImagePlaneDist().
vec3 WfDstWi(ivec2 pix, bool isPrev)
{
    return -(isPrev ? RestirPrevPrimaryRay(pix) : RestirPrimaryRay(pix)).dir;
}
vec3 WfDstCamPos(bool isPrev)     { return isPrev ? uFrame.prevCamPos.xyz : uFrame.camPos.xyz; }
vec3 WfDstCamForward(bool isPrev) { return isPrev ? uFrame.prevCamForward.xyz : uFrame.camForward.xyz; }
float WfDstIpd(bool isPrev)
{
    return (float(uFrame.frameInfo.y) * 0.5)
         / (isPrev ? uFrame.prevCameraParams.x : uFrame.cameraParams.x);
}
GBufferPixel WfDstG(uint dstIdx, bool isPrev)
{
    return RestirLoadGBuf((isPrev ? GBufPrevOffset() : GBufCurOffset()) + dstIdx);
}

void WfJobFail(uint jb)
{
    wfArena[jb].z = uintBitsToFloat(WF_JOB_FAIL);
}

// Final guards + status write shared by every successful completion point
// (the megakernel's end-of-shift NaN/inf/J<=0 rejection).
void WfJobComplete(uint jb, vec3 f, float replayPdf, float J,
                   float rcCos, float rcDist2, float omega, bool pendingShadow)
{
    if (any(isnan(f)) || any(isinf(f)) || isnan(J) || isinf(J) || J <= 0.0)
    {
        WfJobFail(jb);
        return;
    }
    wfArena[jb + 4u] = vec4(f, replayPdf);
    wfArena[jb + 8u] = vec4(rcCos, rcDist2, omega, J);
    wfArena[jb].z = uintBitsToFloat(pendingShadow ? WF_JOB_PENDING_SHADOW : WF_JOB_OK);
}

// Creates one shift job: the megakernel's pre-loop prologue. Everything
// here is ray-free; jobs that need replay work are enqueued into step
// round 0, trivial and impossible shifts complete immediately. The caller
// guarantees the destination anchor exists (non-miss G-buffer entry).
void WfShiftCreateJob(uint jobIdx, uint baseSlot, uint dstIdx, bool isPrev)
{
    uint jb = WfShiftJobBase(jobIdx);
    uint jobBaseSlot = baseSlot;
    wfArena[jb] = vec4(uintBitsToFloat(baseSlot),
                       uintBitsToFloat(dstIdx | (isPrev ? 0x80000000u : 0u)),
                       uintBitsToFloat(WF_JOB_FAIL), 0.0);

    GBufferPixel dstG = WfDstG(dstIdx, isPrev);
    ivec2 pix = ivec2(int(dstIdx % uFrame.frameInfo.x), int(dstIdx / uFrame.frameInfo.x));
    vec3 dstWi = WfDstWi(pix, isPrev);

    uint tech = floatBitsToUint(RS_BASE.core.w);
    uint s = RestirTechS(tech);
    uint t = RestirTechT(tech);
    uint flags = RestirTechFlags(tech);

    if (t == 1u)
    {
        // Reverse hybrid shift prologue (RestirShiftLightPath's early
        // rejections); the light replay itself starts in step round 0.
        if ((flags & RESTIR_FLAG_CAUSTIC) != 0u) return; // caustics never here
        if (s < 2u) return;
        Material mX1 = materials[GBufMaterial(dstG)];
        if (MatIsDelta(mX1)) return;
        if (dot(mX1.emissive.rgb, mX1.emissive.rgb) > 0.0) return;
        wfArena[jb].z = uintBitsToFloat(WF_JOB_RUNNING);
        WfAppend(WfCtrlShade(0u), WfShiftStepQBase(0u), jobIdx);
        return;
    }

    bool rcValid = (flags & RESTIR_FLAG_RCVALID) != 0u;
    bool envEnd = (flags & RESTIR_FLAG_ENVEND) != 0u;
    bool lightRc = (flags & RESTIR_FLAG_LIGHTRC) != 0u;

    if (!rcValid && !envEnd && !lightRc && t == 2u)
    {
        // Directly visible emitter: the shifted path is just the new
        // primary hit - no rays, complete inline.
        uint matId = GBufMaterial(dstG);
        Material m1 = materials[matId];
        if (dot(m1.emissive.rgb, m1.emissive.rgb) <= 0.0) return;
        uint lightIdx = (uFrame.lightInfo.x > 0u) ? triToLight[GBufTriangle(dstG)] : LT_SENTINEL;
        vec3 lightN = (lightIdx != LT_SENTINEL) ? lightTris[lightIdx].normalArea.xyz : dstG.normalMat.xyz;
        if (dot(lightN, dstWi) <= 1e-6) return;
        // omega: 1 either way (s=0 is the only sampler of this path).
        WfJobComplete(jb, m1.emissive.rgb, 1.0, 1.0, 0.0, 0.0, RS_BASE.rcInfo.z, false);
        return;
    }

    // Camera-side replay from the destination anchor: seed the state the
    // megakernel sets up before its vertex loop.
    float dVCM = 0.0, dVC = 0.0, dT1 = 0.0;
    if (RestirLightTracingEnabled())
    {
        vec3 dstCamForward = WfDstCamForward(isPrev);
        float cosAtCam = dot(dstCamForward, -dstWi);
        float cosIn1 = abs(dot(dstG.normalMat.xyz, dstWi));
        if (cosAtCam > 1e-3 && cosIn1 > 1e-6)
        {
            float imagePointToCamDist = WfDstIpd(isPrev) / cosAtCam;
            float cameraPdfW = (imagePointToCamDist * imagePointToCamDist) / cosAtCam;
            float d2v = dstG.posDepth.w * dstG.posDepth.w;
            // = BdptNumLightPaths(), inlined so non-tracing kernels can
            // include this file without bdpt_common (block budget).
            dVCM = (float(uFrame.lightInfo.z) / cameraPdfW) * (d2v / cosIn1);
            dT1 = dVCM;
        }
    }

    wfArena[jb + 1u] = vec4(dstG.posDepth.xyz, dstG.normalMat.w);
    wfArena[jb + 2u] = vec4(dstG.normalMat.xyz, 0.0);
    wfArena[jb + 3u] = vec4(dstWi, 0.0);
    wfArena[jb + 4u] = vec4(1.0, 1.0, 1.0, 1.0);
    wfArena[jb + 5u] = vec4(dVCM, dVC, dT1, 1.0);
    wfArena[jb].z = uintBitsToFloat(WF_JOB_RUNNING);
    WfAppend(WfCtrlShade(0u), WfShiftStepQBase(0u), jobIdx);
}

// Job result accessors for the merge kernels.
bool WfJobOk(uint jb) { return floatBitsToUint(wfArena[jb].z) == WF_JOB_OK; }
vec3 WfJobF(uint jb) { return wfArena[jb + 4u].xyz; }
float WfJobReplayPdf(uint jb) { return wfArena[jb + 4u].w; }
float WfJobJacobian(uint jb) { return wfArena[jb + 8u].w; }
float WfJobOmega(uint jb) { return wfArena[jb + 8u].z; }
float WfJobRcCos(uint jb) { return wfArena[jb + 8u].x; }
float WfJobRcDist2(uint jb) { return wfArena[jb + 8u].y; }
