#include "core/Application.h"
#include "core/Log.h"
#include "scene/GLTFLoader.h"
#include "optics/LensPrescription.h"
#include "io/ImageExport.h"
#include "gfx/Framebuffer.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <chrono>

namespace RoyalGL
{
    namespace
    {
        // The accumulation image stores a running sum, not an average (see
        // PathTracer::Render / shaders/pathtrace.comp) - anything that reads
        // it back for denoising or export needs to divide by the sample
        // count first to get linear HDR color.
        void AverageInPlace(std::vector<float>& rgba, uint32_t sampleCount)
        {
            if (sampleCount == 0) return;
            float inv = 1.0f / static_cast<float>(sampleCount);
            for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            {
                rgba[i + 0] *= inv;
                rgba[i + 1] *= inv;
                rgba[i + 2] *= inv;
                rgba[i + 3] = 1.0f;
            }
        }
    }

    Application::Application(const ApplicationDesc& desc)
    {
        m_windowDesc.title = "RoyalGL";
        m_window = std::make_unique<Window>(m_windowDesc);

        m_scene = std::make_unique<Scene>();
        LoadScene(desc.startupScenePath);

        m_bvh = std::make_unique<BVHBuilder>();
        m_bvh->Build(*m_scene);
        m_lastMaterials = m_scene->materials;

        m_lightList = std::make_unique<LightList>();
        m_lightList->Build(*m_scene);

        m_lensSystem = std::make_unique<LensSystem>();
        *m_lensSystem = LensPrescription::BuiltinTessar(); // always start with a working built-in
        m_lensSystem->Upload();

        m_pathTracer = std::make_unique<PathTracer>();
        glm::ivec2 fbSize = m_window->GetFramebufferSize();
        m_pathTracer->Resize(fbSize.x, fbSize.y);

        m_lensFlare = std::make_unique<LensFlare>();
        m_fullscreenPass = std::make_unique<FullscreenPass>();
        m_denoiser = std::make_unique<Denoiser>();
        m_ui = std::make_unique<UILayer>(m_window->Handle());

        m_lastCamera = m_scene->camera;
        m_lastSettings = m_settings;
        m_lastCameraSettings = m_cameraSettings;
        m_lastLensSystem = std::make_unique<LensSystem>(*m_lensSystem);
        m_dirty = true;

        m_window->OnResize = [this](int w, int h) { OnFramebufferResize(w, h); };
        m_window->OnScroll = [this](double /*xoffset*/, double yoffset)
        {
            if (ImGui::GetIO().WantCaptureMouse) return;
            m_scene->camera.Dolly(static_cast<float>(-yoffset));
        };
    }

    Application::~Application() = default;

    void Application::LoadScene(const std::string& path)
    {
        bool loaded = false;
        if (!path.empty())
        {
            Scene loadedScene;
            if (GLTFLoader::Load(path, loadedScene))
            {
                *m_scene = std::move(loadedScene);
                loaded = true;
            }
            else
            {
                ROYALGL_LOG_WARN("Application: failed to load '", path, "', falling back to the built-in scene.");
            }
        }
        if (!loaded)
            m_scene->LoadFallbackScene();
    }

    void Application::OnFramebufferResize(int width, int height)
    {
        if (width <= 0 || height <= 0) return;
        m_pathTracer->Resize(width, height);
    }

    void Application::HandleCameraInput(float /*dt*/)
    {
        GLFWwindow* handle = m_window->Handle();
        ImGuiIO& io = ImGui::GetIO();

        double mouseX = 0.0, mouseY = 0.0;
        glfwGetCursorPos(handle, &mouseX, &mouseY);
        double dx = mouseX - m_lastMouseX;
        double dy = mouseY - m_lastMouseY;
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;

        if (io.WantCaptureMouse)
        {
            m_orbiting = false;
            m_panning = false;
            return;
        }

        bool leftDown = glfwGetMouseButton(handle, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        bool rightDown = glfwGetMouseButton(handle, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        bool middleDown = glfwGetMouseButton(handle, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

        if (leftDown)
        {
            m_scene->camera.Orbit(static_cast<float>(-dx) * 0.25f, static_cast<float>(-dy) * 0.25f);
            m_orbiting = true;
        }
        else
        {
            m_orbiting = false;
        }

        if (rightDown || middleDown)
        {
            float panScale = 0.0025f * glm::length(m_scene->camera.position - m_scene->camera.target);
            m_scene->camera.Pan(static_cast<float>(-dx) * panScale, static_cast<float>(dy) * panScale);
            m_panning = true;
        }
        else
        {
            m_panning = false;
        }
    }

    void Application::RunDenoiser()
    {
        uint32_t sampleCount = m_pathTracer->SampleCount();
        if (sampleCount == 0)
        {
            ROYALGL_LOG_WARN("Application: nothing accumulated yet, skipping denoise.");
            return;
        }

        std::vector<float> raw = m_pathTracer->AccumulationImage().ReadPixelsFloat();
        AverageInPlace(raw, sampleCount);

        std::vector<float> denoised = m_denoiser->Denoise(raw, m_pathTracer->Width(), m_pathTracer->Height());
        if (ImageExport::SavePNG("denoised.png", denoised, m_pathTracer->Width(), m_pathTracer->Height(), m_settings.exposure))
            ROYALGL_LOG_INFO("Application: wrote denoised.png");
        else
            ROYALGL_LOG_ERROR("Application: failed to write denoised.png");
    }

    void Application::ExportPNG(const std::string& path)
    {
        uint32_t sampleCount = m_pathTracer->SampleCount();
        if (sampleCount == 0)
        {
            ROYALGL_LOG_WARN("Application: nothing accumulated yet, skipping export.");
            return;
        }

        std::vector<float> raw = m_pathTracer->AccumulationImage().ReadPixelsFloat();
        AverageInPlace(raw, sampleCount);

        if (ImageExport::SavePNG(path, raw, m_pathTracer->Width(), m_pathTracer->Height(), m_settings.exposure))
            ROYALGL_LOG_INFO("Application: exported ", path);
        else
            ROYALGL_LOG_ERROR("Application: failed to export ", path);
    }

    void Application::Run()
    {
        using clock = std::chrono::steady_clock;
        auto lastFrameTime = clock::now();

        while (!m_window->ShouldClose())
        {
            auto now = clock::now();
            float dt = std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;

            m_window->PollEvents();
            HandleCameraInput(dt);

            bool lensDirty = (m_cameraSettings != m_lastCameraSettings) || (*m_lensSystem != *m_lastLensSystem);
            bool materialsDirty = (m_scene->materials != m_lastMaterials);
            if (m_scene->camera != m_lastCamera || m_settings != m_lastSettings || lensDirty || materialsDirty)
            {
                if (lensDirty) m_lensSystem->Upload(); // re-derive+reupload GPU data only when something changed
                if (materialsDirty)
                {
                    m_bvh->UpdateMaterials(*m_scene);
                    // Editing a material's emissive value can add/remove/reweight
                    // flare-pass light sources, so the light list + sampling CDF
                    // (built once from the ORIGINAL emissive values at startup)
                    // must be rebuilt too, not just the main materials SSBO.
                    m_lightList->Build(*m_scene);
                }
                m_pathTracer->Reset();
                m_lastCamera = m_scene->camera;
                m_lastSettings = m_settings;
                m_lastCameraSettings = m_cameraSettings;
                *m_lastLensSystem = *m_lensSystem;
                m_lastMaterials = m_scene->materials;
                m_dirty = false;
            }

            m_pathTracer->Render(m_scene->camera, *m_bvh, m_settings, m_cameraSettings,
                                  m_cameraSettings.mode == CameraMode::LensSystem ? m_lensSystem.get() : nullptr);

            bool flareActive = m_settings.enableFlare && m_cameraSettings.mode == CameraMode::LensSystem &&
                                m_lightList->LightCount() > 0;
            if (flareActive)
            {
                m_lensFlare->ResetSplatBuffer();
                m_lensFlare->TraceLightPaths(*m_lightList, m_settings);
                // Compute wrote the splat SSBOs (binding 8/9) - make them
                // visible to the splat pass's vertex shader SSBO reads, and
                // make PathTracer's prior compute image-store into m_accum
                // visible to the upcoming FBO-attached raster write of the
                // same texture (see docs/ARCHITECTURE.md for the full
                // barrier reasoning).
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT);
                m_lensFlare->SplatToAccumulation(m_pathTracer->AccumulationImage(),
                                                  glm::ivec2(m_pathTracer->Width(), m_pathTracer->Height()));
                // Make this raster write visible to next frame's compute
                // imageLoad/imageStore on the same texture.
                glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
            }

            Framebuffer::BindDefault();
            glm::ivec2 fbSize = m_window->GetFramebufferSize();
            glViewport(0, 0, fbSize.x, fbSize.y);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            m_fullscreenPass->Draw(m_pathTracer->AccumulationImage(), m_settings.exposure, m_pathTracer->SampleCount());

            m_ui->BeginFrame();
            UIFrameResult result = m_ui->Draw(m_settings, m_cameraSettings, *m_lensSystem, *m_scene,
                                               m_pathTracer->SampleCount(), dt * 1000.0f, Denoiser::IsAvailable());
            if (result.lensPresetLoadRequested)
            {
                *m_lensSystem = LensPrescription::LoadBuiltinPreset(result.lensPresetToLoad);
            }
            if (result.denoiseRequested) RunDenoiser();
            if (result.exportRequested) ExportPNG(result.exportPath);
            m_ui->EndFrame();

            m_window->SwapBuffers();
        }
    }
}
