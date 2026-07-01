#include "optics/Glass.h"

#include <array>
#include <cmath>

namespace RoyalGL
{
    float Glass::IOR(float wavelengthNm) const
    {
        if (IsAir()) return nd;

        // Fraunhofer C/F/D lines, in micrometers (0.6563um = 656.3nm etc).
        constexpr float lC = 0.6563f, lF = 0.4861f, lD = 0.587561f;
        const float c = (lC * lC * lF * lF) / (lC * lC - lF * lF);
        const float B = (nd - 1.0f) / vd * c;
        const float A = nd - B / (lD * lD);

        const float lambdaUm = wavelengthNm * 0.001f;
        return A + B / (lambdaUm * lambdaUm);
    }

    namespace GlassCatalog
    {
        namespace
        {
            const std::array<Glass, 4> kCatalog = {{
                {"air", 1.0f, 0.0f},
                {"LAK9", 1.6910f, 54.8f},
                {"LLF7", 1.5486f, 45.4f},
                {"SF7", 1.6398f, 34.6f},
            }};
        }

        const Glass* Find(const std::string& name)
        {
            for (const Glass& g : kCatalog)
                if (g.name == name)
                    return &g;
            return nullptr;
        }
    }
}
