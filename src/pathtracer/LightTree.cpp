#include "pathtracer/LightTree.h"
#include "core/Log.h"
#include "gfx/GPUTypes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace RoyalGL
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kHalfPi = 1.57079632679489661923f;
        constexpr uint32_t kSentinel = 0xFFFFFFFFu;

        // Triangles per leaf. The DX source defaults to 16 (its ReSTIR pass
        // cleans up candidate quality afterwards); without ReSTIR the tree's
        // geometric importance should drive every selection, so leaves hold a
        // single triangle and the in-leaf pick (power-weighted only, no
        // geometry) disappears entirely. Degenerate splits (coincident
        // centroids) can still produce multi-triangle leaves as a fallback.
        constexpr uint32_t kMaxLeafTris = 1;
        constexpr uint32_t kBuildBins = 64;

        float Luminance(const glm::vec3& c)
        {
            return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
        }

        struct Aabb
        {
            glm::vec3 mn{std::numeric_limits<float>::max()};
            glm::vec3 mx{std::numeric_limits<float>::lowest()};
        };

        Aabb UnionAabb(const Aabb& a, const Aabb& b)
        {
            return {glm::min(a.mn, b.mn), glm::max(a.mx, b.mx)};
        }

        float AabbSurfaceArea(const Aabb& a)
        {
            glm::vec3 e = a.mx - a.mn;
            return 2.0f * (e.x * e.y + e.x * e.z + e.y * e.z);
        }

        float SafeAcos(float x) { return std::acos(std::clamp(x, -1.0f, 1.0f)); }

        // Bounding cone over emitter normals: axis + half-angle theta_o of
        // the normal spread, theta_e = emission falloff half-angle (pi/2 for
        // diffuse emitters).
        struct Cone
        {
            glm::vec3 axis{0.0f, 0.0f, 1.0f};
            float thetaO = kPi;
            float thetaE = kHalfPi;
        };

        glm::vec3 SlerpUnit(const glm::vec3& a, const glm::vec3& b, float t)
        {
            float cosT = std::clamp(glm::dot(a, b), -1.0f, 1.0f);
            float theta = std::acos(cosT);
            if (theta < 1e-6f) return a;
            float s = std::sin(theta);
            float w0 = std::sin((1.0f - t) * theta) / s;
            float w1 = std::sin(t * theta) / s;
            return glm::normalize(a * w0 + b * w1);
        }

        // Merge two cones per Algorithm 1 of Conty Estevez & Kulla,
        // "Importance Sampling of Many Lights with Adaptive Tree Splitting".
        Cone ConeUnion(Cone a, Cone b)
        {
            if (b.thetaO > a.thetaO) std::swap(a, b);

            float thetaD = SafeAcos(glm::dot(a.axis, b.axis));

            Cone out;
            out.thetaE = std::max(a.thetaE, b.thetaE);

            // b fully inside a?
            if (std::min(thetaD + b.thetaO, kPi) <= a.thetaO)
            {
                out.axis = a.axis;
                out.thetaO = a.thetaO;
                return out;
            }

            float thetaO = std::min((a.thetaO + thetaD + b.thetaO) * 0.5f, kPi);

            if (thetaD < 1e-7f)
            {
                out.axis = a.axis; // nearly parallel
            }
            else if (kPi - thetaD < 1e-7f)
            {
                // nearly anti-parallel: any perpendicular axis works
                glm::vec3 t = (std::fabs(a.axis.x) < 0.9f) ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
                out.axis = glm::normalize(glm::cross(a.axis, t));
            }
            else
            {
                float t = std::clamp((thetaO - a.thetaO) / thetaD, 0.0f, 1.0f);
                out.axis = SlerpUnit(a.axis, b.axis, t);
            }

            out.thetaO = thetaO;
            return out;
        }

        // Orientation measure M_omega (Eq. 1 of the paper): solid-angle-like
        // scalar for how widely the cluster emits; the SAOH cost multiplies
        // surface area by this so tightly-oriented clusters split cheaper.
        float OrientationMeasure(const Cone& c)
        {
            float thetaO = std::clamp(c.thetaO, 0.0f, kPi);
            float thetaW = std::min(thetaO + std::clamp(c.thetaE, 0.0f, kPi), kPi);
            double to = thetaO, tw = thetaW;
            double term0 = 2.0 * kPi * (1.0 - std::cos(to));
            double extra = kHalfPi * (2.0 * tw * std::sin(to)
                - std::cos(to - 2.0 * tw) - 2.0 * to * std::sin(to) + std::cos(to));
            return static_cast<float>(term0 + extra);
        }

        // One emissive scene triangle during the build.
        struct TmpTri
        {
            uint32_t sceneTri = 0;
            glm::vec3 centroid{0.0f};
            Aabb aabb;
            float power = 0.0f;
            Cone cone;
        };

        // Running aggregate of a triangle set (SAOH bin / split side / node).
        struct Agg
        {
            bool valid = false;
            Aabb a;
            float power = 0.0f;
            Cone cone;
            uint32_t count = 0;
        };

        void AggAdd(Agg& A, const TmpTri& t)
        {
            if (!A.valid)
            {
                A.valid = true;
                A.a = t.aabb;
                A.power = t.power;
                A.cone = t.cone;
                A.count = 1;
                return;
            }
            A.a = UnionAabb(A.a, t.aabb);
            A.power += t.power;
            A.cone = ConeUnion(A.cone, t.cone);
            A.count++;
        }

        void AggMerge(Agg& A, const Agg& B)
        {
            if (!B.valid) return;
            if (!A.valid) { A = B; return; }
            A.a = UnionAabb(A.a, B.a);
            A.power += B.power;
            A.cone = ConeUnion(A.cone, B.cone);
            A.count += B.count;
        }

        class Builder
        {
        public:
            std::vector<GPULightTreeNode> nodes;
            std::vector<GPULightTriangle> gpuTris;              // emitted in leaf-list order
            std::vector<uint32_t> triToLight;                   // sceneTri -> gpuTris index (or sentinel)
            const Scene* scene = nullptr;

            void Run(const Scene& s, std::vector<TmpTri>& tmp)
            {
                scene = &s;
                triToLight.assign(s.triangles.size(), kSentinel);
                nodes.reserve(tmp.size() * 2 + 4);
                gpuTris.reserve(tmp.size());
                nodes.emplace_back();
                BuildNode(0, tmp, 0, static_cast<uint32_t>(tmp.size()));
            }

        private:
            static void WriteNodeAggregate(GPULightTreeNode& n, const Agg& agg)
            {
                n.bminPower = glm::vec4(agg.a.mn, agg.power);
                n.bmaxCosO = glm::vec4(agg.a.mx, std::cos(std::clamp(agg.cone.thetaO, 0.0f, kPi)));
                n.axisCosE = glm::vec4(agg.cone.axis, std::cos(std::clamp(agg.cone.thetaE, 0.0f, kPi)));
            }

            void MakeLeaf(uint32_t nodeIdx, std::vector<TmpTri>& tmp, uint32_t begin, uint32_t end)
            {
                nodes[nodeIdx].meta = glm::uvec4(kSentinel, 0u,
                                                 static_cast<uint32_t>(gpuTris.size()), end - begin);
                for (uint32_t i = begin; i < end; ++i)
                {
                    const Triangle& st = scene->triangles[tmp[i].sceneTri];
                    glm::vec3 e1 = st.v1.position - st.v0.position;
                    glm::vec3 e2 = st.v2.position - st.v0.position;
                    glm::vec3 cr = glm::cross(e1, e2);
                    float len = glm::length(cr);
                    float area = 0.5f * len;
                    glm::vec3 normal = (len > 1e-20f) ? cr / len : glm::vec3(0, 0, 1);

                    GPULightTriangle g{};
                    g.p0 = glm::vec4(st.v0.position, 0.0f);
                    g.p1 = glm::vec4(st.v1.position, 0.0f);
                    g.p2 = glm::vec4(st.v2.position, 0.0f);
                    g.normalArea = glm::vec4(normal, area);
                    g.emissionWeight = glm::vec4(scene->materials[st.materialIndex].emissive, tmp[i].power);

                    triToLight[tmp[i].sceneTri] = static_cast<uint32_t>(gpuTris.size());
                    gpuTris.push_back(g);
                }
            }

            // Binned SAOH binary split of [begin,end). Returns false when no
            // usable split exists (all centroids coincide / degenerate cost).
            bool FindBinarySplit(std::vector<TmpTri>& tmp, uint32_t begin, uint32_t end, uint32_t& midOut)
            {
                Agg parent{};
                for (uint32_t i = begin; i < end; ++i) AggAdd(parent, tmp[i]);

                glm::vec3 ext = parent.a.mx - parent.a.mn;
                float lenMax = std::max(ext.x, std::max(ext.y, ext.z));
                float parentMA = std::max(1e-12f, AabbSurfaceArea(parent.a));
                float parentMO = std::max(1e-12f, OrientationMeasure(parent.cone));

                struct Best { float cost = std::numeric_limits<float>::infinity(); int axis = -1; float splitPos = 0.0f; } best;

                for (int axis = 0; axis < 3; ++axis)
                {
                    float mn = tmp[begin].centroid[axis], mx = mn;
                    for (uint32_t i = begin; i < end; ++i)
                    {
                        float v = tmp[i].centroid[axis];
                        mn = std::min(mn, v);
                        mx = std::max(mx, v);
                    }
                    float span = mx - mn;
                    if (span <= 1e-20f) continue;

                    std::vector<Agg> bins(kBuildBins);
                    float invSpan = 1.0f / span;
                    for (uint32_t i = begin; i < end; ++i)
                    {
                        float v = tmp[i].centroid[axis];
                        uint32_t bi = std::min(kBuildBins - 1u,
                            static_cast<uint32_t>(std::floor((v - mn) * invSpan * kBuildBins)));
                        AggAdd(bins[bi], tmp[i]);
                    }

                    std::vector<Agg> pref(kBuildBins), suff(kBuildBins);
                    for (uint32_t i = 0; i < kBuildBins; ++i)
                    {
                        pref[i] = (i == 0) ? bins[i] : pref[i - 1];
                        if (i != 0) AggMerge(pref[i], bins[i]);
                    }
                    for (int i = static_cast<int>(kBuildBins) - 1; i >= 0; --i)
                    {
                        suff[i] = (i == static_cast<int>(kBuildBins) - 1) ? bins[i] : suff[i + 1];
                        if (i != static_cast<int>(kBuildBins) - 1) AggMerge(suff[i], bins[i]);
                    }

                    // Kr regularizer: penalize splitting along short axes so
                    // thin flat clusters don't produce sliver children.
                    float lengthAxis = ext[axis];
                    float Kr = (lengthAxis > 1e-20f) ? (lenMax / lengthAxis) : 1e6f;

                    for (uint32_t s = 1; s < kBuildBins; ++s)
                    {
                        const Agg& L = pref[s - 1];
                        const Agg& R = suff[s];
                        if (!L.valid || !R.valid) continue;

                        float cost = Kr * (L.power * AabbSurfaceArea(L.a) * OrientationMeasure(L.cone)
                                         + R.power * AabbSurfaceArea(R.a) * OrientationMeasure(R.cone))
                                     / (parentMA * parentMO);
                        if (cost < best.cost)
                        {
                            best.cost = cost;
                            best.axis = axis;
                            best.splitPos = mn + span * static_cast<float>(s) / static_cast<float>(kBuildBins);
                        }
                    }
                }

                if (best.axis < 0 || !std::isfinite(best.cost)) return false;

                auto itMid = std::partition(tmp.begin() + begin, tmp.begin() + end,
                    [&](const TmpTri& t) { return t.centroid[best.axis] < best.splitPos; });
                uint32_t mid = static_cast<uint32_t>(itMid - tmp.begin());
                if (mid == begin || mid == end)
                {
                    mid = (begin + end) / 2;
                    std::nth_element(tmp.begin() + begin, tmp.begin() + mid, tmp.begin() + end,
                        [&](const TmpTri& A, const TmpTri& B) { return A.centroid[best.axis] < B.centroid[best.axis]; });
                }

                midOut = mid;
                return true;
            }

            void BuildNode(uint32_t nodeIdx, std::vector<TmpTri>& tmp, uint32_t begin, uint32_t end)
            {
                Agg parent{};
                for (uint32_t i = begin; i < end; ++i) AggAdd(parent, tmp[i]);
                WriteNodeAggregate(nodes[nodeIdx], parent);

                uint32_t count = end - begin;
                if (count <= kMaxLeafTris)
                {
                    MakeLeaf(nodeIdx, tmp, begin, end);
                    return;
                }

                uint32_t mid;
                if (!FindBinarySplit(tmp, begin, end, mid))
                {
                    MakeLeaf(nodeIdx, tmp, begin, end);
                    return;
                }

                // Split each half once more for a 4-wide node (halves small
                // enough to be leaves stay unsplit).
                struct Range { uint32_t b, e; };
                Range buckets[4];
                uint32_t bucketCount = 0;
                auto pushOrSplitOnce = [&](uint32_t b, uint32_t e)
                {
                    if (e <= b) return;
                    uint32_t mid2;
                    if (e - b > kMaxLeafTris && FindBinarySplit(tmp, b, e, mid2) && mid2 > b && mid2 < e)
                    {
                        buckets[bucketCount++] = {b, mid2};
                        buckets[bucketCount++] = {mid2, e};
                    }
                    else
                    {
                        buckets[bucketCount++] = {b, e};
                    }
                };
                pushOrSplitOnce(begin, mid);
                pushOrSplitOnce(mid, end);

                uint32_t firstChild = static_cast<uint32_t>(nodes.size());
                nodes[nodeIdx].meta.x = firstChild;
                nodes[nodeIdx].meta.y = bucketCount;
                nodes.resize(nodes.size() + bucketCount);

                for (uint32_t c = 0; c < bucketCount; ++c)
                    BuildNode(firstChild + c, tmp, buckets[c].b, buckets[c].e);

                // Children emit their leaf triangles contiguously in DFS
                // order, so the parent's range is the union of its children's
                // - needed by the shader's deterministic pdf re-descent.
                uint32_t triFirst = std::numeric_limits<uint32_t>::max();
                uint32_t triCount = 0;
                for (uint32_t c = 0; c < bucketCount; ++c)
                {
                    triFirst = std::min(triFirst, nodes[firstChild + c].meta.z);
                    triCount += nodes[firstChild + c].meta.w;
                }
                nodes[nodeIdx].meta.z = triFirst;
                nodes[nodeIdx].meta.w = triCount;
            }
        };
    }

    void LightTree::Build(const Scene& scene)
    {
        // Gather emissive triangles: weight = area * luminance(emission),
        // per-triangle cone = geometric normal, theta_o=0, theta_e=pi/2
        // (diffuse one-sided emitter); degenerate normals fall back to an
        // isotropic cone.
        std::vector<TmpTri> tmp;
        tmp.reserve(64);
        for (uint32_t i = 0; i < scene.triangles.size(); ++i)
        {
            const Triangle& t = scene.triangles[i];
            if (t.materialIndex >= scene.materials.size()) continue;
            const glm::vec3 Ke = scene.materials[t.materialIndex].emissive;
            if (Ke.x + Ke.y + Ke.z <= 0.0f) continue;

            glm::vec3 e1 = t.v1.position - t.v0.position;
            glm::vec3 e2 = t.v2.position - t.v0.position;
            glm::vec3 cr = glm::cross(e1, e2);
            float area = 0.5f * glm::length(cr);

            TmpTri tt;
            tt.sceneTri = i;
            tt.centroid = (t.v0.position + t.v1.position + t.v2.position) / 3.0f;
            tt.aabb.mn = glm::min(t.v0.position, glm::min(t.v1.position, t.v2.position));
            tt.aabb.mx = glm::max(t.v0.position, glm::max(t.v1.position, t.v2.position));
            tt.power = std::max(area, 1e-10f) * Luminance(Ke);
            if (area > 1e-20f)
            {
                tt.cone.axis = glm::normalize(cr);
                tt.cone.thetaO = 0.0f;
                tt.cone.thetaE = kHalfPi;
            }
            tmp.push_back(tt);
        }

        m_lightCount = static_cast<uint32_t>(tmp.size());

        Builder builder;
        if (!tmp.empty())
        {
            builder.Run(scene, tmp);
        }
        else
        {
            // Keep the SSBOs valid (non-empty) even with no lights; the
            // shader skips NEE entirely when lightInfo.x == 0.
            builder.nodes.emplace_back();
            builder.gpuTris.emplace_back();
            builder.triToLight.assign(std::max<size_t>(scene.triangles.size(), 1), kSentinel);
        }

        // Power-proportional CDF over the (leaf-list ordered) light
        // triangles, for the bidirectional kernels' receiver-independent
        // light selection (the tree's adaptive pdf would break the
        // recursive MIS quantities - see docs/ARCHITECTURE.md).
        std::vector<float> cdf(builder.gpuTris.size(), 1.0f);
        m_totalPower = 0.0f;
        for (const GPULightTriangle& t : builder.gpuTris) m_totalPower += std::max(t.emissionWeight.w, 0.0f);
        if (m_totalPower > 0.0f)
        {
            float accum = 0.0f;
            for (size_t i = 0; i < builder.gpuTris.size(); ++i)
            {
                accum += std::max(builder.gpuTris[i].emissionWeight.w, 0.0f);
                cdf[i] = accum / m_totalPower;
            }
            cdf.back() = 1.0f;
        }

        m_nodeBuffer.Upload(builder.nodes.data(), builder.nodes.size() * sizeof(GPULightTreeNode), GL_STATIC_DRAW);
        m_triBuffer.Upload(builder.gpuTris.data(), builder.gpuTris.size() * sizeof(GPULightTriangle), GL_STATIC_DRAW);
        m_mapBuffer.Upload(builder.triToLight.data(), builder.triToLight.size() * sizeof(uint32_t), GL_STATIC_DRAW);
        m_cdfBuffer.Upload(cdf.data(), cdf.size() * sizeof(float), GL_STATIC_DRAW);

        // Structural validation: every interior node's triangle range must
        // exactly cover its children's, and each child's range must be a
        // sub-interval of the parent's - the shader's deterministic pdf
        // re-descent silently returns 0 (breaking MIS) if this ever drifts.
        uint32_t rangeErrors = 0;
        for (size_t ni = 0; ni < builder.nodes.size(); ++ni)
        {
            const GPULightTreeNode& nd = builder.nodes[ni];
            if (nd.meta.y == 0) continue;
            uint32_t lo = 0xFFFFFFFFu, sum = 0;
            for (uint32_t c = 0; c < nd.meta.y; ++c)
            {
                const GPULightTreeNode& ch = builder.nodes[nd.meta.x + c];
                lo = std::min(lo, ch.meta.z);
                sum += ch.meta.w;
                if (ch.meta.z < nd.meta.z || ch.meta.z + ch.meta.w > nd.meta.z + nd.meta.w)
                    rangeErrors++;
            }
            if (lo != nd.meta.z || sum != nd.meta.w) rangeErrors++;
            // Children must tile the parent's interval contiguously - the
            // min/sum check above can pass with overlapping siblings.
            uint32_t cursor = nd.meta.z;
            for (uint32_t c = 0; c < nd.meta.y; ++c)
            {
                const GPULightTreeNode& ch = builder.nodes[nd.meta.x + c];
                if (ch.meta.z != cursor) rangeErrors++;
                cursor += ch.meta.w;
            }
        }
        if (rangeErrors > 0)
            ROYALGL_LOG_ERROR("LightTree: ", rangeErrors, " node range violations - pdf re-descent is broken!");

        ROYALGL_LOG_INFO("LightTree: built over ", m_lightCount, " emissive triangles (",
                         builder.nodes.size(), " nodes).");
    }

    void LightTree::BindAll() const
    {
        m_nodeBuffer.BindBase();
        m_triBuffer.BindBase();
        m_mapBuffer.BindBase();
        m_cdfBuffer.BindBase();
    }
}
