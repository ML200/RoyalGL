#include "pathtracer/LightList.h"
#include "core/Log.h"

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <vector>

namespace RoyalGL
{
    void LightList::Build(const Scene& scene)
    {
        std::vector<GPULightTriangle> lights;
        std::vector<float> cdf;
        float running = 0.0f;

        for (const Triangle& tri : scene.triangles)
        {
            if (tri.materialIndex >= scene.materials.size()) continue;
            const Material& mat = scene.materials[tri.materialIndex];
            if (mat.emissive.r <= 0.0f && mat.emissive.g <= 0.0f && mat.emissive.b <= 0.0f) continue;

            glm::vec3 e1 = tri.v1.position - tri.v0.position;
            glm::vec3 e2 = tri.v2.position - tri.v0.position;
            glm::vec3 crossProd = glm::cross(e1, e2);
            float area = 0.5f * glm::length(crossProd);
            if (area <= 1e-9f) continue; // degenerate triangle

            glm::vec3 normal = crossProd / (2.0f * area);
            // Power proxy: radiance luminance * area * pi (Lambertian
            // emitter total flux over the hemisphere). Used only as a
            // power-proportional sampling weight, not required to be
            // radiometrically exact - see docs/ARCHITECTURE.md for why
            // power-proportional (not uniform) light selection matters here.
            float luminance = 0.2126f * mat.emissive.r + 0.7152f * mat.emissive.g + 0.0722f * mat.emissive.b;
            float power = luminance * area * 3.14159265f;

            GPULightTriangle gl{};
            gl.p0 = glm::vec4(tri.v0.position, 0.0f);
            gl.p1 = glm::vec4(tri.v1.position, 0.0f);
            gl.p2 = glm::vec4(tri.v2.position, 0.0f);
            gl.normal_area = glm::vec4(normal, area);
            gl.emissive = glm::vec4(mat.emissive, 0.0f);
            lights.push_back(gl);

            running += power;
            cdf.push_back(running);
        }

        m_lightCount = static_cast<uint32_t>(lights.size());

        if (m_lightCount == 0)
        {
            ROYALGL_LOG_INFO("LightList: scene has no emissive triangles; flare/ghost pass will be a no-op.");
            return;
        }

        for (float& c : cdf) c /= running;

        m_lightBuffer.Upload(lights.data(), lights.size() * sizeof(GPULightTriangle), GL_STATIC_DRAW);
        m_cdfBuffer.Upload(cdf.data(), cdf.size() * sizeof(float), GL_STATIC_DRAW);

        ROYALGL_LOG_INFO("LightList: found ", m_lightCount, " emissive triangles for the flare/ghost pass.");
    }

    void LightList::BindAll() const
    {
        m_lightBuffer.BindBase();
        m_cdfBuffer.BindBase();
    }
}
