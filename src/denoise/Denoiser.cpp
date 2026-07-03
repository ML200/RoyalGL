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
            const char* typeName = "unknown";
            switch (m_impl->device.get<oidn::DeviceType>("type"))
            {
            case oidn::DeviceType::CPU:   typeName = "CPU"; break;
            case oidn::DeviceType::SYCL:  typeName = "SYCL"; break;
            case oidn::DeviceType::CUDA:  typeName = "CUDA"; break;
            case oidn::DeviceType::HIP:   typeName = "HIP"; break;
            case oidn::DeviceType::Metal: typeName = "Metal"; break;
            default: break;
            }
            ROYALGL_LOG_INFO("OIDN device created successfully (", typeName, ")");
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

        // GPU devices (CUDA/HIP/SYCL/Metal) can't read arbitrary host
        // pointers - images must live in OIDN buffers, which the runtime
        // places in device-accessible memory and copies through on
        // read()/write(). The RGBA layout rides through unchanged: the
        // filter reads/writes Float3 at a 16-byte pixel stride, leaving
        // the alpha channel (per-pixel sample counts) untouched in the
        // output buffer, which was pre-seeded with the input image.
        const size_t bytes = color.size() * sizeof(float);
        oidn::BufferRef colorBuf = m_impl->device.newBuffer(bytes);
        oidn::BufferRef outputBuf = m_impl->device.newBuffer(bytes);

        const char* errorMessage = nullptr;
        if (!colorBuf || !outputBuf ||
            m_impl->device.getError(errorMessage) != oidn::Error::None)
        {
            ROYALGL_LOG_WARN("OIDN buffer allocation failed: ",
                             errorMessage ? errorMessage : "unknown error");
            return color;
        }

        colorBuf.write(0, bytes, color.data());
        outputBuf.write(0, bytes, color.data());

        oidn::FilterRef filter = m_impl->device.newFilter("RT");
        filter.setImage("color", colorBuf, oidn::Format::Float3, width, height, 0, 4 * sizeof(float), 0);
        filter.setImage("output", outputBuf, oidn::Format::Float3, width, height, 0, 4 * sizeof(float), 0);
        filter.set("hdr", true);
        filter.commit();
        filter.execute();

        if (m_impl->device.getError(errorMessage) != oidn::Error::None)
        {
            ROYALGL_LOG_WARN("OIDN denoise failed: ", errorMessage);
            return color;
        }

        std::vector<float> output(color.size());
        outputBuf.read(0, bytes, output.data());
        return output;
#else
        return color;
#endif
    }
}
