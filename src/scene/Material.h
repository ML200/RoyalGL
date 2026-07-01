#pragma once

#include <glm/glm.hpp>

namespace RoyalGL
{
    // CPU-side material description, produced by GLTFLoader (or hand-built
    // for the fallback scene) and converted to GPUMaterial when uploaded.
    struct Material
    {
        glm::vec3 baseColor{0.8f, 0.8f, 0.8f};
        glm::vec3 emissive{0.0f, 0.0f, 0.0f};
        float metallic = 0.0f;
        float roughness = 0.5f;

        bool operator==(const Material& other) const
        {
            return baseColor == other.baseColor &&
                   emissive == other.emissive &&
                   metallic == other.metallic &&
                   roughness == other.roughness;
        }
        bool operator!=(const Material& other) const { return !(*this == other); }
    };
}
