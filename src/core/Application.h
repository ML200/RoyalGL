#pragma once

#include <memory>
#include <string>
#include "core/Window.h"
#include "scene/Scene.h"
#include "scene/CameraSettings.h"
#include "bvh/BVHBuilder.h"
#include "optics/LensSystem.h"
#include "pathtracer/PathTracer.h"
#include "pathtracer/RenderSettings.h"
#include "pathtracer/LightList.h"
#include "pathtracer/LensFlare.h"
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

        WindowDesc m_windowDesc;
        std::unique_ptr<Window> m_window;
        std::unique_ptr<Scene> m_scene;
        std::unique_ptr<BVHBuilder> m_bvh;
        std::unique_ptr<LensSystem> m_lensSystem;
        std::unique_ptr<LightList> m_lightList;
        std::unique_ptr<PathTracer> m_pathTracer;
        std::unique_ptr<LensFlare> m_lensFlare;
        std::unique_ptr<FullscreenPass> m_fullscreenPass;
        std::unique_ptr<Denoiser> m_denoiser;
        std::unique_ptr<UILayer> m_ui;

        RenderSettings m_settings;
        Camera m_lastCamera;
        RenderSettings m_lastSettings;
        CameraSettings m_cameraSettings;
        CameraSettings m_lastCameraSettings;
        // Value snapshot for dirty-checking, mirrors m_lastCamera/m_lastSettings.
        // Must be a pointer, not a direct LensSystem member: LensSystem owns a
        // Buffer (creates a GL object on construction), and direct data
        // members of Application are default-constructed during Application's
        // member-init phase - BEFORE the constructor body creates the Window
        // (and therefore the GL context). Constructed in the constructor body
        // instead, after m_lensSystem exists.
        std::unique_ptr<LensSystem> m_lastLensSystem;
        // Value snapshot for dirty-checking, mirrors the other m_last*
        // members, but for scene.materials specifically since Scene itself
        // isn't otherwise snapshotted/compared.
        std::vector<Material> m_lastMaterials;
        bool m_dirty = true; // forces an accumulation reset next frame

        double m_lastMouseX = 0.0;
        double m_lastMouseY = 0.0;
        bool m_orbiting = false;
        bool m_panning = false;
    };
}
