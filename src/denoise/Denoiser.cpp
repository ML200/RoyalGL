#include "denoise/Denoiser.h"

#include "core/Log.h"

#ifdef ROYALGL_HAS_OIDN
#include <OpenImageDenoise/oidn.hpp>
#endif

namespace RoyalGL
{
    struct Denoiser::Impl
    {
#ifdef ROYALGL_HAS_OIDN
        oidn::DeviceRef device;
        bool valid = false;
#endif
    };

    Denoiser::Denoiser()
    {
        m_impl = new Impl();

#ifdef ROYALGL_HAS_OIDN
        m_impl->device = oidn::newDevice();
        m_impl->device.commit();

        const char* errorMessage = nullptr;
        if (m_impl->device.getError(errorMessage) != oidn::Error::None)
        {
            ROYALGL_LOG_ERROR("Failed to create OIDN device: ", errorMessage);
            m_impl->valid = false;
        }
        else
        {
            m_impl->valid = true;
            ROYALGL_LOG_INFO("OIDN device created successfully");
        }
#endif
    }

    Denoiser::~Denoiser()
    {
        delete m_impl;
    }

    bool Denoiser::IsAvailable()
    {
#ifdef ROYALGL_HAS_OIDN
        return true;
#else
        return false;
#endif
    }

    std::vector<float> Denoiser::Denoise(const std::vector<float>& color, int width, int height)
    {
#ifdef ROYALGL_HAS_OIDN
        if (!m_impl->valid)
        {
            return color;
        }

        oidn::FilterRef filter = m_impl->device.newFilter("RT");

        std::vector<float> output = color;

        filter.setImage("color", const_cast<float*>(color.data()), oidn::Format::Float3, width, height, 0, 4 * sizeof(float), 0);
        filter.setImage("output", output.data(), oidn::Format::Float3, width, height, 0, 4 * sizeof(float), 0);
        filter.set("hdr", true);
        filter.commit();
        filter.execute();

        const char* errorMessage = nullptr;
        if (m_impl->device.getError(errorMessage) != oidn::Error::None)
        {
            ROYALGL_LOG_WARN("OIDN denoise failed: ", errorMessage);
            return color;
        }

        return output;
#else
        return color;
#endif
    }
}
