#pragma once

#include <vector>
#include <string>
#include "optics/LensSurface.h"
#include "gfx/Buffer.h"
#include "gfx/GPUTypes.h"

namespace RoyalGL
{
    // Owns one lens prescription plus the shooting parameters layered on top
    // (aperture blade count/f-stop, focus, sensor size, overall scale). This
    // is the optics analog of BVHBuilder: builds CPU-derived GPU data once
    // per change and uploads it to a fixed SSBO binding point.
    //
    // `surfaces` is ordered object-side (front element, first surface light
    // hits) -> image-side (last surface, closest to the sensor); this
    // matches the paper's own table order and requires no reversal on
    // upload. The primary (eye) ray tracer walks this array back-to-front
    // (sensor -> front element); the flare/ghost light tracer walks it
    // front-to-back (front element -> sensor) - see shaders/lens_common.glsl
    // and shaders/lens_flare.comp.
    class LensSystem
    {
    public:
        LensSystem() = default;

        // GL buffers are not copyable/shareable; copies of LensSystem are
        // only ever used as value snapshots for change-detection
        // (Application::m_lastLensSystem, UILayer's before/after diff), so
        // these copy only the data fields and leave the copy's GL buffer
        // freshly (and independently) default-constructed - see LensSystem.cpp.
        LensSystem(const LensSystem& other);
        LensSystem& operator=(const LensSystem& other);

        std::vector<LensSurface> surfaces;
        std::string name = "Untitled";
        double effectiveFocalLengthMm = 50.0;

        int apertureBlades = 6; // N-sided polygon; 0/1/2 = circular aperture
        double apertureBladeRotationDeg = 0.0;
        double fStop = 5.6; // photographic f-number N = f / entrance-pupil-diameter

        // v1 focus model: uniformly rescale every surface's thicknessMm gap
        // by a factor derived from the desired object distance (a simple,
        // widely-used "move the whole rear group" approximation). Exposed
        // to the UI as a physical object distance rather than the internal
        // scale factor.
        double focusDistanceMm = 1.0e6; // ~infinity by default

        double sensorWidthMm = 360.0;  // 10x a real 36mm sensor - deliberately oversized default (see docs/ARCHITECTURE.md)
        double sensorHeightMm = 240.0; // kept at the same 3:2 ratio as the 36x24mm default; unused by the renderer itself (aspect comes from the viewport - see ARCHITECTURE.md), only shown in the UI

        double scale = 1.5; // "zoom" - scales the whole lens (and therefore its EFL) uniformly; >1 = more telephoto

        // Recomputes cached per-surface RGB IOR (via Glass::IOR at the 3
        // representative wavelengths), cumulative axial positions, and the
        // aperture's physical radius (from fStop + EFL), then uploads a
        // GPULensSurface[] to SSBO binding 5. Call whenever any field above
        // changes.
        void Upload();
        void BindAll() const;

        int SurfaceCount() const { return static_cast<int>(surfaces.size()); }

        bool operator==(const LensSystem& other) const;
        bool operator!=(const LensSystem& other) const { return !(*this == other); }

    private:
        std::vector<GPULensSurface> BuildGPUSurfaces() const;

        Buffer m_surfaceBuffer{BufferType::ShaderStorage, 5};
    };
}
