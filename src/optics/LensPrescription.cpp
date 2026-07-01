#include "optics/LensPrescription.h"
#include "optics/Glass.h"
#include "core/Log.h"

#include <fstream>
#include <sstream>

namespace RoyalGL
{
    bool LensPrescription::Load(const std::filesystem::path& path, LensSystem& outSystem)
    {
        std::ifstream file(path);
        if (!file)
        {
            ROYALGL_LOG_ERROR("LensPrescription: failed to open '", path.string(), "'");
            return false;
        }

        LensSystem system;
        std::string line;
        while (std::getline(file, line))
        {
            size_t firstNonSpace = line.find_first_not_of(" \t\r\n");
            if (firstNonSpace == std::string::npos) continue; // blank line
            if (line[firstNonSpace] == '#') continue;         // comment line

            std::istringstream ss(line);
            std::string key;
            ss >> key;
            if (key.empty()) continue;

            if (key == "name")
            {
                std::string rest;
                std::getline(ss, rest);
                size_t start = rest.find_first_not_of(" \t");
                system.name = (start == std::string::npos) ? "" : rest.substr(start);
            }
            else if (key == "efl_mm")
            {
                ss >> system.effectiveFocalLengthMm;
            }
            else if (key == "fstop")
            {
                ss >> system.fStop;
            }
            else if (key == "sensor_mm")
            {
                ss >> system.sensorWidthMm >> system.sensorHeightMm;
            }
            else
            {
                LensSurface surf;
                try
                {
                    surf.radiusMm = std::stod(key);
                }
                catch (const std::exception&)
                {
                    ROYALGL_LOG_ERROR("LensPrescription: malformed line in '", path.string(), "': ", line);
                    return false;
                }
                ss >> surf.thicknessMm >> surf.material >> surf.semiDiameterMm;
                if (!ss)
                {
                    ROYALGL_LOG_ERROR("LensPrescription: malformed surface row in '", path.string(), "': ", line);
                    return false;
                }
                std::string apertureTag;
                if (ss >> apertureTag && apertureTag == "aperture")
                {
                    surf.isAperture = true;
                    surf.radiusMm = 0.0;
                    surf.material = "air";
                }
                if (GlassCatalog::Find(surf.material) == nullptr)
                {
                    ROYALGL_LOG_ERROR("LensPrescription: unknown material '", surf.material, "' in '", path.string(), "'");
                    return false;
                }
                system.surfaces.push_back(surf);
            }
        }

        if (system.surfaces.empty())
        {
            ROYALGL_LOG_ERROR("LensPrescription: '", path.string(), "' defines no surfaces.");
            return false;
        }

        outSystem = std::move(system);
        return true;
    }

    bool LensPrescription::Save(const std::filesystem::path& path, const LensSystem& system)
    {
        std::ofstream file(path);
        if (!file)
        {
            ROYALGL_LOG_ERROR("LensPrescription: failed to open '", path.string(), "' for writing.");
            return false;
        }

        file << "# RoyalGL lens prescription\n";
        file << "name       " << system.name << "\n";
        file << "efl_mm     " << system.effectiveFocalLengthMm << "\n";
        file << "fstop      " << system.fStop << "\n";
        file << "sensor_mm  " << system.sensorWidthMm << " " << system.sensorHeightMm << "\n\n";
        file << "# radius_mm  thickness_mm  material  semi_diameter_mm\n";
        for (const LensSurface& s : system.surfaces)
        {
            file << "  " << s.radiusMm << "  " << s.thicknessMm << "  " << s.material << "  " << s.semiDiameterMm;
            if (s.isAperture) file << "  aperture";
            file << "\n";
        }
        return true;
    }

    LensSystem LensPrescription::BuiltinTessar()
    {
        LensSystem s;
        s.name = "Tessar (Brendel, USP 2854889) f/2.8 100mm EFL";
        s.effectiveFocalLengthMm = 100.0;
        s.fStop = 2.8;
        s.surfaces = {
            {42.970, 9.8, 19.2, "LAK9", false},
            {-115.330, 2.1, 19.2, "LLF7", false},
            {306.840, 4.16, 19.2, "air", false},
            {0.0, 4.0, 15.0, "air", true},
            {-59.060, 1.870, 17.3, "SF7", false},
            {40.930, 10.640, 17.3, "air", false},
            {183.920, 7.050, 16.5, "LAK9", false},
            {-48.910, 79.831, 16.5, "air", false},
        };
        return s;
    }

    std::vector<std::string> LensPrescription::BuiltinPresetNames()
    {
        return {"Tessar (Brendel, USP 2854889) f/2.8 100mm EFL"};
    }

    LensSystem LensPrescription::LoadBuiltinPreset(const std::string& name)
    {
        // Only one built-in preset ships pre-verified for now (see
        // docs/ARCHITECTURE.md); fall back to it regardless of `name` so
        // callers always get a valid lens system rather than an empty one.
        (void)name;
        return BuiltinTessar();
    }
}
