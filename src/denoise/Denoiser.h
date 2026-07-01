#pragma once

#include <vector>

namespace RoyalGL
{
    // Thin wrapper around Intel Open Image Denoise. When ROYALGL_HAS_OIDN is
    // not defined at compile time (no OIDN package found by CMake), this
    // compiles to a no-op passthrough, so the rest of the app never needs to
    // #ifdef around it.
    class Denoiser
    {
    public:
        Denoiser();
        ~Denoiser();

        Denoiser(const Denoiser&) = delete;
        Denoiser& operator=(const Denoiser&) = delete;

        static bool IsAvailable();

        // `color` is a tightly packed RGBA32F buffer of width*height pixels
        // (alpha is ignored, and left unchanged in the output). Returns a
        // denoised buffer of the same size, or a copy of `color` unchanged
        // if OIDN isn't available.
        std::vector<float> Denoise(const std::vector<float>& color, int width, int height);

    private:
        struct Impl;
        Impl* m_impl = nullptr;
    };
}
