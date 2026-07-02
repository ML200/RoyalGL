#pragma once

#include <glm/glm.hpp>

namespace RoyalGL
{
    enum class MaterialType : int
    {
        Diffuse = 0,
        Glass = 1 // delta dielectric: perfect Fresnel reflection/refraction
    };

    // CPU-side material description, produced by GLTFLoader (or hand-built
    // for the fallback scene) and converted to GPUMaterial when uploaded.
    struct Material
    {
        glm::vec3 baseColor{0.8f, 0.8f, 0.8f};
        glm::vec3 emissive{0.0f, 0.0f, 0.0f};
        float metallic = 0.0f;
        float roughness = 0.5f;
        MaterialType type = MaterialType::Diffuse;
        float ior = 1.5f; // index of refraction, used by Glass only

        bool operator==(const Material& other) const
        {
            return baseColor == other.baseColor &&
                   emissive == other.emissive &&
                   metallic == other.metallic &&
                   roughness == other.roughness &&
                   type == other.type &&
                   ior == other.ior;
        }
        bool operator!=(const Material& other) const { return !(*this == other); }
    };
}
