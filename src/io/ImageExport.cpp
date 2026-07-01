#include "io/ImageExport.h"
#include "core/Log.h"

#include <algorithm>
#include <cmath>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace RoyalGL
{
    bool ImageExport::SavePNG(const std::filesystem::path& path,
                               const std::vector<float>& rgba32f,
                               int width, int height,
                               float exposure)
    {
        if (rgba32f.size() < static_cast<size_t>(width) * height * 4)
        {
            ROYALGL_LOG_ERROR("ImageExport: buffer too small for ", width, "x", height);
            return false;
        }

        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);

        for (int i = 0; i < width * height; ++i)
        {
            const float* src = &rgba32f[static_cast<size_t>(i) * 4];
            unsigned char* dst = &pixels[static_cast<size_t>(i) * 4];

            for (int c = 0; c < 3; ++c)
            {
                float value = src[c] * exposure;
                value = value / (1.0f + value);
                value = std::pow(value, 1.0f / 2.2f);
                dst[c] = static_cast<unsigned char>(std::round(std::clamp(value, 0.0f, 1.0f) * 255.0f));
            }
            dst[3] = 255;
        }

        std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent);

        if (stbi_write_png(path.string().c_str(), width, height, 4, pixels.data(), width * 4) == 0)
        {
            ROYALGL_LOG_ERROR("ImageExport: failed to write PNG: ", path.string());
            return false;
        }

        return true;
    }
}
