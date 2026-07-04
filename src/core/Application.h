#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
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
        bool LoadScene(const std::string& path);
        // Recommended camera/fog preset for the bundled scenes (LightMaze,
        // LensFog, built-in) - applied on scene switches and, before the
        // env overrides, at startup.
        void ApplyScenePreset(const std::filesystem::path& path);
        // Full scene swap at a safe frame boundary: reload + BLAS/TLAS +
        // light tree rebuilds + path tracer reset + ReSTIR history clear.
        void PerformSceneSwitch(int index);
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

        // assets/lenses/*.lens, scanned once at startup; parallel arrays of
        // file paths and display names for the UI preset dropdown.
        std::vector<std::filesystem::path> m_lensPresetPaths;
        std::vector<std::string> m_lensPresetNames;
        // Scene picker: entry 0 is the built-in Cornell fallback (empty
        // path), the rest are assets/scenes/*.glb (Duck.glb excluded - it
        // is a prop merged into the fallback, not a standalone scene) plus
        // an optional custom startup path. Parallel arrays for the UI.
        std::vector<std::filesystem::path> m_scenePaths;
        std::vector<std::string> m_sceneNames;
        int m_sceneIndex = 0;
        // Built-in scene composition (UI-editable; initialized from the
        // ROYALGL_DUCKS / ROYALGL_MAT envs): clutter duck count and the
        // main duck's material preset (0 glass, 1 conductor, 2 rough
        // glass, 3 layered coat, 4 layered + medium).
        int m_duckCount = 0;
        int m_duckMaterial = 0;
        // Scene switches requested by the UI are deferred to the next
        // frame boundary where no async BVH rebuild is in flight.
        int m_pendingSceneIndex = -1;
        bool m_pendingSceneReload = false;
        // ROYALGL_SWITCH_TEST="index,frame": scripted runtime scene switch
        // through the UI's deferred path (headless verification hook).
        int m_switchTestIndex = -1;
        int m_switchTestFrame = 0;
        int m_switchTestTick = 0;
        bool m_dirty = true; // forces an accumulation reset next frame

        // ROYALGL_STATS=1: periodically log luminance tail statistics of the
        // raw accumulation buffer - the tool for chasing fireflies.
        bool m_statsEnabled = false;
        // ROYALGL_STATS_MASK=1: additionally log the same statistics
        // restricted to pixels the temporal pass flagged as disoccluded
        // (no usable history) - the region where spatial candidate
        // selection strategies actually differ. Needs temporal reuse on.
        bool m_statsMaskEnabled = false;
        uint32_t m_lastStatsSample = 0;
        uint32_t m_statsFrame = 0;
        // One dirty flag per scene instance: set when the UI edits its
        // transform, cleared when the async BVH rebuild for it is kicked
        // (coalesces edits that arrive while a build is in flight).
        std::vector<bool> m_instanceDirty;
        int m_statsInterval = 256; // ROYALGL_STATS_INTERVAL: samples between stat logs
        // ROYALGL_LOCK_CAMERA: ignore camera input so scripted soak tests
        // stay deterministic even if the window gets focus/mouse events.
        bool m_cameraLocked = false;
        // ROYALGL_ORBIT=<deg/s>: scripted continuous yaw rotation - brings
        // surfaces into view at grazing angles every frame, the repro case
        // for temporal-reuse transients that locked-camera soaks can't see.
        float m_orbitSpeed = 0.0f;
        float m_orbitPhase = 0.0f;
        // ROYALGL_DOLLY=<units/s>: scripted lateral truck (rocking along the
        // camera right axis, flip every 2s) - PARALLAX, which pure rotation
        // never produces: the repro case for reprojection-pairing losses
        // (fog history vs surface anchors under translation).
        float m_dollySpeed = 0.0f;
        float m_dollyPhase = 0.0f;
        // ROYALGL_FIXED_DT=<s>: constant per-frame step for the scripted
        // motions (orbit/dolly/move) - frame-deterministic A/B comparisons
        // between configs with different frame costs.
        float m_fixedDt = 0.0f;
        // ROYALGL_MOVE=<rad/s>: scripted oscillation of the last instance -
        // headless exercise of the async BLAS/TLAS rebuild path.
        float m_moveTestSpeed = 0.0f;
        float m_movePhase = 0.0f;

        double m_lastMouseX = 0.0;
        double m_lastMouseY = 0.0;
        bool m_flying = false; // true while the right mouse button is driving mouse-look
    };
}
