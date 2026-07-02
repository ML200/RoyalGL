#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "gfx/Buffer.h"

namespace RoyalGL
{
    // One row of a lens prescription table, exactly as printed in Steinert
    // et al. 2011 Fig. 4: signed radius (positive = center of curvature on
    // the image side), axial thickness to the next surface, the medium
    // filling that gap, and the element's semi-diameter. The aperture stop
    // is a flat row flagged isAperture.
    struct LensRow
    {
        float radiusMm = 0.0f;
        float thicknessMm = 0.0f;
        std::string material = "air";
        float semiDiameterMm = 0.0f;
        bool isAperture = false;
    };

    // User-tunable lens state, kept in RenderSettings so the existing
    // dirty-check drives re-derivation.
    struct LensSettings
    {
        float fNumber = 2.8f;
        float focusShiftMm = 0.0f;   // extra sensor-to-rear-vertex distance
        float scale = 0.4f;          // uniform prescription scale (scales EFL)
        int apertureBlades = 6;      // 0 = circular
        float bladeRotationDeg = 0.0f;
        float sensorHeightMm = 24.0f;
        // Light-traced lens flares (Fresnel ghosts) in the BDPT t=1 lens
        // connection - the paper's sec. 4.3 architecture. Ghost paths are
        // rare stochastic branches, so each connection runs `flareSamples`
        // extra walks (sharing one shadow ray and entry point, differing
        // only in the reflect/refract branches and the wavelength).
        bool enableFlare = false;
        int flareSamples = 8;
        // Artist multiplier on flared splats only - uncoated 2-bounce
        // ghosts carry ~0.2% of the source flux, which can need a boost to
        // read against a bright image.
        float flareIntensity = 1.0f;
        // Aperture diffraction glare streaks (paper sec. 4.4), a stochastic
        // branch near blade edges inside the flare walks. The coefficient's
        // absolute scale is not independently verified (the paper's own
        // caveat), hence the artist intensity multiplier.
        bool enableDiffraction = true;
        float diffractionIntensity = 1.0f;
        float diffractionEdgeWidthMm = 0.05f;
        // Index into Application's scan of assets/lenses/*.lens.
        int presetIndex = 0;

        bool operator==(const LensSettings& o) const
        {
            return fNumber == o.fNumber && focusShiftMm == o.focusShiftMm && scale == o.scale &&
                   apertureBlades == o.apertureBlades && bladeRotationDeg == o.bladeRotationDeg &&
                   sensorHeightMm == o.sensorHeightMm && enableFlare == o.enableFlare &&
                   flareSamples == o.flareSamples && flareIntensity == o.flareIntensity &&
                   enableDiffraction == o.enableDiffraction &&
                   diffractionIntensity == o.diffractionIntensity &&
                   diffractionEdgeWidthMm == o.diffractionEdgeWidthMm &&
                   presetIndex == o.presetIndex;
        }
        bool operator!=(const LensSettings& o) const { return !(*this == o); }
    };

    // Full lens system per Steinert et al. 2011: prescription + derived GPU
    // data. Derive() bakes the walk-ordered (rear -> front) surface records
    // with per-medium Sellmeier/Cauchy coefficients; the shader does exact
    // per-wavelength Snell refraction through them (shaders/lens_common.glsl).
    class LensSystem
    {
    public:
        // The paper's Fig. 4 prescription: Tessar by Brendel (USP 2854889),
        // f/2.8, 100mm EFL.
        void LoadBuiltinTessar();
        bool LoadLensFile(const std::filesystem::path& path);

        // Re-derives GPU records from the prescription and settings and
        // uploads them (SSBO binding 13).
        void Derive(const LensSettings& settings);
        void Bind() const { m_surfaceBuffer.BindBase(); }

        float FrontVertexZMm() const { return m_frontVertexZMm; }
        float RearVertexZMm() const { return m_rearVertexZMm; }
        float RearSemiDiameterMm() const { return m_rearSemiDiameterMm; }
        float FrontSemiDiameterMm() const { return m_frontSemiDiameterMm; }
        // Paraxial effective focal length at the d line, after scaling -
        // drives the effective-pinhole camera pdf the BDPT MIS weights use.
        float EffectiveFocalLengthMm() const { return m_eflMm; }
        uint32_t SurfaceCount() const { return static_cast<uint32_t>(m_rows.size()); }
        const std::string& Name() const { return m_name; }

    private:
        std::vector<LensRow> m_rows; // file order: front element first
        std::string m_name = "none";
        float m_baseFNumber = 2.8f;  // f-number the prescription's stop radius corresponds to
        float m_frontVertexZMm = 0.0f;
        float m_rearVertexZMm = 0.0f;
        float m_rearSemiDiameterMm = 0.0f;
        float m_frontSemiDiameterMm = 0.0f;
        float m_eflMm = 50.0f;
        Buffer m_surfaceBuffer{BufferType::ShaderStorage, 13};
    };
}
