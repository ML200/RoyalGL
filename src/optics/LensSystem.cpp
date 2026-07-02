#include "optics/LensSystem.h"
#include "optics/Glass.h"
#include "core/Log.h"
#include "gfx/GPUTypes.h"

#include <fstream>
#include <sstream>

namespace RoyalGL
{
    void LensSystem::LoadBuiltinTessar()
    {
        // Steinert et al. 2011, Fig. 4 - Tessar by Brendel (USP 2854889),
        // f/2.8, 100mm EFL. Rows are front element -> sensor side.
        m_rows = {
            {42.970f, 9.8f, "LAK9", 19.2f, false},
            {-115.33f, 2.1f, "LLF7", 19.2f, false},
            {306.840f, 4.16f, "air", 19.2f, false},
            {0.0f, 4.0f, "air", 15.0f, true}, // aperture stop
            {-59.060f, 1.870f, "SF7", 17.3f, false},
            {40.930f, 10.640f, "air", 17.3f, false},
            {183.920f, 7.050f, "LAK9", 16.5f, false},
            {-48.910f, 79.831f, "air", 16.5f, false},
        };
        m_name = "Tessar f/2.8 100mm (Brendel, USP 2854889)";
        m_baseFNumber = 2.8f;
    }

    bool LensSystem::LoadLensFile(const std::filesystem::path& path)
    {
        // Line format mirroring the paper's table: `radius thickness
        // material semiDiameter`, with `aperture` in the radius column for
        // the stop row. `#` comments; a `name:`/`fnumber:` header.
        std::ifstream file(path);
        if (!file) return false;

        std::vector<LensRow> rows;
        std::string name = path.stem().string();
        float baseF = 2.8f;

        std::string line;
        while (std::getline(file, line))
        {
            size_t hash = line.find('#');
            if (hash != std::string::npos) line = line.substr(0, hash);
            std::istringstream ss(line);
            std::string first;
            if (!(ss >> first)) continue;

            if (first == "name:")
            {
                std::getline(ss, name);
                continue;
            }
            if (first == "fnumber:")
            {
                ss >> baseF;
                continue;
            }

            LensRow row;
            if (first == "aperture")
            {
                row.isAperture = true;
                row.radiusMm = 0.0f;
                if (!(ss >> row.thicknessMm >> row.semiDiameterMm)) return false;
                row.material = "air";
            }
            else
            {
                row.radiusMm = std::stof(first);
                if (!(ss >> row.thicknessMm >> row.material >> row.semiDiameterMm)) return false;
            }
            rows.push_back(row);
        }

        if (rows.empty()) return false;
        m_rows = std::move(rows);
        m_name = name;
        m_baseFNumber = baseF;
        ROYALGL_LOG_INFO("LensSystem: loaded '", m_name, "' (", m_rows.size(), " surfaces).");
        return true;
    }

    void LensSystem::Derive(const LensSettings& settings)
    {
        if (m_rows.empty()) LoadBuiltinTessar();
        const size_t n = m_rows.size();

        // Axial vertex positions in the tracing frame: sensor plane at z=0,
        // +z toward the scene. Row i's thickness is the gap on its image
        // side, so the rear vertex sits at the last row's thickness (plus
        // focus shift); everything scales uniformly (the paper: "all values
        // are scaled with the same factor" to change the focal length).
        float s = settings.scale;
        std::vector<float> zVertex(n);
        float z = m_rows[n - 1].thicknessMm * s + settings.focusShiftMm;
        for (size_t i = n; i-- > 0;)
        {
            zVertex[i] = z;
            if (i > 0) z += m_rows[i - 1].thicknessMm * s;
        }
        m_frontVertexZMm = zVertex[0];
        m_rearVertexZMm = zVertex[n - 1];
        m_rearSemiDiameterMm = m_rows[n - 1].semiDiameterMm * s;
        m_frontSemiDiameterMm = m_rows[0].semiDiameterMm * s;

        // Paraxial EFL at the d line (587.6nm): trace a parallel marginal
        // ray (h=1, u=0) front to back; EFL = -h0 / u_exit. Scales linearly
        // with the prescription scale.
        {
            float h = 1.0f, u = 0.0f;
            float nPrev = 1.0f;
            for (size_t i = 0; i < n; ++i)
            {
                const LensRow& row = m_rows[i];
                float nNext = Glass::Lookup(row.material).Eta(587.6f);
                if (!row.isAperture && std::fabs(row.radiusMm) > 1e-6f)
                {
                    float phi = (nNext - nPrev) / row.radiusMm;
                    u = (nPrev * u - h * phi) / nNext;
                }
                h += u * row.thicknessMm;
                nPrev = nNext;
            }
            m_eflMm = (std::fabs(u) > 1e-9f) ? (-1.0f / u) * s : 50.0f * s;
        }

        // f-number: the prescription's stop radius corresponds to its design
        // f-number; entrance pupil diameter scales linearly with the stop
        // (paraxial pupil imaging), so N = f/d_entr (paper Eq. 5) is honored
        // by scaling the stop radius with baseF/N.
        float stopScale = m_baseFNumber / std::max(settings.fNumber, 0.1f);

        // GPU records in WALK order (rear -> front): crossing record k while
        // moving frontward leaves mediumA (image side) and enters mediumB
        // (object side); after an odd number of flare reflections the roles
        // swap symmetrically.
        auto packMedium = [](const Glass& g, glm::vec4& coef, glm::vec4& coef2)
        {
            if (g.sellmeier)
            {
                coef = glm::vec4(g.B[0], g.B[1], g.B[2], 1.0f);
                coef2 = glm::vec4(g.C[0], g.C[1], g.C[2], 0.0f);
            }
            else
            {
                coef = glm::vec4(g.cauchyB, 0.0f, 0.0f, 0.0f);
                coef2 = glm::vec4(0.0f, 0.0f, 0.0f, g.cauchyA);
            }
        };

        std::vector<GPULensSurface> recs(n);
        for (size_t k = 0; k < n; ++k)
        {
            size_t i = n - 1 - k; // file-order row
            const LensRow& row = m_rows[i];
            GPULensSurface& r = recs[k];

            r.geo = glm::vec4(zVertex[i], row.radiusMm * s, row.semiDiameterMm * s,
                              row.isAperture ? 1.0f : 0.0f);

            Glass mediumA = Glass::Lookup(row.material);                       // image side of surface i
            Glass mediumB = (i > 0) ? Glass::Lookup(m_rows[i - 1].material)    // object side
                                    : Glass::Air();
            packMedium(mediumA, r.mediumA, r.mediumA2);
            packMedium(mediumB, r.mediumB, r.mediumB2);

            r.aperture = glm::vec4(static_cast<float>(settings.apertureBlades),
                                   glm::radians(settings.bladeRotationDeg),
                                   row.isAperture ? row.semiDiameterMm * s * stopScale : 0.0f,
                                   0.0f);
        }

        m_surfaceBuffer.Upload(recs.data(), recs.size() * sizeof(GPULensSurface), GL_STATIC_DRAW);
    }
}
