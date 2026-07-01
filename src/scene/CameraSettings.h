#pragma once

#include <string>

namespace RoyalGL
{
    enum class CameraMode
    {
        Pinhole,
        LensSystem
    };

    // Camera-model selection, layered on top of Camera (position/target/fov)
    // and, when CameraMode::LensSystem, on top of a LensSystem instance
    // owned by Application. Kept separate from RenderSettings because it's
    // camera-model state, not shading/sampling state, but follows the same
    // operator== dirty-checking convention.
    struct CameraSettings
    {
        CameraMode mode = CameraMode::Pinhole; // pinhole stays the default; lens mode is additive, not a replacement
        std::string activeLensPreset = "Tessar (Brendel, USP 2854889) f/2.8 100mm EFL";

        bool operator==(const CameraSettings& other) const
        {
            return mode == other.mode && activeLensPreset == other.activeLensPreset;
        }
        bool operator!=(const CameraSettings& other) const { return !(*this == other); }
    };
}
