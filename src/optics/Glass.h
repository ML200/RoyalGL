#pragma once

#include <string>

namespace RoyalGL
{
    // The 3 representative wavelengths (nm) used everywhere dispersion is
    // approximated in lens mode: index 0=R, 1=G, 2=B.
    namespace LensWavelengths
    {
        constexpr float kNm[3] = {611.0f, 550.0f, 465.0f};
    }

    // Two-term Cauchy dispersion (n(lambda) = A + B/lambda^2, lambda in
    // micrometers), derived from a glass's catalog (nd, Vd) pair via the
    // standard Abbe-number-to-Cauchy-coefficient conversion. This mirrors
    // spectrum_cauchy_from_abbe/spectrum_eta_from_abbe_um from Johannes
    // Hanika's own lens-tracing renderer ("corona-6" - Hanika is a co-author
    // of Steinert et al. 2011, the paper this module implements): it only
    // needs the two catalog numbers (nd, Vd) that are commonly published,
    // rather than a 6-coefficient Sellmeier fit that isn't reliably
    // available for these specific (partly discontinued) Schott glass codes.
    struct Glass
    {
        std::string name;
        float nd = 1.0f; // refractive index at the Fraunhofer d-line (587.6nm)
        float vd = 0.0f; // Abbe number; 0 = dispersion-free sentinel (used by "air")

        // Index of refraction at `wavelengthNm`.
        float IOR(float wavelengthNm) const;

        bool IsAir() const { return vd == 0.0f; }
    };

    namespace GlassCatalog
    {
        // Built-in catalog: "air" plus every glass the built-in Tessar
        // preset needs (LAK9, LLF7, SF7). (nd, Vd) values transcribed from
        // the exact lens prescription data file for this Tessar shipped by
        // Johannes Hanika alongside his own lens-tracing renderer source
        // (polynomial-optics/lenses/brendel-tessar.fx in
        // https://jo.dreggn.org/home/2016_optics.tar.bz2), which lists
        // index/vno per surface directly - not fabricated.
        // Returns nullptr if `name` is not a known glass.
        const Glass* Find(const std::string& name);
    }
}
