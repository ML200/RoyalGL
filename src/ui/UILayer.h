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
        // Index of a scene instance whose transform the user edited this
        // frame (-1 = none). Application coalesces these into async BVH
        // rebuilds.
        int instanceMoved = -1;
        // Scene combo pick this frame (-1 = no change) and whether the
        // built-in scene's composition controls (clutter ducks / duck
        // material) changed - both need a full scene reload, which
        // Application defers to a safe frame boundary.
        int sceneSelected = -1;
        bool sceneCompositionChanged = false;
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
                            bool oidnAvailable, const std::vector<std::string>& lensPresetNames,
                            const std::vector<std::string>& sceneNames, int sceneIndex,
                            int& duckCount, int& duckMaterial);

    private:
        GLFWwindow* m_window = nullptr;
    };
}
