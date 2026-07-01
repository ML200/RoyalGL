#pragma once

#include <filesystem>
#include <vector>

namespace RoyalGL
{
    class ImageExport
    {
    public:
        // Converts a linear HDR RGBA32F buffer to tonemapped (Reinhard) +
        // gamma-corrected 8-bit sRGB and writes a PNG. `rgba32f` must contain
        // width * height * 4 floats.
        static bool SavePNG(const std::filesystem::path& path,
                             const std::vector<float>& rgba32f,
                             int width, int height,
                             float exposure = 1.0f);
    };
}
