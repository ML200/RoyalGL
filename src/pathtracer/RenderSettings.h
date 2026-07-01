#pragma once

#include <glm/glm.hpp>

namespace RoyalGL
{
    // User-tunable render settings, surfaced in the UI. Changing any of
    // these (compared via operator==) resets progressive accumulation.
    struct RenderSettings
    {
        int maxBounces = 6;
        float exposure = 1.0f;
        glm::vec3 backgroundColor{0.015f, 0.015f, 0.02f}; // much darker than the old sky-blue default, so small bright light sources actually pop
        float backgroundIntensity = 1.0f;
        int maxSamples = 0; // 0 = unlimited

        // Lens flare / ghost (bidirectional light-tracing) pass - see
        // pathtracer/LensFlare.h and docs/ARCHITECTURE.md.
        bool enableFlare = true;
        int flareSamplesPerFrame = 65536;
        // Artist-facing calibration multiplier, same rationale as
        // diffractionIntensity below: the physically-derived connection
        // throughput (flux through light-to-aperture, converted to a
        // per-pixel-area value) is dimensionally correct but its absolute
        // scale depends on scene-specific factors (light brightness/size,
        // distance, lens aperture) that make photographically-plausible
        // ghosts easy to under- or over-shoot; this compensates rather than
        // asking every scene to hand-tune emissive intensities instead.
        float flareIntensity = 60.0f;

        // Aperture diffraction (Keller cone glare streaks), a stochastic
        // branch inside the flare pass's aperture-stop handling.
        bool enableDiffraction = true;
        float diffractionEdgeEpsilonMM = 0.05f;
        float diffractionBranchProbability = 0.5f;
        float diffractionIntensity = 1.0f;

        bool operator==(const RenderSettings& other) const
        {
            return maxBounces == other.maxBounces &&
                   exposure == other.exposure &&
                   backgroundColor == other.backgroundColor &&
                   backgroundIntensity == other.backgroundIntensity &&
                   maxSamples == other.maxSamples &&
                   enableFlare == other.enableFlare &&
                   flareSamplesPerFrame == other.flareSamplesPerFrame &&
                   flareIntensity == other.flareIntensity &&
                   enableDiffraction == other.enableDiffraction &&
                   diffractionEdgeEpsilonMM == other.diffractionEdgeEpsilonMM &&
                   diffractionBranchProbability == other.diffractionBranchProbability &&
                   diffractionIntensity == other.diffractionIntensity;
        }
        bool operator!=(const RenderSettings& other) const { return !(*this == other); }
    };
}
