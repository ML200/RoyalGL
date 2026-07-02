// The bidirectional hybrid shift mapping (paper Sec. 5, Phase 2 technique
// set). Requires common.glsl + light_tree.glsl + bdpt_common.glsl +
// restir_common.glsl included first.
//
// RestirShiftPath maps a base reservoir's path onto a new primary hit.
// Camera-side techniques (t>=2): random-replay the scatters at vertices
// 1..r-2 from the stored seed, then reconnect to the stored reconnection
// vertex x_r and reuse the cached suffix radiance L_suf. Paths without a
// reconnection vertex (specular chains ending on an emitter, environment
// paths, directly visible emitters) are re-traced entirely by random
// replay. Light tracing (t=1, non-caustic): the REVERSE hybrid shift -
// replay the light subpath from the stored seed up to y_{s-2}, reconnect
// y_{s-2} to the destination pixel's primary hit (which becomes the new
// y_{s-1} = x_1), and re-evaluate the deterministic camera connection.
//
// Everything is measured in solid angle (paper Sec. 7 / Appendix B):
//   replayed scatter Jacobian  = pdf_base / pdf_shifted      (Eq. 53)
//   reconnection Jacobian      = (cos'/cos) * (d2_base/d2')  (Eq. 55)
// The base path's replayed-pdf product is cached in the reservoir
// (rcInfo.w), so no per-vertex base storage is needed.
//
// Bijectivity: a shift fails (returns ok=false, contribution 0) if any
// replayed vertex changes its delta/non-delta classification, hits an
// emitter prematurely, ends in the wrong terminal (env vs emitter), or if
// the reconnection is occluded / lands on the back side of x_r.

struct RestirShiftResult
{
    bool ok;
    vec3 f;          // f of the shifted path (current-domain, our f convention)
    float jacobian;  // |dT/dX|
    float rcCos;     // updated rcInfo.x for the shifted path
    float rcDist2;   // updated rcInfo.y
    float replayPdf; // updated rcInfo.w
};

RestirShiftResult RestirShiftFail()
{
    RestirShiftResult res;
    res.ok = false;
    res.f = vec3(0.0);
    res.jacobian = 0.0;
    res.rcCos = 0.0;
    res.rcDist2 = 0.0;
    res.replayPdf = 1.0;
    return res;
}

// Reverse hybrid shift for non-caustic t=1 paths (paper Sec. 5 "t<=1,
// non-caustic"). The light subpath is replayed under the DESTINATION
// domain's sampler: the light-tree descent is anchored at the destination
// camera, so temporal shifts across a camera move may replay onto a
// different emitter - that is ordinary random replay, priced by the pdf
// ratio (Eq. 53); with a static camera the base subpath is reproduced
// exactly. The reconnection Jacobian is the endpoint geometry ratio in
// solid angle at y_{s-2} (Eq. 56, i=2), the same form as the camera-side
// Eq. 55 with the moving endpoint on the x_1 side. The deterministic camera
// connection (imageToSurface) changes f only - nothing samples it, so it
// contributes no Jacobian. omega_tau is copied (Sec. 6.4 biased variant,
// like the camera-side shift; Phase 5 recomputes it).
RestirShiftResult RestirShiftLightPath(PathReservoir base, GBufferPixel dstG, vec3 dstWi,
                                       vec3 dstCamPos, vec3 dstCamForward, float dstIpd)
{
    RestirShiftResult res = RestirShiftFail();
    if (dstG.posDepth.w < 0.0) return res;

    uint tech = floatBitsToUint(base.core.w);
    uint s = RestirTechS(tech);
    uint flags = RestirTechFlags(tech);
    if ((flags & RESTIR_FLAG_CAUSTIC) != 0u) return res; // caustics: Phase 3, never shifted here
    if (s < 2u) return res;
    uint deltaMask = RestirFlagsDeltaMask(flags);

    // The destination anchor x_1' must be connectable and non-emissive
    // (light subpaths terminate on emitters, so no base path has an
    // emissive x_1 - classification must match).
    Material mX1 = materials[GBufMaterial(dstG)];
    if (MatIsDelta(mX1)) return res;
    if (dot(mX1.emissive.rgb, mX1.emissive.rgb) > 0.0) return res;

    // ---------------------------------------------------- light replay ---
    g_rngSeed = floatBitsToUint(base.fSeed.w);
    RngStream(0u, RNG_EMIT);
    float pdfDescent, pdfLeaf;
    uint leaf = LT_Descend(dstCamPos, dstCamForward, pdfDescent);
    if (pdfDescent <= 0.0) return res;
    uint lightIdx = LT_SampleLeafTriangle(leaf, pdfLeaf);
    float pickPdf = pdfDescent * pdfLeaf;
    if (pickPdf <= 0.0) return res;
    LightTri lt = lightTris[lightIdx];
    vec3 lightN = lt.normalArea.xyz;
    vec3 origin = BdptSampleLightPoint(lightIdx, RandomFloat2());
    float invArea = 1.0 / max(lt.normalArea.w, 1e-10);

    vec3 f;
    float replayPdf;
    vec3 prevPos, prevN;
    vec3 prevWi = vec3(0.0);
    Material prevMat = mX1; // overwritten before use; keeps GLSL happy
    bool prevIsLight = (s == 2u);

    if (s == 2u)
    {
        // y_{s-2} is the emission point itself; only the pick and the area
        // sample are replayed (the emission direction is what the
        // reconnection replaces).
        prevPos = origin;
        prevN = lightN;
        f = lt.emissionWeight.rgb;
        replayPdf = pickPdf * invArea;
    }
    else
    {
        vec3 dir = CosineSampleHemisphere(lightN);
        float cosTheta = dot(lightN, dir);
        if (cosTheta <= 1e-6) return res;
        f = lt.emissionWeight.rgb * cosTheta;
        replayPdf = pickPdf * invArea * cosTheta / PI;

        Ray ray = MakeRay(origin + lightN * 1e-4, dir);
        bool reached = false;
        for (uint j = 1u; j + 1u < s; ++j) // vertices y_1 .. y_{s-2}
        {
            Hit hit;
            if (!IntersectScene(ray, hit)) return res;
            Material mat = materials[hit.materialIndex];
            // Base subpaths have no interior emitters (bijectivity).
            if (dot(mat.emissive.rgb, mat.emissive.rgb) > 0.0) return res;
            bool isDelta = MatIsDelta(mat);
            if (isDelta != (((deltaMask >> (j - 1u)) & 1u) == 1u)) return res;

            vec3 hitPos = ray.origin + ray.dir * hit.t;
            vec3 wi = -ray.dir;
            if (j + 2u == s)
            {
                prevPos = hitPos;
                prevN = hit.normal;
                prevWi = wi;
                prevMat = mat;
                reached = true;
                break;
            }

            RngStream(j, RNG_BSDF);
            BsdfSample bs = SampleBsdf(mat, hit.normal, wi, true);
            if (bs.weight == vec3(0.0)) return res;
            if (!bs.specular && bs.pdfDir <= 0.0) return res;
            float pdfStep = bs.choicePdf * (bs.specular ? 1.0 : bs.pdfDir);
            f *= bs.weight * pdfStep;
            replayPdf *= pdfStep;

            vec3 offN = (dot(bs.dir, hit.normal) >= 0.0) ? hit.normal : -hit.normal;
            ray = MakeRay(hitPos + offN * 1e-4, bs.dir);
        }
        if (!reached) return res;
    }

    // ----------------------------------- reconnection y_{s-2} -> x_1' ----
    vec3 x1 = dstG.posDepth.xyz;
    vec3 toX1 = x1 - prevPos;
    float d2 = dot(toX1, toX1);
    if (d2 <= 1e-12) return res;
    float dist = sqrt(d2);
    vec3 dirTo = toX1 / dist;

    if (prevIsLight)
    {
        float cosL = dot(prevN, dirTo);
        if (cosL <= 1e-6) return res; // one-sided emitter
        f *= cosL;
    }
    else
    {
        float pd, pr, cosOut;
        vec3 fb = EvalBsdf(prevMat, prevN, prevWi, dirTo, pd, pr, cosOut);
        if (fb == vec3(0.0)) return res;
        f *= fb * cosOut;
    }

    vec3 nf = (dot(prevN, dirTo) >= 0.0) ? prevN : -prevN;
    Ray shadowRay = MakeRay(prevPos + nf * 1e-4, dirTo);
    if (IntersectSceneOccluded(shadowRay, dist * 0.999)) return res;

    // x_1' factors: BSDF toward the destination camera plus the
    // deterministic t=1 camera factor, mirroring candidate creation in
    // restir_light.comp. cosRc (cos at x_1' toward y_{s-2}) is the moving
    // endpoint's cosine in the reconnection Jacobian.
    float pdX, prX, cosRc;
    vec3 fbX = EvalBsdf(mX1, dstG.normalMat.xyz, dstWi, -dirTo, pdX, prX, cosRc);
    if (fbX == vec3(0.0)) return res;
    vec3 nfX = (dot(dstG.normalMat.xyz, dstWi) >= 0.0) ? dstG.normalMat.xyz : -dstG.normalMat.xyz;
    float cosToCam = dot(nfX, dstWi);
    float cosAtCam = dot(dstCamForward, -dstWi);
    if (cosAtCam <= 1e-3 || cosToCam <= 1e-6) return res;
    float d2cam = dstG.posDepth.w * dstG.posDepth.w;
    float imagePointToCamDist = dstIpd / cosAtCam;
    float imageToSurface = (imagePointToCamDist * imagePointToCamDist) / cosAtCam
                           * cosToCam / max(d2cam, 1e-12);
    f *= fbX * imageToSurface;

    // Jacobians: reverse reconnection (Eq. 56, i=2) + random replay (Eq. 53).
    if (base.rcInfo.x <= 0.0 || base.rcInfo.y <= 0.0 || replayPdf <= 0.0) return res;
    float J = (cosRc / base.rcInfo.x) * (base.rcInfo.y / d2);
    J *= base.rcInfo.w / replayPdf;

    res.f = f;
    res.jacobian = J;
    res.rcCos = cosRc;
    res.rcDist2 = d2;
    res.replayPdf = replayPdf;
    res.ok = true;
    if (any(isnan(res.f)) || any(isinf(res.f)) || isnan(J) || isinf(J) || J <= 0.0)
        res = RestirShiftFail();
    return res;
}

// dstG: primary hit of the target pixel; dstWi: unit vector from that hit
// toward the camera that generated it; dstCamPos/dstCamForward/dstIpd: that
// camera's position, forward axis and image-plane distance in pixel units
// (current or previous frame's) - used only by t=1 shifts, which must
// re-evaluate the camera connection and replay the light-tree descent in
// the destination domain.
RestirShiftResult RestirShiftPath(PathReservoir base, GBufferPixel dstG, vec3 dstWi,
                                  vec3 dstCamPos, vec3 dstCamForward, float dstIpd)
{
    RestirShiftResult res;
    res.ok = false;
    res.f = vec3(0.0);
    res.jacobian = 0.0;
    res.rcCos = 0.0;
    res.rcDist2 = 0.0;
    res.replayPdf = 1.0;

    if (dstG.posDepth.w < 0.0) return res;

    uint tech = floatBitsToUint(base.core.w);
    uint s = RestirTechS(tech);
    uint t = RestirTechT(tech);
    if (t == 1u)
        return RestirShiftLightPath(base, dstG, dstWi, dstCamPos, dstCamForward, dstIpd);
    uint flags = RestirTechFlags(tech);
    bool rcValid = (flags & RESTIR_FLAG_RCVALID) != 0u;
    bool envEnd = (flags & RESTIR_FLAG_ENVEND) != 0u;
    uint r = RestirFlagsRcIndex(flags);
    uint deltaMask = RestirFlagsDeltaMask(flags);

    g_rngSeed = floatBitsToUint(base.fSeed.w);

    vec3 x = dstG.posDepth.xyz;
    vec3 n = dstG.normalMat.xyz;
    uint matId = GBufMaterial(dstG);
    uint triIdx = GBufTriangle(dstG);
    vec3 wi = dstWi;

    // Directly visible emitter (s=0, t=2, no rc): the shifted path is just
    // the new primary hit, which must itself be a front-facing emitter.
    if (!rcValid && !envEnd && t == 2u)
    {
        Material m1 = materials[matId];
        if (dot(m1.emissive.rgb, m1.emissive.rgb) <= 0.0) return res;
        uint lightIdx = (uFrame.lightInfo.x > 0u) ? triToLight[triIdx] : LT_SENTINEL;
        vec3 lightN = (lightIdx != LT_SENTINEL) ? lightTris[lightIdx].normalArea.xyz : n;
        if (dot(lightN, wi) <= 1e-6) return res;
        res.ok = true;
        res.f = m1.emissive.rgb;
        res.jacobian = 1.0;
        return res;
    }

    vec3 f = vec3(1.0);
    float replayPdf = 1.0;
    float J = 1.0;

    // Vertex loop; i is the index of the current shifted vertex x'_i.
    for (uint i = 1u; i <= RESTIR_MAX_VERTS; ++i)
    {
        Material mat = materials[matId];
        bool isDelta = MatIsDelta(mat);

        // Classification must match the base path (bijectivity).
        if (isDelta != (((deltaMask >> (i - 1u)) & 1u) == 1u)) return res;

        // Premature emitter: every vertex processed at the loop top is a
        // non-terminal vertex of the base path (terminals are handled by
        // the early direct-emitter branch, the reconnection branch, or the
        // loop-bottom terminal checks), so base x_i was non-emissive and a
        // shifted path terminating here has no counterpart.
        if (dot(mat.emissive.rgb, mat.emissive.rgb) > 0.0) return res;

        if (rcValid && i == r - 1u)
        {
            // ------------------------------------------ reconnection -----
            if (isDelta) return res; // guaranteed by mask, kept for clarity
            vec3 toRc = base.rcPosMat.xyz - x;
            float d2 = dot(toRc, toRc);
            if (d2 <= 1e-12) return res;
            float dist = sqrt(d2);
            vec3 dir = toRc / dist;

            // The stored normal is oriented toward valid observers; the new
            // predecessor must be on that side (Lambertian hemisphere /
            // emitter one-sidedness).
            float cosRc = dot(base.rcNormal.xyz, -dir);
            if (cosRc <= 1e-6) return res;

            float pdfDir, pdfRev, cosOut;
            vec3 fb = EvalBsdf(mat, n, wi, dir, pdfDir, pdfRev, cosOut);
            if (fb == vec3(0.0)) return res;

            vec3 nf = (dot(n, dir) >= 0.0) ? n : -n;
            Ray shadowRay = MakeRay(x + nf * 1e-4, dir);
            if (IntersectSceneOccluded(shadowRay, dist * 0.999)) return res;

            if (base.rcInfo.x <= 0.0 || base.rcInfo.y <= 0.0) return res;

            f *= fb * cosOut;          // rho*cos at x'_{r-1}
            f *= base.rcLsuf.xyz;      // cached suffix radiance
            J *= (cosRc / base.rcInfo.x) * (base.rcInfo.y / d2); // Eq. 55

            res.ok = true;
            res.rcCos = cosRc;
            res.rcDist2 = d2;
            break;
        }

        // -------------------------------------------- replayed scatter ---
        RngStream(i, RNG_BSDF);
        BsdfSample bs = SampleBsdf(mat, n, wi, false);
        if (bs.weight == vec3(0.0)) return res;
        if (!bs.specular && bs.pdfDir <= 0.0) return res;

        float pdfStep = bs.choicePdf * (bs.specular ? 1.0 : bs.pdfDir);
        f *= bs.weight * pdfStep; // = rho*cos under our delta convention
        replayPdf *= pdfStep;

        vec3 offN = (dot(bs.dir, n) >= 0.0) ? n : -n;
        Ray ray = MakeRay(x + offN * 1e-4, bs.dir);
        Hit hit;
        if (!IntersectScene(ray, hit))
        {
            // Environment terminal: only valid if the base path also ended
            // at the environment after this exact scatter.
            if (!envEnd || rcValid || (i + 2u) != t) return res;
            f *= uFrame.background.rgb * uFrame.background.a;
            res.ok = true;
            break;
        }
        if (envEnd && (i + 2u) == t) return res; // base escaped, shift didn't

        // Advance to x'_{i+1}.
        x = ray.origin + ray.dir * hit.t;
        n = hit.normal;
        matId = hit.materialIndex;
        triIdx = hit.triIndex;
        wi = -bs.dir;

        // Pure-replay terminal: the final surface vertex must be a
        // front-facing emitter (s=0 with a specular chain).
        if (!rcValid && !envEnd && (i + 1u) == t - 1u)
        {
            Material em = materials[matId];
            if (dot(em.emissive.rgb, em.emissive.rgb) <= 0.0) return res;
            uint lightIdx = (uFrame.lightInfo.x > 0u) ? triToLight[triIdx] : LT_SENTINEL;
            vec3 lightN = (lightIdx != LT_SENTINEL) ? lightTris[lightIdx].normalArea.xyz : n;
            if (dot(lightN, wi) <= 1e-6) return res;
            f *= em.emissive.rgb;
            res.ok = true;
            break;
        }
    }

    if (!res.ok) return res;

    // Replay Jacobian (Eq. 53): base pdf product over the replayed scatters
    // is cached in rcInfo.w.
    if (replayPdf <= 0.0) { res.ok = false; return res; }
    J *= base.rcInfo.w / replayPdf;

    res.f = f;
    res.jacobian = J;
    res.replayPdf = replayPdf;
    if (any(isnan(res.f)) || any(isinf(res.f)) || isnan(J) || isinf(J) || J <= 0.0)
    {
        res.ok = false;
        res.f = vec3(0.0);
        res.jacobian = 0.0;
    }
    return res;
}
