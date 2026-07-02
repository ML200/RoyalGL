#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "pathtracer/RenderSettings.h"
#include "scene/Scene.h"

struct GLFWwindow;

namespace RoyalGL
{
    struct UIFrameResult
    {
        bool settingsChanged = false;
        bool materialsChanged = false;
        bool denoiseRequested = false;
        bool exportRequested = false;
        std::string exportPath;
    };

    // Immediate-mode ImGui overlay: stats, camera info, render settings,
    // denoise toggle and PNG export. Owns no rendering resources itself -
    // it only reads/mutates the settings structs Application passed in and
    // reports back what the user asked for.
    class UILayer
    {
    public:
        explicit UILayer(GLFWwindow* window);
        ~UILayer();

        UILayer(const UILayer&) = delete;
        UILayer& operator=(const UILayer&) = delete;

        void BeginFrame() const;
        void EndFrame() const;

        UIFrameResult Draw(RenderSettings& settings, Scene& scene, uint32_t sampleCount, float frameTimeMs,
                            bool oidnAvailable, const std::vector<std::string>& lensPresetNames);

    private:
        GLFWwindow* m_window = nullptr;
    };
}
