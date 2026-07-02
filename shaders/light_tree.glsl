// Light tree sampling for next-event estimation, ported from
// RoyalTracer-DX's LightTree_v8.hlsli (single level: RoyalGL's scene is one
// flattened world-space triangle soup, so the DX engine's TLAS/BLAS pair
// collapses into one 4-wide tree). Requires common.glsl (RNG, FrameUBO).
//
// Sampling: stochastic descent from the root, at each node picking a child
// proportionally to a trig-free importance estimate (power * geometry *
// normal-cone orientation * receiver cosine), then a power-weighted pick
// inside the leaf, then a uniform point on the chosen triangle.
// Pdf: LT_PdfSelectTriangle re-walks the tree deterministically (each node
// stores its subtree's triangle range, and the triangle array is in
// leaf-list order) multiplying the same per-node probabilities - needed to
// MIS BSDF-sampled emitter hits against this strategy.

// Struct fields mirror src/gfx/GPUTypes.h (GPULightTreeNode). The light
// triangle buffer, tri->light map and LT_* constants live in common.glsl,
// shared with the bidirectional kernels.
struct LightTreeNode
{
    vec4 bminPower; // xyz=aabb min, w=subtree power
    vec4 bmaxCosO;  // xyz=aabb max, w=cos(theta_o) of merged normal cone
    vec4 axisCosE;  // xyz=cone axis, w=cos(theta_e) emission falloff
    uvec4 meta;     // x=firstChild, y=childCount (0=leaf), z=triFirst, w=triCount
};
layout(std430, binding = 5) readonly buffer LightTreeNodesSSBO { LightTreeNode ltNodes[]; };

// ------------------------------------------ trig-free node importance ----
// Estimates the contribution of a node's cluster to shading point x with
// surface normal n, without ever intersecting it: power falloff over the
// cluster's bounding-sphere distance, the merged normal cone's orientation
// toward x (widened by the uncertainty angle subtended by the cluster),
// and the receiver cosine (same widening). The 0.01 relative floors keep
// boundary regions sampleable - a hard zero produces fireflies wherever the
// pdf underestimates a reachable light.
float LT_NodeImportance(LightTreeNode nd, vec3 x, vec3 n)
{
    vec3 bmin = nd.bminPower.xyz;
    vec3 bmax = nd.bmaxCosO.xyz;
    float power = nd.bminPower.w;
    float cosTheta_o = nd.bmaxCosO.w;

    vec3 c = 0.5 * (bmin + bmax);
    vec3 e = 0.5 * (bmax - bmin);
    float R2 = dot(e, e);

    vec3 v = x - c;
    float d2 = dot(v, v);
    float invD = inversesqrt(max(d2, 1e-12));
    vec3 dir = v * invD;

    float R = sqrt(R2);
    float sinU = clamp(R * invD, 0.0, 1.0);
    float cosU = sqrt(max(1.0 - sinU * sinU, 0.0));

    float cosTheta = dot(nd.axisCosE.xyz, dir);
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float sinTheta_o = sqrt(max(1.0 - cosTheta_o * cosTheta_o, 0.0));

    // cos/sin of (theta - theta_o) and then of (theta - theta_o - theta_u)
    float cosA = cosTheta * cosTheta_o + sinTheta * sinTheta_o;
    float sinA = sinTheta * cosTheta_o - cosTheta * sinTheta_o;
    float cosFull = cosA * cosU + sinA * sinU;

    float cosOuterBound = cosTheta_o * cosU - sinTheta_o * sinU;
    float sinOuterBound = sinTheta_o * cosU + cosTheta_o * sinU;
    bool insideCone = (sinOuterBound <= 0.0) || (cosTheta >= cosOuterBound);

    float orientTerm = insideCone ? 1.0 : max(cosFull, 0.0);

    float ci = dot(-dir, n);
    float cosReceiver = clamp(ci + sinU, 0.0, 1.0);

    float geom = 1.0 / (d2 + R2);

    const float kFloor = 0.01;
    cosReceiver = max(cosReceiver, kFloor);
    orientTerm  = max(orientTerm,  kFloor);

    return power * geom * orientTerm * cosReceiver;
}

// ------------------------------- q-smoothed descent probabilities --------
// gain=0 collapses the descent to the raw importance pick. The smoothed
// per-child probability is q_i/sumQ with q_i = s + (1-s)*w_i/sumW and
// sumQ = n*s + (1-s): near-balanced children move toward uniform selection
// (removes visible banding at cluster boundaries), dominated children stay
// concentrated. Applied identically on the sampling and pdf sides, so MIS
// stays unbiased.
const float LT_SPLIT_GAIN = 1.0;
const float LT_SPLIT_MAX  = 0.5;

// Both the sampler and the pdf evaluation walk nodes with the same two-pass
// scalar scheme below - pass 1 accumulates sum/min/max of the child
// importances, pass 2 turns them into the (smoothed) probability of one
// child. No local arrays: dynamically indexed register arrays inside
// breaking loops miscompile on some GL drivers, and importances are cheap
// enough to evaluate twice.
struct LTLevelStats { float sumW; float s; };

LTLevelStats LT_LevelStats(LightTreeNode N, vec3 x, vec3 n)
{
    LTLevelStats st;
    st.sumW = 0.0;
    float lo = 1e30;
    float hi = 0.0;
    for (uint i = 0u; i < N.meta.y; ++i)
    {
        float w = max(LT_NodeImportance(ltNodes[N.meta.x + i], x, n), 0.0);
        st.sumW += w;
        lo = min(lo, w);
        hi = max(hi, w);
    }
    st.s = (LT_SPLIT_GAIN > 0.0 && hi > 0.0)
        ? clamp((lo / hi) * LT_SPLIT_GAIN, 0.0, LT_SPLIT_MAX)
        : 0.0;
    return st;
}

// Smoothed probability of child i whose raw importance is w.
float LT_ChildProb(LTLevelStats st, uint n, float w)
{
    if (st.sumW <= 0.0) return 1.0 / float(n);
    float q = st.s + (1.0 - st.s) * (w / st.sumW);
    float sumQ = float(n) * st.s + (1.0 - st.s);
    return q / sumQ;
}

// ------------------------------------------------ stochastic descent -----
// Returns the leaf node index; pdfDescent accumulates the product of the
// per-level child probabilities. One fresh uniform per level. Depth-capped:
// a malformed tree returns pdf 0, which invalidates the sample instead of
// hanging the GPU.
uint LT_Descend(vec3 x, vec3 n, out float pdfDescent)
{
    pdfDescent = 1.0;
    uint node = 0u;

    for (uint iter = 0u; iter < 64u; ++iter)
    {
        LightTreeNode N = ltNodes[node];
        if (N.meta.y == 0u) return node;

        LTLevelStats st = LT_LevelStats(N, x, n);
        float xi = min(RandomFloat(), LT_ONE_MINUS_EPSILON);

        // Walk the (smoothed) child probabilities; the loop-end fallback
        // absorbs float round-off in the running sum.
        float accum = 0.0;
        uint pick = N.meta.y - 1u;
        float pickP = 0.0;
        for (uint i = 0u; i < N.meta.y; ++i)
        {
            float w = max(LT_NodeImportance(ltNodes[N.meta.x + i], x, n), 0.0);
            float p = LT_ChildProb(st, N.meta.y, w);
            if (xi < accum + p || i == N.meta.y - 1u) { pick = i; pickP = p; break; }
            accum += p;
        }

        pdfDescent *= pickP;
        node = N.meta.x + pick;
    }

    pdfDescent = 0.0;
    return 0u;
}

// ------------------------------------------- leaf triangle sampling ------
// Power-weighted pick inside the leaf (uniform picks would waste samples on
// dim triangles whenever one dominates). Two passes: sum, then select.
uint LT_SampleLeafTriangle(uint leafNode, out float pdfLeaf)
{
    LightTreeNode N = ltNodes[leafNode];
    uint base = N.meta.z;
    uint count = max(N.meta.w, 1u);
    float xi = min(RandomFloat(), LT_ONE_MINUS_EPSILON);

    float sumW = 0.0;
    for (uint i = 0u; i < count; ++i)
        sumW += max(lightTris[base + i].emissionWeight.w, 0.0);

    if (sumW <= 0.0)
    {
        uint k = min(uint(floor(xi * float(count))), count - 1u);
        pdfLeaf = 1.0 / float(count);
        return base + k;
    }

    float target = xi * sumW;
    float accum = 0.0;
    uint sel = count - 1u;
    float selW = max(lightTris[base + sel].emissionWeight.w, 0.0);
    for (uint k = 0u; k < count; ++k)
    {
        float w = max(lightTris[base + k].emissionWeight.w, 0.0);
        float next = accum + w;
        if (target < next) { sel = k; selW = w; break; }
        accum = next;
    }

    pdfLeaf = selW / sumW;
    return base + sel;
}

// --------------------------------------------------------------- pdf -----
// Probability that LT_Descend + LT_SampleLeafTriangle would have selected
// this exact light triangle from shading point x - the tree walks to the
// child whose triangle range contains lightIdx (the triangle array is in
// leaf-list order) multiplying the same q-smoothed probabilities the
// sampler used.
float LT_PdfSelectTriangle(vec3 x, vec3 n, uint lightIdx)
{
    float pdf = 1.0;
    uint node = 0u;

    for (uint iter = 0u; iter < 64u; ++iter)
    {
        LightTreeNode N = ltNodes[node];

        if (N.meta.y == 0u)
        {
            uint base = N.meta.z;
            uint count = max(N.meta.w, 1u);
            float sumW = 0.0;
            float myW = 0.0;
            for (uint j = 0u; j < count; ++j)
            {
                float w = max(lightTris[base + j].emissionWeight.w, 0.0);
                sumW += w;
                if (base + j == lightIdx) myW = w;
            }
            float pdfLeaf = (sumW > 0.0) ? (myW / sumW) : (1.0 / float(count));
            return pdf * pdfLeaf;
        }

        LTLevelStats st = LT_LevelStats(N, x, n);

        int childHit = -1;
        float wChild = 0.0;
        for (uint i = 0u; i < N.meta.y; ++i)
        {
            LightTreeNode C = ltNodes[N.meta.x + i];
            if (lightIdx >= C.meta.z && lightIdx < C.meta.z + C.meta.w)
            {
                childHit = int(i);
                wChild = max(LT_NodeImportance(C, x, n), 0.0);
                break;
            }
        }
        if (childHit < 0) return 0.0; // malformed ranges - invalidate

        pdf *= LT_ChildProb(st, N.meta.y, wChild);
        node = N.meta.x + uint(childHit);
    }

    return 0.0;
}

// ------------------------------------------------- light point sample ----
struct LTLightSample
{
    uint triIdx;
    vec3 position;
    vec3 normal;   // geometric, from the stored (winding-defined) normal
    vec3 emission;
    float pdfSolidAngle;
};

// Full NEE sample: descend the tree, pick a triangle, pick a uniform point
// on it, and convert the selection*area pdf to solid angle at refPos.
// pdfSolidAngle == 0 flags an invalid/unreachable sample (backfacing light,
// degenerate geometry, depth-cap overflow) - the caller must skip it.
LTLightSample LT_SamplePointOnLight(vec3 refPos, vec3 refNormal)
{
    LTLightSample s;
    s.pdfSolidAngle = 0.0;
    s.triIdx = 0u;

    float pdfDescent, pdfLeaf;
    uint leaf = LT_Descend(refPos, refNormal, pdfDescent);
    if (pdfDescent <= 0.0) return s;
    uint triIdx = LT_SampleLeafTriangle(leaf, pdfLeaf);
    s.triIdx = triIdx;
    float pdfSelect = pdfDescent * pdfLeaf;


    LightTri tri = lightTris[triIdx];
    s.emission = tri.emissionWeight.rgb;
    s.normal = tri.normalArea.xyz;

    // Uniform point on the triangle
    vec2 r = RandomFloat2();
    float sqrtR1 = sqrt(r.x);
    float u = 1.0 - sqrtR1;
    float v = r.y * sqrtR1;
    s.position = (1.0 - u - v) * tri.p0.xyz + u * tri.p1.xyz + v * tri.p2.xyz;

    // pdf_SA = pdf_area * dist^2 / cos(theta_light); emitters are one-sided
    // for NEE (matching the tree's normal cones) - backfacing samples return
    // pdf 0 and the BSDF strategy covers that side alone (its MIS weight is
    // 1 there, so the estimator stays unbiased).
    float area = max(tri.normalArea.w, 1e-10);
    vec3 toLight = s.position - refPos;
    float distSq = dot(toLight, toLight);
    if (distSq <= 1e-12) return s;
    float dist = sqrt(distSq);
    float cosLight = dot(s.normal, -toLight / dist);
    if (cosLight <= 1e-6) return s;

    s.pdfSolidAngle = (pdfSelect / area) * distSq / cosLight;
    return s;
}

// Solid-angle pdf of NEE having produced the direction that hit light
// triangle `lightIdx` at distance `dist` from (x, n) - the counterpart the
// BSDF strategy needs for its MIS weight when a bounce ray hits an emitter.
float LT_PdfSolidAngle(vec3 x, vec3 n, uint lightIdx, vec3 rayDir, float dist)
{
    LightTri tri = lightTris[lightIdx];
    float cosLight = dot(tri.normalArea.xyz, -rayDir);
    if (cosLight <= 1e-6) return 0.0; // back side: NEE can never sample it

    float pdfSelect = LT_PdfSelectTriangle(x, n, lightIdx);
    float area = max(tri.normalArea.w, 1e-10);
    return (pdfSelect / area) * (dist * dist) / cosLight;
}
