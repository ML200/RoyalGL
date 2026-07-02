#pragma once

#include <memory>
#include <string>
#include "core/Window.h"
#include "scene/Scene.h"
#include "bvh/BVHBuilder.h"
#include "optics/LensSystem.h"
#include "pathtracer/PathTracer.h"
#include "pathtracer/LightTree.h"
#include "pathtracer/RenderSettings.h"
#include "denoise/Denoiser.h"
#include "gfx/FullscreenPass.h"
#include "ui/UILayer.h"

namespace RoyalGL
{
    struct ApplicationDesc
    {
        std::string startupScenePath; // .glb/.gltf; empty = built-in fallback scene
    };

    // Owns every subsystem and runs the main loop: input -> path trace one
    // more sample -> tonemap to screen -> ImGui -> swap buffers.
    class Application
    {
    public:
        explicit Application(const ApplicationDesc& desc);
        ~Application();

        Application(const Application&) = delete;
        Application& operator=(const Application&) = delete;

        void Run();

    private:
        void LoadScene(const std::string& path);
        void HandleCameraInput(float dt);
        void OnFramebufferResize(int width, int height);
        void ExportPNG(const std::string& path);
        void RunDenoiser();
        void LogAccumulationStats();

        WindowDesc m_windowDesc;
        std::unique_ptr<Window> m_window;
        std::unique_ptr<Scene> m_scene;
        std::unique_ptr<BVHBuilder> m_bvh;
        std::unique_ptr<LightTree> m_lightTree;
        std::unique_ptr<LensSystem> m_lensSystem;
        std::unique_ptr<PathTracer> m_pathTracer;
        std::unique_ptr<FullscreenPass> m_fullscreenPass;
        std::unique_ptr<Denoiser> m_denoiser;
        std::unique_ptr<UILayer> m_ui;

        RenderSettings m_settings;
        Camera m_lastCamera;
        RenderSettings m_lastSettings;
        // Value snapshot for dirty-checking, mirrors the other m_last*
        // members, but for scene.materials specifically since Scene itself
        // isn't otherwise snapshotted/compared.
        std::vector<Material> m_lastMaterials;
        bool m_dirty = true; // forces an accumulation reset next frame

        // ROYALGL_STATS=1: periodically log luminance tail statistics of the
        // raw accumulation buffer - the tool for chasing fireflies.
        bool m_statsEnabled = false;
        uint32_t m_lastStatsSample = 0;

        double m_lastMouseX = 0.0;
        double m_lastMouseY = 0.0;
        bool m_flying = false; // true while the right mouse button is driving mouse-look
    };
}
