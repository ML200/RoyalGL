#include "optics/LensSystem.h"
#include "optics/Glass.h"
#include "core/Log.h"

#include <GL/glew.h>
#include <algorithm>
#include <cmath>

namespace RoyalGL
{
    namespace
    {
        // v1 focus model: a simple "unit focusing" approximation - the last
        // air gap (lens rear group to sensor) is scaled per the thin-lens
        // equation so an object at `focusDistanceMm` forms a sharp image,
        // while every other gap only receives the user's overall `scale`
        // factor. This is a deliberate simplification (see docs/ARCHITECTURE.md);
        // a full through-focus solve is a documented extension point.
        double FocusScaleForLastGap(double efl, double focusDistanceMm)
        {
            if (efl <= 0.0 || focusDistanceMm <= efl * 1.001) return 1.0;
            double designImageDist = efl;
            double focusedImageDist = 1.0 / (1.0 / efl - 1.0 / focusDistanceMm);
            return focusedImageDist / designImageDist;
        }
    }

    LensSystem::LensSystem(const LensSystem& other)
        : surfaces(other.surfaces), name(other.name), effectiveFocalLengthMm(other.effectiveFocalLengthMm),
          apertureBlades(other.apertureBlades), apertureBladeRotationDeg(other.apertureBladeRotationDeg),
          fStop(other.fStop), focusDistanceMm(other.focusDistanceMm), sensorWidthMm(other.sensorWidthMm),
          sensorHeightMm(other.sensorHeightMm), scale(other.scale)
        // m_surfaceBuffer intentionally default-constructed fresh, not copied - see LensSystem.h.
    {
    }

    LensSystem& LensSystem::operator=(const LensSystem& other)
    {
        if (this != &other)
        {
            surfaces = other.surfaces;
            name = other.name;
            effectiveFocalLengthMm = other.effectiveFocalLengthMm;
            apertureBlades = other.apertureBlades;
            apertureBladeRotationDeg = other.apertureBladeRotationDeg;
            fStop = other.fStop;
            focusDistanceMm = other.focusDistanceMm;
            sensorWidthMm = other.sensorWidthMm;
            sensorHeightMm = other.sensorHeightMm;
            scale = other.scale;
            // m_surfaceBuffer intentionally left untouched - see LensSystem.h.
        }
        return *this;
    }

    std::vector<GPULensSurface> LensSystem::BuildGPUSurfaces() const
    {
        std::vector<GPULensSurface> out;
        out.reserve(surfaces.size());
        if (surfaces.empty()) return out;

        double scaledEFL = effectiveFocalLengthMm * scale;
        double focusScale = FocusScaleForLastGap(scaledEFL, focusDistanceMm);
        size_t lastIndex = surfaces.size() - 1;

        // Effective (scaled, focus-adjusted) thickness per surface, and the
        // cumulative axial position (mm) from the sensor (z=0) toward the
        // front element (largest z) - a suffix sum of thicknesses, since
        // `surfaces` is stored front-to-back but distances are measured
        // from the sensor. See LensSystem.h for the coordinate convention.
        std::vector<double> effThickness(surfaces.size(), 0.0);
        std::vector<double> zPos(surfaces.size(), 0.0);
        double z = 0.0;
        for (int i = static_cast<int>(lastIndex); i >= 0; --i)
        {
            double thickness = surfaces[i].thicknessMm * scale;
            if (static_cast<size_t>(i) == lastIndex) thickness *= focusScale;
            effThickness[i] = thickness;
            z += thickness;
            zPos[i] = z;
        }

        double apertureRadiusMm = 0.0;
        for (const LensSurface& s : surfaces)
            if (s.isAperture)
                apertureRadiusMm = std::min(s.semiDiameterMm * scale, (scaledEFL / fStop) * 0.5);

        double bladeRotationRad = apertureBladeRotationDeg * 3.14159265358979 / 180.0;

        for (size_t i = 0; i < surfaces.size(); ++i)
        {
            const LensSurface& s = surfaces[i];
            GPULensSurface g{};

            float isApertureFlag = s.isAperture ? 1.0f : 0.0f;
            // LensSurface::radiusMm follows the standard optical-prescription
            // sign convention (as in the paper's table / any external lens
            // patent: z increases object->image, positive radius = center of
            // curvature toward the image side) so pasted-in prescriptions
            // need no manual sign-flipping. The ray tracer's internal z axis
            // runs the opposite way (sensor at z=0, front element at the
            // largest z), which inverts the meaning of a sphere's radius
            // sign - so it is negated exactly once, here, at the CPU->GPU
            // boundary. Confirmed by an independent forward (object->sensor)
            // paraxial ray trace: without this negation the 8-surface Tessar
            // nets ~0.0003 rad of bend end-to-end (surfaces nearly cancel)
            // and a sensor-side ray bundle converges *behind* the lens
            // (virtual image); with it, parallel rays converge ~29.6m out
            // (near infinity, as expected) and a 6500mm focus target
            // converges at ~6487mm - see docs/ARCHITECTURE.md.
            g.geometry = glm::vec4(static_cast<float>(-s.radiusMm * scale), static_cast<float>(effThickness[i]),
                                    static_cast<float>(s.semiDiameterMm * scale), isApertureFlag);

            const Glass* glass = GlassCatalog::Find(s.material);
            if (!glass)
            {
                ROYALGL_LOG_WARN("LensSystem: unknown material '", s.material, "', treating as air.");
                glass = GlassCatalog::Find("air");
            }
            glm::vec3 iorRGB(glass->IOR(LensWavelengths::kNm[0]), glass->IOR(LensWavelengths::kNm[1]),
                              glass->IOR(LensWavelengths::kNm[2]));
            g.iorRGB_z = glm::vec4(iorRGB, static_cast<float>(zPos[i]));

            g.coatingRGB = glm::vec4(s.coatingR, s.coatingG, s.coatingB, 0.0f);

            g.apertureData = glm::vec4(s.isAperture ? static_cast<float>(apertureBlades) : 0.0f,
                                        static_cast<float>(bladeRotationRad),
                                        static_cast<float>(apertureRadiusMm), 0.0f);

            out.push_back(g);
        }
        return out;
    }

    void LensSystem::Upload()
    {
        std::vector<GPULensSurface> gpu = BuildGPUSurfaces();
        if (gpu.empty()) return;
        m_surfaceBuffer.Upload(gpu.data(), gpu.size() * sizeof(GPULensSurface), GL_STATIC_DRAW);
    }

    void LensSystem::BindAll() const { m_surfaceBuffer.BindBase(); }

    bool LensSystem::operator==(const LensSystem& other) const
    {
        return surfaces == other.surfaces && name == other.name &&
               effectiveFocalLengthMm == other.effectiveFocalLengthMm && apertureBlades == other.apertureBlades &&
               apertureBladeRotationDeg == other.apertureBladeRotationDeg && fStop == other.fStop &&
               focusDistanceMm == other.focusDistanceMm && sensorWidthMm == other.sensorWidthMm &&
               sensorHeightMm == other.sensorHeightMm && scale == other.scale;
    }
}
