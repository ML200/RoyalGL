#pragma once

#include <cmath>
#include <string>

namespace RoyalGL
{
    // Optical glass dispersion, per Steinert et al. 2011 sec. 3.2: Sellmeier
    // definitions of the Schott catalog where available. eta^2(lambda) =
    // 1 + sum_i B_i * l2 / (l2 - C_i), lambda in micrometers.
    //
    // Discontinued catalog glasses without published Sellmeier coefficients
    // fall back to a two-term Cauchy model derived from their (nd, vd)
    // catalog entry - exact at the d line with the correct Abbe number, a
    // few 1e-4 off at the spectrum edges.
    struct Glass
    {
        bool sellmeier = false;
        float B[3] = {0, 0, 0};
        float C[3] = {0, 0, 0};
        float cauchyA = 1.0f;
        float cauchyB = 0.0f; // um^2

        float Eta(float lambdaNm) const
        {
            float um = lambdaNm * 1e-3f;
            float l2 = um * um;
            if (sellmeier)
            {
                float n2 = 1.0f;
                for (int i = 0; i < 3; ++i) n2 += B[i] * l2 / (l2 - C[i]);
                return std::sqrt(std::max(n2, 1.0f));
            }
            return cauchyA + cauchyB / l2;
        }

        static Glass Air() { return Glass{}; } // cauchyA=1 -> eta 1

        static Glass FromNdVd(float nd, float vd)
        {
            // Two-term Cauchy n = A + B/l^2 fitted to n_d (587.6nm) and the
            // Abbe number vd = (nd-1)/(nF-nC), F=486.1nm, C=656.3nm.
            constexpr float lD2 = 0.5876f * 0.5876f;
            constexpr float lF2 = 0.4861f * 0.4861f;
            constexpr float lC2 = 0.6563f * 0.6563f;
            Glass g;
            g.cauchyB = (nd - 1.0f) / (vd * (1.0f / lF2 - 1.0f / lC2));
            g.cauchyA = nd - g.cauchyB / lD2;
            return g;
        }

        // Named lookup for the prescriptions we ship. N-LAK9 carries genuine
        // Schott Sellmeier coefficients; LLF7 and SF7 are discontinued and
        // use the (nd, vd) fallback.
        static Glass Lookup(const std::string& name)
        {
            if (name == "air" || name.empty()) return Air();
            if (name == "LAK9" || name == "N-LAK9")
            {
                Glass g;
                g.sellmeier = true;
                g.B[0] = 1.46231905f; g.B[1] = 0.344399589f; g.B[2] = 1.15508372f;
                g.C[0] = 0.00724270156f; g.C[1] = 0.0243353131f; g.C[2] = 85.4686868f;
                return g;
            }
            if (name == "LLF7") return FromNdVd(1.5486f, 45.4f);
            if (name == "SF7") return FromNdVd(1.6398f, 34.6f);
            if (name == "BK7" || name == "N-BK7")
            {
                Glass g;
                g.sellmeier = true;
                g.B[0] = 1.03961212f; g.B[1] = 0.231792344f; g.B[2] = 1.01046945f;
                g.C[0] = 0.00600069867f; g.C[1] = 0.0200179144f; g.C[2] = 103.560653f;
                return g;
            }
            return FromNdVd(1.5f, 50.0f); // unknown glass: generic crown
        }
    };
}
