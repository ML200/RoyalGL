#include "core/Application.h"
#include "core/Log.h"

#include <exception>

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
