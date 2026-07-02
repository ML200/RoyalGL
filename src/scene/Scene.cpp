#include "scene/Scene.h"
#include "core/Log.h"

#include <cmath>

namespace RoyalGL
{
    namespace
    {
        // Appends a quad (a,b,c,d walked around the perimeter) as two
        // triangles, all vertices sharing the given explicit normal.
        void AddQuad(std::vector<Triangle>& triangles, const glm::vec3& a, const glm::vec3& b,
                     const glm::vec3& c, const glm::vec3& d, const glm::vec3& normal, uint32_t materialIndex)
        {
            Vertex va{a, normal};
            Vertex vb{b, normal};
            Vertex vc{c, normal};
            Vertex vd{d, normal};

            Triangle t0;
            t0.v0 = va;
            t0.v1 = vb;
            t0.v2 = vc;
            t0.materialIndex = materialIndex;

            Triangle t1;
            t1.v0 = va;
            t1.v1 = vc;
            t1.v2 = vd;
            t1.materialIndex = materialIndex;

            triangles.push_back(t0);
            triangles.push_back(t1);
        }

        // Appends an outward-wound UV sphere. The winding matters: it
        // defines the geometric normal, which is the emission side for
        // emissive materials (emitters are one-sided).
        void AddSphere(std::vector<Triangle>& triangles, const glm::vec3& center, float radius,
                       uint32_t materialIndex, int rings = 8, int segments = 12)
        {
            constexpr float kPi = 3.14159265358979323846f;
            auto point = [&](int ring, int seg)
            {
                float theta = kPi * static_cast<float>(ring) / static_cast<float>(rings);
                float phi = 2.0f * kPi * static_cast<float>(seg) / static_cast<float>(segments);
                glm::vec3 n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
                Vertex v;
                v.position = center + radius * n;
                v.normal = n;
                return v;
            };

            for (int i = 0; i < rings; ++i)
            {
                for (int j = 0; j < segments; ++j)
                {
                    Vertex p00 = point(i, j), p01 = point(i, j + 1);
                    Vertex p10 = point(i + 1, j), p11 = point(i + 1, j + 1);
                    Triangle t;
                    t.materialIndex = materialIndex;
                    if (i > 0) // upper triangle degenerates at the top pole
                    {
                        t.v0 = p00; t.v1 = p01; t.v2 = p11;
                        triangles.push_back(t);
                    }
                    if (i < rings - 1) // lower triangle degenerates at the bottom pole
                    {
                        t.v0 = p00; t.v1 = p11; t.v2 = p10;
                        triangles.push_back(t);
                    }
                }
            }
        }
    }

    void Scene::LoadFallbackScene()
    {
        triangles.clear();
        materials.clear();
        sourcePath.clear();

        materials.push_back(Material{glm::vec3(0.73f, 0.73f, 0.73f), glm::vec3(0.0f), 0.0f, 1.0f}); // 0: floor
        materials.push_back(Material{glm::vec3(0.65f, 0.05f, 0.05f), glm::vec3(0.0f), 0.0f, 1.0f}); // 1: left wall (red)
        materials.push_back(Material{glm::vec3(0.12f, 0.45f, 0.15f), glm::vec3(0.0f), 0.0f, 1.0f}); // 2: right wall (green)
        materials.push_back(Material{glm::vec3(0.73f, 0.73f, 0.73f), glm::vec3(0.0f), 0.0f, 1.0f}); // 3: back wall + ceiling
        materials.push_back(Material{glm::vec3(1.0f, 1.0f, 1.0f), glm::vec3(25.0f, 25.0f, 25.0f), 0.0f, 1.0f}); // 4: light
        materials.push_back(Material{glm::vec3(0.4f, 0.5f, 0.75f), glm::vec3(0.0f), 0.0f, 1.0f}); // 5: center box

        // Flip to strip the scene down to just the duck (plus whatever
        // else is added outside the gate).
        constexpr bool kBuildCornellBox = true;

        const float ext = 2.0f;    // room half-extent in X/Z
        const float top = 4.0f;    // ceiling height

        if (kBuildCornellBox)
        {
        // Floor (y=0), normal +Y.
        AddQuad(triangles,
                glm::vec3(-ext, 0.0f, -ext), glm::vec3(ext, 0.0f, -ext),
                glm::vec3(ext, 0.0f, ext), glm::vec3(-ext, 0.0f, ext),
                glm::vec3(0.0f, 1.0f, 0.0f), 0);

        // Left wall (x=-2), normal +X (faces into the room).
        AddQuad(triangles,
                glm::vec3(-ext, 0.0f, -ext), glm::vec3(-ext, 0.0f, ext),
                glm::vec3(-ext, top, ext), glm::vec3(-ext, top, -ext),
                glm::vec3(1.0f, 0.0f, 0.0f), 1);

        // Right wall (x=+2), normal -X.
        AddQuad(triangles,
                glm::vec3(ext, 0.0f, ext), glm::vec3(ext, 0.0f, -ext),
                glm::vec3(ext, top, -ext), glm::vec3(ext, top, ext),
                glm::vec3(-1.0f, 0.0f, 0.0f), 2);

        // Back wall (z=-2), normal +Z. No wall at z=+ext - that side stays open toward the camera.
        AddQuad(triangles,
                glm::vec3(-ext, 0.0f, -ext), glm::vec3(ext, 0.0f, -ext),
                glm::vec3(ext, top, -ext), glm::vec3(-ext, top, -ext),
                glm::vec3(0.0f, 0.0f, 1.0f), 3);

        // Ceiling (y=4), normal -Y, a single solid quad - the light panels
        // below sit just under it (own z-fighting-free Y) rather than in a
        // cut-out hole, so many small lights can be scattered across it
        // without carving a separate hole per light.
        AddQuad(triangles,
                glm::vec3(-ext, top, -ext), glm::vec3(ext, top, -ext),
                glm::vec3(ext, top, ext), glm::vec3(-ext, top, ext),
                glm::vec3(0.0f, -1.0f, 0.0f), 3);

        // A 3x3 grid of small emissive panels recessed just below the
        // ceiling.
        const float lightY = top - 0.02f;
        const float lightHalfSize = 0.12f;
        const int lightGridN = 3;
        const float lightSpacing = 1.1f;
        for (int gz = 0; gz < lightGridN; ++gz)
        {
            for (int gx = 0; gx < lightGridN; ++gx)
            {
                float cx = (gx - (lightGridN - 1) * 0.5f) * lightSpacing;
                float cz = (gz - (lightGridN - 1) * 0.5f) * lightSpacing;
                AddQuad(triangles,
                        glm::vec3(cx - lightHalfSize, lightY, cz - lightHalfSize),
                        glm::vec3(cx + lightHalfSize, lightY, cz - lightHalfSize),
                        glm::vec3(cx + lightHalfSize, lightY, cz + lightHalfSize),
                        glm::vec3(cx - lightHalfSize, lightY, cz + lightHalfSize),
                        glm::vec3(0.0f, -1.0f, 0.0f), 4);
            }
        }
        } // kBuildCornellBox

        if (kBuildCornellBox)
        {
        // Small box sitting on the floor (bottom face omitted - it is
        // coincident with the floor and never visible).
        const glm::vec3 boxMin(-1.0f, 0.0f, -0.2f);
        const glm::vec3 boxMax(-0.2f, 1.4f, 0.6f);

        AddQuad(triangles, // top
                glm::vec3(boxMin.x, boxMax.y, boxMin.z), glm::vec3(boxMax.x, boxMax.y, boxMin.z),
                glm::vec3(boxMax.x, boxMax.y, boxMax.z), glm::vec3(boxMin.x, boxMax.y, boxMax.z),
                glm::vec3(0.0f, 1.0f, 0.0f), 5);
        AddQuad(triangles, // +X face
                glm::vec3(boxMax.x, boxMin.y, boxMin.z), glm::vec3(boxMax.x, boxMin.y, boxMax.z),
                glm::vec3(boxMax.x, boxMax.y, boxMax.z), glm::vec3(boxMax.x, boxMax.y, boxMin.z),
                glm::vec3(1.0f, 0.0f, 0.0f), 5);
        AddQuad(triangles, // -X face
                glm::vec3(boxMin.x, boxMin.y, boxMax.z), glm::vec3(boxMin.x, boxMin.y, boxMin.z),
                glm::vec3(boxMin.x, boxMax.y, boxMin.z), glm::vec3(boxMin.x, boxMax.y, boxMax.z),
                glm::vec3(-1.0f, 0.0f, 0.0f), 5);
        AddQuad(triangles, // +Z face
                glm::vec3(boxMax.x, boxMin.y, boxMax.z), glm::vec3(boxMin.x, boxMin.y, boxMax.z),
                glm::vec3(boxMin.x, boxMax.y, boxMax.z), glm::vec3(boxMax.x, boxMax.y, boxMax.z),
                glm::vec3(0.0f, 0.0f, 1.0f), 5);
        AddQuad(triangles, // -Z face
                glm::vec3(boxMin.x, boxMin.y, boxMin.z), glm::vec3(boxMax.x, boxMin.y, boxMin.z),
                glm::vec3(boxMax.x, boxMax.y, boxMin.z), glm::vec3(boxMin.x, boxMax.y, boxMin.z),
                glm::vec3(0.0f, 0.0f, -1.0f), 5);
        } // kBuildCornellBox

        camera.target = glm::vec3(0.0f, 1.5f, 0.0f);
        camera.position = glm::vec3(0.0f, 1.8f, 6.5f);

        ROYALGL_LOG_INFO("Scene: loaded fallback Cornell-box scene - ", triangles.size(), " triangles, ",
                          materials.size(), " materials.");
    }

    glm::vec3 Scene::BoundsMin() const
    {
        if (triangles.empty())
            return glm::vec3(0.0f);

        glm::vec3 minBounds = triangles[0].v0.position;
        for (const Triangle& tri : triangles)
            for (const Vertex* v : {&tri.v0, &tri.v1, &tri.v2})
                minBounds = glm::min(minBounds, v->position);
        return minBounds;
    }

    glm::vec3 Scene::BoundsMax() const
    {
        if (triangles.empty())
            return glm::vec3(0.0f);

        glm::vec3 maxBounds = triangles[0].v0.position;
        for (const Triangle& tri : triangles)
            for (const Vertex* v : {&tri.v0, &tri.v1, &tri.v2})
                maxBounds = glm::max(maxBounds, v->position);
        return maxBounds;
    }

    std::vector<GPUTriangle> Scene::BuildGPUTriangles() const
    {
        std::vector<GPUTriangle> gpuTriangles;
        gpuTriangles.reserve(triangles.size());

        for (const Triangle& tri : triangles)
        {
            GPUTriangle gpuTri;
            gpuTri.p0 = glm::vec4(tri.v0.position, 0.0f);
            gpuTri.p1 = glm::vec4(tri.v1.position, 0.0f);
            gpuTri.p2 = glm::vec4(tri.v2.position, 0.0f);
            gpuTri.n0 = glm::vec4(tri.v0.normal, 0.0f);
            gpuTri.n1 = glm::vec4(tri.v1.normal, 0.0f);
            gpuTri.n2 = glm::vec4(tri.v2.normal, 0.0f);
            gpuTri.uv0_uv1 = glm::vec4(tri.v0.uv, tri.v1.uv);
            gpuTri.uv2_material = glm::vec4(tri.v2.uv, static_cast<float>(tri.materialIndex), 0.0f);
            gpuTriangles.push_back(gpuTri);
        }

        return gpuTriangles;
    }

    void Scene::MergeInstance(const Scene& other, const glm::vec3& floorCenter, float targetHeight,
                               const Material& material)
    {
        if (other.triangles.empty()) return;

        glm::vec3 mn = other.BoundsMin();
        glm::vec3 mx = other.BoundsMax();
        glm::vec3 size = mx - mn;
        float scale = (size.y > 1e-6f) ? targetHeight / size.y : 1.0f;
        glm::vec3 bottomCenter = glm::vec3((mn.x + mx.x) * 0.5f, mn.y, (mn.z + mx.z) * 0.5f);

        uint32_t materialIndex = static_cast<uint32_t>(materials.size());
        materials.push_back(material);

        triangles.reserve(triangles.size() + other.triangles.size());
        auto xform = [&](const Vertex& v)
        {
            Vertex out = v;
            out.position = (v.position - bottomCenter) * scale + floorCenter;
            // Uniform scale + translation: normals are unchanged.
            return out;
        };
        for (const Triangle& t : other.triangles)
        {
            Triangle nt;
            nt.v0 = xform(t.v0);
            nt.v1 = xform(t.v1);
            nt.v2 = xform(t.v2);
            nt.materialIndex = materialIndex;
            triangles.push_back(nt);
        }

        ROYALGL_LOG_INFO("Scene: merged instance with ", other.triangles.size(),
                         " triangles at scale ", scale, ".");
    }

    std::vector<GPUMaterial> Scene::BuildGPUMaterials() const
    {
        if (materials.empty())
        {
            GPUMaterial defaultMat;
            defaultMat.baseColor = glm::vec4(0.8f, 0.8f, 0.8f, 0.0f);
            defaultMat.emissive = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
            defaultMat.params = glm::vec4(0.0f, 0.5f, 0.0f, 0.0f);
            return {defaultMat};
        }

        std::vector<GPUMaterial> gpuMaterials;
        gpuMaterials.reserve(materials.size());

        for (const Material& mat : materials)
        {
            GPUMaterial gpuMat;
            gpuMat.baseColor = glm::vec4(mat.baseColor, 0.0f);
            gpuMat.emissive = glm::vec4(mat.emissive, 0.0f);
            gpuMat.params = glm::vec4(mat.metallic, mat.roughness, mat.ior, static_cast<float>(mat.type));
            gpuMaterials.push_back(gpuMat);
        }

        return gpuMaterials;
    }
}
