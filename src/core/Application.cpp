#include "core/Application.h"
#include "core/Log.h"
#include "scene/GLTFLoader.h"
#include "io/ImageExport.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>

#ifndef ROYALGL_ASSET_DIR
#define ROYALGL_ASSET_DIR "assets/"
#endif

namespace RoyalGL
{
    namespace
    {
        // The accumulation image stores a running sum, not an average; the
        // per-pixel sample count lives in alpha (rows finish at different
        // times under tiled dispatch, so a global count would be wrong).
        void AverageInPlace(std::vector<float>& rgba)
        {
            for (size_t i = 0; i + 3 < rgba.size(); i += 4)
            {
                float inv = 1.0f / std::max(rgba[i + 3], 1.0f);
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

        m_lightTree = std::make_unique<LightTree>();
        m_lightTree->Build(*m_scene);

        m_lensSystem = std::make_unique<LensSystem>();
        if (!m_lensSystem->LoadLensFile(std::filesystem::path(ROYALGL_ASSET_DIR) / "lenses/tessar.lens"))
            m_lensSystem->LoadBuiltinTessar();
        m_lensSystem->Derive(m_settings.lens);

        m_pathTracer = std::make_unique<PathTracer>();
        glm::ivec2 fbSize = m_window->GetFramebufferSize();
        m_pathTracer->Resize(fbSize.x, fbSize.y);

        m_fullscreenPass = std::make_unique<FullscreenPass>();
        m_denoiser = std::make_unique<Denoiser>();
        m_ui = std::make_unique<UILayer>(m_window->Handle());

        // Env-var overrides for scripted A/B experiments (no rebuild):
        // ROYALGL_BIDIR=0/1, ROYALGL_NEE=0/1, ROYALGL_STATS=1.
        if (const char* v = std::getenv("ROYALGL_BIDIR")) m_settings.enableBidir = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_NEE")) m_settings.enableNEE = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_LENS")) m_settings.cameraMode = (v[0] != '0') ? CameraMode::Lens : CameraMode::Pinhole;
        m_statsEnabled = (std::getenv("ROYALGL_STATS") != nullptr);

        m_lastCamera = m_scene->camera;
        m_lastSettings = m_settings;
        m_dirty = true;

        m_window->OnResize = [this](int w, int h) { OnFramebufferResize(w, h); };
        m_window->OnScroll = [this](double /*xoffset*/, double yoffset)
        {
            if (ImGui::GetIO().WantCaptureMouse) return;
            m_scene->camera.Dolly(static_cast<float>(yoffset));
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
        {
            m_scene->LoadFallbackScene();

            // A small glass duck in the middle of the Cornell box - a delta
            // dielectric that only bidirectional strategies can render
            // efficiently (caustics via light tracing, the duck itself via
            // eye paths).
            Scene duck;
            if (GLTFLoader::Load(std::filesystem::path(ROYALGL_ASSET_DIR) / "scenes/Duck.glb", duck))
            {
                Material glass;
                glass.baseColor = glm::vec3(0.98f, 0.98f, 0.98f);
                glass.type = MaterialType::Glass;
                glass.ior = 1.5f;
                m_scene->MergeInstance(duck, glm::vec3(0.4f, 0.0f, 0.2f), 1.0f, glass);
            }
            else
            {
                ROYALGL_LOG_WARN("Application: Duck.glb not found, fallback scene has no glass object.");
            }
        }
    }

    void Application::OnFramebufferResize(int width, int height)
    {
        if (width <= 0 || height <= 0) return;
        m_pathTracer->Resize(width, height);
    }

    void Application::HandleCameraInput(float dt)
    {
        GLFWwindow* handle = m_window->Handle();
        ImGuiIO& io = ImGui::GetIO();

        double mouseX = 0.0, mouseY = 0.0;
        glfwGetCursorPos(handle, &mouseX, &mouseY);
        double dx = mouseX - m_lastMouseX;
        double dy = mouseY - m_lastMouseY;
        m_lastMouseX = mouseX;
        m_lastMouseY = mouseY;

        // Hold right mouse button to look around; the cursor is captured
        // (hidden, unbounded deltas) for the duration so panning past the
        // window edge keeps working.
        bool rightDown = !io.WantCaptureMouse && glfwGetMouseButton(handle, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        if (rightDown && !m_flying)
        {
            glfwSetInputMode(handle, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            dx = dy = 0.0; // suppress the jump on the press frame
        }
        else if (!rightDown && m_flying)
        {
            glfwSetInputMode(handle, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }
        m_flying = rightDown;

        if (rightDown)
        {
            // Vertical axis inverted: moving the mouse up looks down.
            m_scene->camera.Look(static_cast<float>(-dx) * 0.25f, static_cast<float>(-dy) * 0.25f);
        }

        if (io.WantCaptureKeyboard) return;

        glm::vec3 move{0.0f};
        if (glfwGetKey(handle, GLFW_KEY_W) == GLFW_PRESS) move.z += 1.0f;
        if (glfwGetKey(handle, GLFW_KEY_S) == GLFW_PRESS) move.z -= 1.0f;
        if (glfwGetKey(handle, GLFW_KEY_D) == GLFW_PRESS) move.x += 1.0f;
        if (glfwGetKey(handle, GLFW_KEY_A) == GLFW_PRESS) move.x -= 1.0f;
        if (glfwGetKey(handle, GLFW_KEY_SPACE) == GLFW_PRESS) move.y += 1.0f;
        if (glfwGetKey(handle, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) move.y -= 1.0f;

        if (move != glm::vec3{0.0f})
        {
            constexpr float kFlySpeed = 3.0f; // units/second
            m_scene->camera.Move(glm::normalize(move) * kFlySpeed * dt);
        }
    }

    void Application::LogAccumulationStats()
    {
        uint32_t n = m_pathTracer->SampleCount();
        std::vector<float> raw = m_pathTracer->AccumulationImage().ReadPixelsFloat();
        std::vector<float> lum;
        lum.reserve(raw.size() / 4);
        for (size_t i = 0; i + 3 < raw.size(); i += 4)
            lum.push_back((raw[i] + raw[i + 1] + raw[i + 2]) / (3.0f * std::max(raw[i + 3], 1.0f)));

        // High-frequency noise: mean |pixel - 4-neighbor average| over
        // non-emitter pixels, relative to the image mean. Pure variance
        // measure - a converged image scores ~0 regardless of content.
        int w = m_pathTracer->Width();
        int h = m_pathTracer->Height();
        double noiseSum = 0.0;
        size_t noiseCount = 0;
        for (int y = 1; y < h - 1; ++y)
        {
            for (int x = 1; x < w - 1; ++x)
            {
                float c = lum[static_cast<size_t>(y) * w + x];
                if (c > 5.0f) continue; // skip directly-visible emitters
                float nb = 0.25f * (lum[static_cast<size_t>(y) * w + x - 1] + lum[static_cast<size_t>(y) * w + x + 1] +
                                     lum[(static_cast<size_t>(y) - 1) * w + x] + lum[(static_cast<size_t>(y) + 1) * w + x]);
                noiseSum += std::abs(c - nb);
                noiseCount++;
            }
        }

        std::sort(lum.begin(), lum.end());
        auto pct = [&](double p) { return lum[static_cast<size_t>(p * (lum.size() - 1))]; };
        double mean = 0.0;
        for (float v : lum) mean += v;
        mean /= static_cast<double>(lum.size());
        double relNoise = (noiseCount && mean > 0.0) ? (noiseSum / noiseCount) / mean : 0.0;

        ROYALGL_LOG_INFO("Stats @", n, " samples: mean=", mean, " relNoise=", relNoise,
                         " p50=", pct(0.5), " p99=", pct(0.99), " p99.9=", pct(0.999),
                         " p99.99=", pct(0.9999), " max=", lum.back());
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
        AverageInPlace(raw);

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
        AverageInPlace(raw);

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

            bool materialsDirty = (m_scene->materials != m_lastMaterials);
            if (m_scene->camera != m_lastCamera || m_settings != m_lastSettings || materialsDirty)
            {
                if (materialsDirty)
                {
                    m_bvh->UpdateMaterials(*m_scene);
                    // Emissive edits re-weight (or add/remove) light tree
                    // leaves, so the tree built at startup goes stale too.
                    m_lightTree->Build(*m_scene);
                }
                if (m_settings.lens != m_lastSettings.lens || m_settings.cameraMode != m_lastSettings.cameraMode)
                {
                    m_lensSystem->Derive(m_settings.lens);
                    m_pathTracer->MarkPupilsDirty();
                }
                m_pathTracer->Reset();
                m_lastCamera = m_scene->camera;
                m_lastSettings = m_settings;
                m_lastMaterials = m_scene->materials;
                m_dirty = false;
            }

            m_pathTracer->Render(m_scene->camera, *m_bvh, *m_lightTree, *m_lensSystem, m_settings);

            if (m_statsEnabled)
            {
                uint32_t n = m_pathTracer->SampleCount();
                if (n > 0 && n % 256 == 0 && n != m_lastStatsSample)
                {
                    m_lastStatsSample = n;
                    LogAccumulationStats();
                }
            }

            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glm::ivec2 fbSize = m_window->GetFramebufferSize();
            glViewport(0, 0, fbSize.x, fbSize.y);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            m_fullscreenPass->Draw(m_pathTracer->AccumulationImage(), m_settings.exposure, m_pathTracer->SampleCount());

            m_ui->BeginFrame();
            UIFrameResult result = m_ui->Draw(m_settings, *m_scene, m_pathTracer->SampleCount(),
                                               dt * 1000.0f, Denoiser::IsAvailable());
            if (result.denoiseRequested) RunDenoiser();
            if (result.exportRequested) ExportPNG(result.exportPath);
            m_ui->EndFrame();

            m_window->SwapBuffers();
        }
    }
}
