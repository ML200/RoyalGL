#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include "optics/LensSystem.h"

namespace RoyalGL
{
    class LensPrescription
    {
    public:
        // Parses a .lens text file into `outSystem`. Returns false (leaving
        // outSystem untouched) on any parse error or unknown glass name;
        // check the log. See docs/ARCHITECTURE.md for the file format.
        static bool Load(const std::filesystem::path& path, LensSystem& outSystem);

        // Inverse of Load - writes `system` back to the same text format.
        static bool Save(const std::filesystem::path& path, const LensSystem& system);

        // The exact Tessar prescription from Steinert et al. 2011 Figure 4
        // (Brendel, USP 2854889, f/2.8, 100mm EFL), with (nd, Vd) glass
        // values cross-checked against Johannes Hanika's own lens-tracing
        // renderer data (see optics/Glass.h) - the only preset shipped
        // pre-verified; the file format itself is fully general, so any
        // other prescription can be pasted in via Load().
        static LensSystem BuiltinTessar();

        static std::vector<std::string> BuiltinPresetNames();
        static LensSystem LoadBuiltinPreset(const std::string& name);
    };
}
