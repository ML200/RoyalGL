#include "core/Application.h"
#include "core/Log.h"

#include <exception>

#ifdef _WIN32
// Hybrid-graphics systems (laptop iGPU + discrete GPU) give OpenGL contexts
// to the integrated GPU by default. Exporting these symbols asks the NVIDIA
// and AMD drivers to route this process to the discrete GPU instead.
extern "C"
{
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

int main(int argc, char** argv)
{
    RoyalGL::ApplicationDesc desc;
    if (argc > 1) desc.startupScenePath = argv[1];

    try
    {
        RoyalGL::Application app(desc);
        app.Run();
    }
    catch (const std::exception& e)
    {
        ROYALGL_LOG_ERROR("Fatal: ", e.what());
        return 1;
    }
    return 0;
}
