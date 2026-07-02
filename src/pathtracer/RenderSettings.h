#pragma once

#include <glm/glm.hpp>
#include "optics/LensSystem.h"

namespace RoyalGL
{
    enum class CameraMode : int
    {
        Pinhole = 0,
        Lens = 1 // physical lens per Steinert et al. 2011
    };

    // User-tunable render settings, surfaced in the UI. Changing any of
    // these (compared via operator==) resets progressive accumulation.
    struct RenderSettings
    {
        int maxBounces = 6;
        float exposure = 1.0f;
        glm::vec3 backgroundColor{0.015f, 0.015f, 0.02f}; // much darker than the old sky-blue default, so small bright light sources actually pop
        float backgroundIntensity = 1.0f;
        int maxSamples = 0; // 0 = unlimited

        // Next-event estimation via the light tree, MIS-combined with BSDF
        // sampling (balance heuristic). Only affects the unidirectional
        // pipeline (bidir has its own s=1 strategy). Off = pure BSDF
        // sampling; both converge to the same image, which makes this a
        // useful A/B check.
        bool enableNEE = true;

        // Bidirectional path tracing (three compute passes, recursive MIS
        // weights - see docs/ARCHITECTURE.md). Converges to the same image
        // as the unidirectional pipeline; caustics through delta glass are
        // only reachable efficiently here (t=1 light tracing).
        bool enableBidir = true;

        // Physical lens camera (Steinert et al. 2011). Lens mode renders
        // through the unidirectional pipeline regardless of enableBidir -
        // exact BDPT MIS through a lens is future work.
        CameraMode cameraMode = CameraMode::Pinhole;
        LensSettings lens;

        bool operator==(const RenderSettings& other) const
        {
            return maxBounces == other.maxBounces &&
                   exposure == other.exposure &&
                   backgroundColor == other.backgroundColor &&
                   backgroundIntensity == other.backgroundIntensity &&
                   maxSamples == other.maxSamples &&
                   enableNEE == other.enableNEE &&
                   enableBidir == other.enableBidir &&
                   cameraMode == other.cameraMode &&
                   lens == other.lens;
        }
        bool operator!=(const RenderSettings& other) const { return !(*this == other); }
    };
}
