// The bidirectional hybrid shift mapping (Phase 1: camera-side techniques
// s<=1 only - paper Sec. 5 "t>=2" case restricted to our two-material
// world). Requires common.glsl + restir_common.glsl included first.
//
// RestirShiftPath maps a base reservoir's path onto a new primary hit:
// random-replay the scatters at vertices 1..r-2 from the stored seed, then
// reconnect to the stored reconnection vertex x_r and reuse the cached
// suffix radiance L_suf. Paths without a reconnection vertex (specular
// chains ending on an emitter, environment paths, directly visible
// emitters) are re-traced entirely by random replay.
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

// dstG: primary hit of the target pixel; dstWi: unit vector from that hit
// toward the camera that generated it (current or previous frame's).
RestirShiftResult RestirShiftPath(PathReservoir base, GBufferPixel dstG, vec3 dstWi)
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
