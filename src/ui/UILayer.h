#pragma once

#include <string>
#include <cstdint>
#include "pathtracer/RenderSettings.h"
#include "scene/Scene.h"
#include "scene/CameraSettings.h"
#include "optics/LensSystem.h"

struct GLFWwindow;

namespace RoyalGL
{
    struct UIFrameResult
    {
        bool settingsChanged = false;
        bool lensChanged = false; // true if cameraSettings or lensSystem's tunables changed this frame
        bool materialsChanged = false;
        bool lensPresetLoadRequested = false;
        std::string lensPresetToLoad;
        bool denoiseRequested = false;
        bool exportRequested = false;
        std::string exportPath;
    };

    // Immediate-mode ImGui overlay: stats, camera info, camera model / lens
    // editor, render settings, denoise toggle and PNG export. Owns no
    // rendering resources itself - it only reads/mutates the settings
    // structs Application passed in and reports back what the user asked for.
    class UILayer
    {
    public:
        explicit UILayer(GLFWwindow* window);
        ~UILayer();

        UILayer(const UILayer&) = delete;
        UILayer& operator=(const UILayer&) = delete;

        void BeginFrame() const;
        void EndFrame() const;

        UIFrameResult Draw(RenderSettings& settings, CameraSettings& cameraSettings, LensSystem& lensSystem,
                            Scene& scene, uint32_t sampleCount, float frameTimeMs, bool oidnAvailable);

    private:
        GLFWwindow* m_window = nullptr;
    };
}
