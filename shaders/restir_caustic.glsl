// Caustic-path replay for the ReSTIR BDPT caustic reservoirs (paper
// Sec. 5.1; docs/RESTIR_BDPT_PLAN.md Phase 3). A caustic t=1 path is fully
// determined by its light subpath seed, so the shift between frames is PURE
// random replay: re-run the samplers with the stored seed under the
// destination frame's distributions (light-tree descent anchored at the
// destination camera), after which the path lands wherever its new y_{s-1}
// projects on screen (the paper's Appendix A justifies this pixel
// redistribution in GRIS). The camera connection is deterministic - it
// contributes imageToSurface to f and nothing to the Jacobian; the shift
// Jacobian is the replayed-pdf ratio alone (Eq. 53).
//
// Requires common.glsl + light_tree.glsl + bdpt_common.glsl +
// restir_common.glsl included first.

struct RestirCausticReplayResult
{
    bool ok;
    vec3 f;          // full path f in pixel-measurement units (incl. imageToSurface)
    float replayPdf; // product of every sampled pdf (pick, area, emission dir, scatters)
    ivec2 pixel;     // landing pixel under the destination camera
    float omega;     // omega_tau of the replayed path in the destination
                     // domain (Phase 5 recursive recompute; callers fall
                     // back to the stored omega when the toggle is off)
};

// Replays the caustic path (seed, s, flags with the delta/volume masks)
// under the camera described by the explicit basis + intrinsics (so both
// the current and the previous frame's camera work), including the
// classification checks that make the mapping bijective and the occlusion
// test of the camera connection. Volume-ending t=1 paths (airlight) are
// caustic-class too: pure replay + free landing is exactly their shift.
// gbufBase selects the destination frame's G-buffer half (GBufCurOffset /
// GBufPrevOffset, matching the camera): the landing pixel's primary depth
// feeds the airlight support gate and its omega's truncated camera-side
// competitor pdf (see restir_light.comp).
RestirCausticReplayResult RestirCausticReplay(uint seed, uint s, uint pathFlags,
    vec3 camPos, vec3 camForward, vec3 camRight, vec3 camUp,
    float tanFovY, float aspect, uint gbufBase)
{
    uint deltaMask = RestirFlagsDeltaMask(pathFlags);
    uint volMask = RestirFlagsVolMask(pathFlags);
    // Naive-port baseline (volume mode 0): volume-bearing caustic-class
    // paths (airlight) cannot replay either.
    if (!RestirVolShiftEnabled() && volMask != 0u) { RestirCausticReplayResult r; r.ok = false; r.f = vec3(0.0); r.replayPdf = 1.0; r.pixel = ivec2(-1); r.omega = 0.0; return r; }
    RestirCausticReplayResult res;
    res.ok = false;
    res.f = vec3(0.0);
    res.replayPdf = 1.0;
    res.pixel = ivec2(-1);
    res.omega = 0.0;
    if (s < 2u) return res;

    // ------------------------------------------------------- emission ----
    // Power-CDF pick, identical to creation (restir_light.comp): the pick
    // is camera-independent, so motion never re-descends to a different
    // light - the old tree-descent replay churned airlight/caustic history
    // under any camera movement.
    g_rngSeed = seed;
    RngStream(0u, RNG_EMIT);
    float pickPdf;
    uint lightIdx = RestirSampleLightIndex(pickPdf);
    if (pickPdf <= 0.0) return res;
    LightTri lt = lightTris[lightIdx];
    vec3 lightN = lt.normalArea.xyz;
    vec3 origin = BdptSampleLightPoint(lightIdx, RandomFloat2());
    vec3 dir = CosineSampleHemisphere(lightN);
    float cosTheta = dot(lightN, dir);
    if (cosTheta <= 1e-6) return res;
    float invArea = 1.0 / max(lt.normalArea.w, 1e-10);

    vec3 fNum = lt.emissionWeight.rgb * cosTheta;
    float replayPdf = pickPdf * invArea * cosTheta / PI;

    // Light-side MIS recursions (Phase 5; restir_light.comp conventions),
    // seeded with the frame-independent eval pdfs.
    float directPdfA, emissionPdfW;
    RestirLightPdfs(lightIdx, cosTheta, directPdfA, emissionPdfW);
    if (emissionPdfW <= 0.0) return res;
    float mVCM = directPdfA / emissionPdfW;
    float mVC = cosTheta / emissionPdfW;
    float mLW = mVC;

    Ray ray = MakeRay(origin + lightN * 1e-4, dir);

    // -------------------------------------------- walk y_1 .. y_{s-1} ----
    for (uint j = 1u; j < s; ++j)
    {
        Hit hit;
        bool surfHit = IntersectScene(ray, hit);

        // Fog free-flight replay (stream/keying identical to the creation
        // walk in restir_light.comp) + classification bijectivity.
        bool jVol = false;
        if (FogEnabled())
        {
            RngStream(j, RNG_DIST);
            float tS = FogSampleDist(RandomFloat());
            float dSurf = surfHit ? hit.t : 1e30;
            if (tS < dSurf) { jVol = true; hit.t = tS; }
        }
        if (jVol != (((volMask >> (j - 1u)) & 1u) == 1u)) return res;
        if (!surfHit && !jVol) return res;

        Material mat = materials[jVol ? 0u : hit.materialIndex];
        // Base subpaths have no interior emitters (bijectivity).
        if (!jVol && dot(mat.emissive.rgb, mat.emissive.rgb) > 0.0) return res;
        bool isDelta = !jVol && MatIsDelta(mat);
        if (isDelta != (((deltaMask >> (j - 1u)) & 1u) == 1u)) return res;

        float kInJ;
        if (jVol)
        {
            kInJ = FogSigmaT();
            float trSeg = FogTr(hit.t);
            fNum *= trSeg;
            replayPdf *= FogSigmaT() * trSeg;
        }
        else
        {
            float cosInJ = abs(dot(hit.normal, ray.dir));
            if (cosInJ <= 1e-6) return res;
            kInJ = cosInJ;
            if (FogEnabled())
            {
                float trSeg = FogTr(hit.t);
                fNum *= trSeg;
                replayPdf *= trSeg;
            }
        }
        mVCM *= hit.t * hit.t / kInJ;
        mVC /= kInJ;
        mLW /= kInJ;

        vec3 hitPos = ray.origin + ray.dir * hit.t;
        vec3 wi = -ray.dir;

        if (j + 1u == s)
        {
            // ------------------------- deterministic camera connection ---
            vec3 toCam = camPos - hitPos;
            float dist2 = dot(toCam, toCam);
            if (dist2 <= 1e-12) return res;
            float dist = sqrt(dist2);
            vec3 dirToCam = toCam / dist;

            float bsdfDirPdfW, bsdfRevPdfW, cosToCam, kToCam;
            vec3 fb;
            if (jVol)
            {
                fb = EvalPhase(ray.dir, dirToCam, bsdfDirPdfW, bsdfRevPdfW);
                cosToCam = 1.0;
                kToCam = FogSigmaT();
            }
            else
            {
                fb = EvalBsdf(mat, hit.normal, wi, dirToCam, bsdfDirPdfW, bsdfRevPdfW, cosToCam);
                kToCam = cosToCam;
            }
            if (fb == vec3(0.0)) return res;

            // Project y_{s-1} through the destination camera (same
            // conventions as BdptDirToPixel / RestirPrevProject).
            float z = dot(-toCam, camForward);
            if (z <= 1e-6) return res;
            float ndcX = dot(-toCam, camRight) / (z * tanFovY * aspect);
            float ndcY = -dot(-toCam, camUp) / (z * tanFovY);
            if (abs(ndcX) >= 1.0 || abs(ndcY) >= 1.0) return res;
            vec2 sizeF = vec2(uFrame.frameInfo.xy);
            vec2 p = (vec2(ndcX, ndcY) * 0.5 + 0.5) * sizeF;
            res.pixel = clamp(ivec2(p), ivec2(0), ivec2(sizeF) - 1);
            float cosAtCam = z / dist;

            // Airlight support gate, mirroring creation: the in-scatter
            // point must lie in front of the landing pixel's surface (miss
            // pixels accept every distance).
            float dPix = RestirLoadGBuf(gbufBase + RestirPixelIndex(res.pixel)).posDepth.w;
            if (jVol && dPix >= 0.0 && dist >= dPix) return res;

            Ray shadowRay;
            if (jVol)
                shadowRay = MakeRay(hitPos, dirToCam);
            else
            {
                vec3 nf = (dot(hit.normal, dirToCam) >= 0.0) ? hit.normal : -hit.normal;
                shadowRay = MakeRay(hitPos + nf * 1e-4, dirToCam);
            }
            if (IntersectSceneOccluded(shadowRay, ShadowTMax(dist))) return res;

            float ipd = (float(uFrame.frameInfo.y) * 0.5) / tanFovY;
            float imagePointToCamDist = ipd / cosAtCam;
            float imageToSolid = (imagePointToCamDist * imagePointToCamDist) / cosAtCam;
            float imageToSurface = imageToSolid * cosToCam / dist2;

            // omega_tau in the destination domain (Phase 5): the same t=1
            // weight restir_light.comp computes at candidate creation.
            // Volume-ending paths compete with the camera-side airlight
            // family (truncated primary in-scatter, restir_wf_caminit
            // walk 2), whose Tr-free volume density carries the extra
            // 1/pScat of the truncation - identical to creation's formula.
            float pdfScale = jVol ? 1.0 / max(FogScatterProb(dPix), 1e-7) : 1.0;
            float wL = (imageToSolid * kToCam * pdfScale / dist2 / float(BdptNumLightPaths()))
                     * (RestirConnectionsEnabled()
                            ? (mVCM + bsdfRevPdfW * mVC)
                            : (j == 1u ? (mVCM + bsdfRevPdfW * mLW)
                                       : (bsdfRevPdfW * mLW)));
            res.omega = 1.0 / (1.0 + wL);

            res.f = fNum * fb * imageToSurface * FogTr(dist);
            res.replayPdf = replayPdf;
            res.ok = !(any(isnan(res.f)) || any(isinf(res.f)) || replayPdf <= 0.0
                       || isnan(wL) || isinf(wL) || wL < 0.0);
            return res;
        }

        // -------------------------------------------- replayed scatter ---
        RngStream(j, RNG_BSDF);
        BsdfSample bs;
        if (jVol)
        {
            float pdfPhase;
            bs.dir = SamplePhaseHG(ray.dir, FogG(), RandomFloat2(), pdfPhase);
            bs.weight = vec3(FogSigmaS());
            bs.pdfDir = pdfPhase;
            bs.pdfRev = pdfPhase;
            bs.cosOut = FogSigmaT();
            bs.choicePdf = 1.0;
            bs.specular = false;
        }
        else
            bs = SampleBsdf(mat, hit.normal, wi, true);
        if (bs.weight == vec3(0.0)) return res;
        if (!bs.specular && bs.pdfDir <= 0.0) return res;
        float pdfStep = bs.choicePdf * (bs.specular ? 1.0 : bs.pdfDir);
        fNum *= bs.weight * pdfStep;
        replayPdf *= pdfStep;

        if (bs.specular)
        {
            mVC *= bs.cosOut;
            mLW *= bs.cosOut;
            mVCM = 0.0;
        }
        else
        {
            mVC = (bs.cosOut / bs.pdfDir) * (mVCM + bs.pdfRev * mVC);
            mLW = (bs.cosOut / bs.pdfDir) * ((j == 1u ? mVCM : 0.0) + bs.pdfRev * mLW);
            mVCM = 1.0 / bs.pdfDir;
        }

        if (jVol)
            ray = MakeRay(hitPos, bs.dir);
        else
        {
            vec3 offN = (dot(bs.dir, hit.normal) >= 0.0) ? hit.normal : -hit.normal;
            ray = MakeRay(hitPos + offN * 1e-4, bs.dir);
        }
    }
    return res;
}
