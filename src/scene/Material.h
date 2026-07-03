#pragma once

#include <glm/glm.hpp>

namespace RoyalGL
{
    enum class MaterialType : int
    {
        Diffuse = 0,
        Glass = 1,           // delta dielectric: perfect Fresnel reflection/refraction
        Conductor = 2,       // GGX microfacet conductor, baseColor = F0
        RoughDielectric = 3, // GGX microfacet dielectric (Walter 2007), reflection+transmission
        Layered = 4          // position-free layered slab (Guo et al. 2018): rough
                             // dielectric coat + HG medium + conductor/diffuse base
    };

    // CPU-side material description, produced by GLTFLoader (or hand-built
    // for the fallback scene) and converted to GPUMaterial when uploaded.
    struct Material
    {
        glm::vec3 baseColor{0.8f, 0.8f, 0.8f};
        glm::vec3 emissive{0.0f, 0.0f, 0.0f};
        float metallic = 0.0f;  // Layered: base lobe blend diffuse->conductor
        float roughness = 0.5f; // Conductor/RoughDielectric lobe; Layered: base interface
        MaterialType type = MaterialType::Diffuse;
        float ior = 1.5f; // Glass / RoughDielectric index of refraction

        // Layered-only coat parameters (Guo et al. 2018 slab):
        float coatRoughness = 0.1f;              // top dielectric interface GGX roughness
        float coatIor = 1.5f;                    // top interface IOR
        float coatDepth = 0.0f;                  // medium optical depth tau (0 = clear coat)
        float coatG = 0.0f;                      // Henyey-Greenstein g of the medium
        glm::vec3 coatAlbedo{1.0f, 1.0f, 1.0f};  // medium single-scattering albedo (RGB)

        bool operator==(const Material& other) const
        {
            return baseColor == other.baseColor &&
                   emissive == other.emissive &&
                   metallic == other.metallic &&
                   roughness == other.roughness &&
                   type == other.type &&
                   ior == other.ior &&
                   coatRoughness == other.coatRoughness &&
                   coatIor == other.coatIor &&
                   coatDepth == other.coatDepth &&
                   coatG == other.coatG &&
                   coatAlbedo == other.coatAlbedo;
        }
        bool operator!=(const Material& other) const { return !(*this == other); }
    };
}
