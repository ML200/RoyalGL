#pragma once

#include <string>

namespace RoyalGL
{
    // One physical surface in a lens system, in the paper's table format
    // (Steinert et al. 2011, Sec 3.4 / Fig 4): a spherical, axially-
    // symmetric interface, or the aperture stop. Units are millimeters
    // (Sec 4.1: "millimeters are used for the lens tracing").
    struct LensSurface
    {
        // Signed radius of curvature, mm. Positive => center of curvature is
        // to the right (image/sensor side) of this surface; negative =>
        // left (object side). 0.0 for the aperture stop row (flat, no
        // curvature).
        double radiusMm = 0.0;

        // Axial distance, mm, from this surface to the NEXT surface in the
        // list (this row's thickness governs the gap that follows it).
        double thicknessMm = 0.0;

        // Semi-diameter (mm) - the physical radius of this element's clear
        // aperture / blocking edge.
        double semiDiameterMm = 0.0;

        // Material AFTER this surface (what the ray travels through over
        // `thicknessMm`), by catalog name. "air" for gaps and for the
        // aperture stop row.
        std::string material = "air";

        // True for exactly one row: the aperture stop.
        bool isAperture = false;

        // Optional single-layer AR coating: per-channel (R,G,B) multiplier
        // on this surface's Fresnel reflectance, consumed by the flare/ghost
        // pass. {1,1,1} = no coating (Fresnel-only).
        float coatingR = 1.0f;
        float coatingG = 1.0f;
        float coatingB = 1.0f;

        bool operator==(const LensSurface& other) const
        {
            return radiusMm == other.radiusMm && thicknessMm == other.thicknessMm &&
                   semiDiameterMm == other.semiDiameterMm && material == other.material &&
                   isAperture == other.isAperture && coatingR == other.coatingR &&
                   coatingG == other.coatingG && coatingB == other.coatingB;
        }
        bool operator!=(const LensSurface& other) const { return !(*this == other); }
    };
}
