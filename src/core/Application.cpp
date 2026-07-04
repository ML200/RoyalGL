#include "core/Application.h"
#include "core/Log.h"
#include "scene/GLTFLoader.h"
#include "io/ImageExport.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

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
        // Built-in scene composition (UI-editable later): seed from the
        // headless test envs BEFORE the first load so soak recipes keep
        // working unchanged.
        if (const char* v = std::getenv("ROYALGL_DUCKS"))
            m_duckCount = std::clamp(std::atoi(v), 0, 8);
        if (const char* v = std::getenv("ROYALGL_MAT"))
        {
            std::string m = v;
            m_duckMaterial = (m == "conductor")  ? 1
                           : (m == "roughglass") ? 2
                           : (m == "layered")    ? 3
                           : (m == "layeredmed") ? 4 : 0;
        }
        bool startupLoaded = LoadScene(desc.startupScenePath);

        // Scene picker list: built-in first, then the bundled .glb scenes.
        m_sceneNames.push_back("Cornell box + duck (built-in)");
        m_scenePaths.push_back({});
        {
            std::vector<std::filesystem::path> found;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(
                     std::filesystem::path(ROYALGL_ASSET_DIR) / "scenes", ec))
            {
                if (entry.path().extension() != ".glb") continue;
                if (entry.path().stem() == "Duck") continue; // prop, not a scene
                found.push_back(entry.path());
            }
            std::sort(found.begin(), found.end());
            for (const auto& p : found)
            {
                m_sceneNames.push_back(p.stem().string());
                m_scenePaths.push_back(p);
            }
        }
        if (startupLoaded)
        {
            std::filesystem::path sp = desc.startupScenePath;
            m_sceneIndex = -1;
            std::error_code eqec;
            for (size_t i = 1; i < m_scenePaths.size(); ++i)
            {
                if (std::filesystem::equivalent(m_scenePaths[i], sp, eqec))
                {
                    m_sceneIndex = static_cast<int>(i);
                    break;
                }
            }
            if (m_sceneIndex < 0) // custom path outside assets/scenes
            {
                m_sceneNames.push_back(sp.stem().string() + " (custom)");
                m_scenePaths.push_back(sp);
                m_sceneIndex = static_cast<int>(m_scenePaths.size()) - 1;
            }
            // Recommended framing for the bundled scenes; the env camera
            // override below still wins for scripted runs.
            ApplyScenePreset(sp);
        }

        m_bvh = std::make_unique<BVHBuilder>();
        m_bvh->Build(*m_scene);
        m_lastMaterials = m_scene->materials;

        m_lightTree = std::make_unique<LightTree>();
        m_lightTree->Build(*m_scene);

        // Scan the lens preset directory; default to the Tessar when found.
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(
                 std::filesystem::path(ROYALGL_ASSET_DIR) / "lenses", ec))
        {
            if (entry.path().extension() == ".lens")
                m_lensPresetPaths.push_back(entry.path());
        }
        std::sort(m_lensPresetPaths.begin(), m_lensPresetPaths.end());
        m_lensSystem = std::make_unique<LensSystem>();
        for (size_t i = 0; i < m_lensPresetPaths.size(); ++i)
        {
            LensSystem probe;
            m_lensPresetNames.push_back(probe.LoadLensFile(m_lensPresetPaths[i]) ? probe.Name()
                                                                                  : m_lensPresetPaths[i].stem().string());
            if (m_lensPresetPaths[i].stem() == "tessar") m_settings.lens.presetIndex = static_cast<int>(i);
        }
        if (m_lensPresetPaths.empty() ||
            !m_lensSystem->LoadLensFile(m_lensPresetPaths[m_settings.lens.presetIndex]))
            m_lensSystem->LoadBuiltinTessar();
        m_lensSystem->Derive(m_settings.lens);

        m_pathTracer = std::make_unique<PathTracer>();
        glm::ivec2 fbSize = m_window->GetFramebufferSize();
        m_pathTracer->Resize(fbSize.x, fbSize.y);

        m_fullscreenPass = std::make_unique<FullscreenPass>();
        m_denoiser = std::make_unique<Denoiser>();
        m_ui = std::make_unique<UILayer>(m_window->Handle());

        // Env-var overrides for scripted A/B experiments (no rebuild):
        // ROYALGL_RESTIR=0/1 (off = plain BDPT reference), ROYALGL_STATS=1.
        if (const char* v = std::getenv("ROYALGL_RESTIR")) m_settings.enableRestir = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_DEBUG")) m_settings.restirDebugView = std::atoi(v);
        if (const char* v = std::getenv("ROYALGL_RESTIR_TEMPORAL")) m_settings.restirTemporal = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_SPATIAL")) m_settings.restirSpatial = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_SPATIAL_NBRS"))
            m_settings.restirSpatialNeighbors = std::max(std::atoi(v), 0);
        if (const char* v = std::getenv("ROYALGL_RESTIR_STRAT")) m_settings.restirStratified = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_SPMODE"))
            m_settings.restirSpatialMode = std::clamp(std::atoi(v), 0, 2);
        if (const char* v = std::getenv("ROYALGL_RESTIR_VSCORE")) m_settings.restirShiftScore = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_CELLSEARCH")) m_settings.restirCellSearch = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_SEARCH_ITERS"))
            m_settings.restirSearchIters = std::clamp(std::atoi(v), 1, 31);
        if (const char* v = std::getenv("ROYALGL_RESTIR_SEARCH_RADIUS"))
            m_settings.restirSearchRadius = std::clamp(std::atoi(v), 1, 255);
        if (const char* v = std::getenv("ROYALGL_RESTIR_SEARCH_GATE"))
            m_settings.restirSearchGate = std::clamp(std::atoi(v), 0, 3);
        if (const char* v = std::getenv("ROYALGL_EXPOSURE"))
            m_settings.exposure = std::clamp(static_cast<float>(std::atof(v)), 0.01f, 100.0f);
        if (const char* v = std::getenv("ROYALGL_RESTIR_CLUSTER_NORMAL")) m_settings.restirClusterNormal = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_PSEL")) m_settings.restirProbeSelection = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_SHIFTDIAG"))
            m_settings.restirShiftDiag = std::clamp(std::atoi(v), 0, 6);
        if (const char* v = std::getenv("ROYALGL_RESTIR_SHIFTDIAG_DIST"))
            m_settings.restirShiftDiagDist = std::clamp(std::atoi(v), 0, 512);
        if (const char* v = std::getenv("ROYALGL_RESTIR_EMA"))
            m_settings.restirScoreEmaRate = std::clamp(static_cast<float>(std::atof(v)), 0.01f, 1.0f);
        if (const char* v = std::getenv("ROYALGL_RESTIR_DEFMIX"))
            m_settings.restirScoreDefMix = std::clamp(static_cast<float>(std::atof(v)), 0.0f, 1.0f);
        if (const char* v = std::getenv("ROYALGL_RESTIR_SPATIAL_ITERS"))
            m_settings.restirSpatialIterations = std::clamp(std::atoi(v), 1, 8);
        if (const char* v = std::getenv("ROYALGL_RESTIR_DECORR")) m_settings.restirDecorrelate = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_VOLMODE"))
            m_settings.restirVolumeMode = std::clamp(std::atoi(v), 0, 2);
        // Back-compat alias: VOLRC=0 = replay-only, 1 = volume-anchored.
        if (const char* v = std::getenv("ROYALGL_RESTIR_VOLRC")) m_settings.restirVolumeMode = (v[0] != '0') ? 2 : 1;
        if (const char* v = std::getenv("ROYALGL_RESTIR_FOGPAIR")) m_settings.restirFogPairing = (v[0] != '0');
        // Moved from PathTracer: the caustic-pass switch is a RenderSettings
        // field now (UI toggle), the env stays as the scripted override.
        if (const char* v = std::getenv("ROYALGL_RESTIR_CAUSTIC")) m_settings.restirCausticReuse = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_NL"))
            m_settings.restirLightPaths = std::max(std::atoi(v), 4096);
        if (const char* v = std::getenv("ROYALGL_BOUNCES")) m_settings.maxBounces = std::max(std::atoi(v), 1);
        if (const char* v = std::getenv("ROYALGL_RESTIR_CAP")) m_settings.restirConfidenceCap = static_cast<float>(std::max(std::atoi(v), 1));
        // Homogeneous fog for volumetric soaks: ROYALGL_FOG="sigmaS,sigmaA,g".
        if (const char* v = std::getenv("ROYALGL_FOG"))
        {
            float f[3] = {0.15f, 0.02f, 0.0f};
            if (sscanf(v, "%f,%f,%f", &f[0], &f[1], &f[2]) >= 1)
            {
                m_settings.fogEnable = f[0] > 0.0f || f[1] > 0.0f;
                m_settings.fogSigmaS = f[0];
                m_settings.fogSigmaA = f[1];
                m_settings.fogG = f[2];
            }
        }
        if (const char* v = std::getenv("ROYALGL_RESTIR_LIGHT")) m_settings.restirLightTracing = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_CONN")) m_settings.restirConnections = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_RESTIR_MISFIX")) m_settings.restirRecomputeMis = (v[0] != '0');
        // Ignore all camera input - keeps scripted soak tests deterministic
        // even if the window is focused or the mouse passes over it.
        m_cameraLocked = (std::getenv("ROYALGL_LOCK_CAMERA") != nullptr);
        if (const char* v = std::getenv("ROYALGL_ORBIT")) m_orbitSpeed = static_cast<float>(std::atof(v));
        if (const char* v = std::getenv("ROYALGL_DOLLY")) m_dollySpeed = static_cast<float>(std::atof(v));
        if (const char* v = std::getenv("ROYALGL_MOVE")) m_moveTestSpeed = static_cast<float>(std::atof(v));
        if (const char* v = std::getenv("ROYALGL_FIXED_DT")) m_fixedDt = static_cast<float>(std::atof(v));
        if (const char* v = std::getenv("ROYALGL_LENS")) m_settings.cameraMode = (v[0] != '0') ? CameraMode::Lens : CameraMode::Pinhole;
        m_statsEnabled = (std::getenv("ROYALGL_STATS") != nullptr);
        if (const char* v = std::getenv("ROYALGL_STATS_MASK")) m_statsMaskEnabled = (v[0] != '0');
        if (const char* v = std::getenv("ROYALGL_STATS_INTERVAL")) m_statsInterval = std::max(std::atoi(v), 1);
        if (const char* v = std::getenv("ROYALGL_ACCUM")) m_settings.accumulate = (v[0] != '0');

        // Headless test hook for the runtime scene-switch path (the same
        // deferred path UI clicks take): ROYALGL_SWITCH_TEST="index,frame"
        // requests scene <index> once the frame counter passes <frame>.
        if (const char* v = std::getenv("ROYALGL_SWITCH_TEST"))
        {
            int i = 0, f = 0;
            if (sscanf(v, "%d,%d", &i, &f) == 2)
            {
                m_switchTestIndex = i;
                m_switchTestFrame = f;
            }
        }

        // Scripted camera pose for headless artifact repros:
        // ROYALGL_CAM="px,py,pz,tx,ty,tz"
        if (const char* v = std::getenv("ROYALGL_CAM"))
        {
            float c[6];
            if (sscanf(v, "%f,%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) == 6)
            {
                m_scene->camera.position = glm::vec3(c[0], c[1], c[2]);
                m_scene->camera.target = glm::vec3(c[3], c[4], c[5]);
            }
        }

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

    void Application::ApplyScenePreset(const std::filesystem::path& path)
    {
        std::string stem = path.stem().string();
        if (stem == "LightMaze")
        {
            // Sparse-carrier stress scene (docs/make_maze_scene.py): the
            // recommended dark-end framing; no medium.
            m_scene->camera.position = glm::vec3(1.45f, 1.25f, 1.5f);
            m_scene->camera.target = glm::vec3(-1.2f, 0.7f, -1.0f);
            m_settings.fogEnable = false;
        }
        else if (stem == "LensFog")
        {
            // Volumetric caustic showcase (docs/make_lens_scene.py): the
            // paper's camera and fog recipe.
            m_scene->camera.position = glm::vec3(2.2f, 1.4f, 3.2f);
            m_scene->camera.target = glm::vec3(0.0f, 1.2f, 0.0f);
            m_settings.fogEnable = true;
            m_settings.fogSigmaS = 0.30f;
            m_settings.fogSigmaA = 0.02f;
            m_settings.fogG = 0.5f;
        }
        else if (path.empty())
        {
            // Built-in Cornell: LoadFallbackScene set its own camera.
            m_settings.fogEnable = false;
        }
        // Unknown custom scenes: keep whatever the file/loader provided.
    }

    void Application::PerformSceneSwitch(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_scenePaths.size())) return;
        m_sceneIndex = index;
        LoadScene(m_scenePaths[index].string());
        ApplyScenePreset(m_scenePaths[index]);
        m_bvh->Build(*m_scene);
        m_lightTree->Build(*m_scene);
        // Cross-scene reservoirs/G-buffers reference dead geometry in
        // object space - both the accumulation and the ReSTIR history must
        // restart from scratch.
        m_pathTracer->Reset();
        m_pathTracer->ClearRestirHistory();
        m_lastMaterials = m_scene->materials;
        m_lastCamera = m_scene->camera;
        m_lastSettings = m_settings;
        ROYALGL_LOG_INFO("Application: switched to scene '", m_sceneNames[index],
                         "' (", m_scene->triangles.size(), " triangles).");
    }

    bool Application::LoadScene(const std::string& path)
    {
        bool loaded = false;
        if (!path.empty())
        {
            Scene loadedScene;
            if (GLTFLoader::Load(path, loadedScene))
            {
                *m_scene = std::move(loadedScene);
                // glTF scenes arrive flattened; register the whole file as
                // one movable instance.
                m_scene->RegisterInstance(std::filesystem::path(path).filename().string(), 0);
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

                // Main duck material preset (UI "Duck material" combo /
                // ROYALGL_MAT env; the presets soak tests exercise -
                // integrator cross-checks must agree on every lobe).
                switch (m_duckMaterial)
                {
                case 1: // conductor
                    glass = Material{};
                    glass.type = MaterialType::Conductor;
                    glass.baseColor = glm::vec3(0.95f, 0.64f, 0.54f); // copper-ish F0
                    glass.roughness = 0.3f;
                    break;
                case 2: // rough glass
                    glass = Material{};
                    glass.type = MaterialType::RoughDielectric;
                    glass.baseColor = glm::vec3(0.98f);
                    glass.roughness = 0.2f;
                    glass.ior = 1.5f;
                    break;
                case 3: // clear coat over rough copper
                    glass = Material{};
                    glass.type = MaterialType::Layered;
                    glass.baseColor = glm::vec3(0.95f, 0.64f, 0.54f);
                    glass.metallic = 1.0f;
                    glass.roughness = 0.4f;
                    glass.coatRoughness = 0.1f;
                    glass.coatIor = 1.5f;
                    glass.coatDepth = 0.0f;
                    break;
                case 4: // coat + blue-tinted scattering medium over diffuse
                    glass = Material{};
                    glass.type = MaterialType::Layered;
                    glass.baseColor = glm::vec3(0.8f);
                    glass.metallic = 0.0f;
                    glass.roughness = 0.5f;
                    glass.coatRoughness = 0.15f;
                    glass.coatIor = 1.5f;
                    glass.coatDepth = 1.0f;
                    glass.coatG = 0.4f;
                    glass.coatAlbedo = glm::vec3(0.4f, 0.6f, 0.9f);
                    break;
                default: break; // 0 = glass
                }
                m_scene->MergeInstance(duck, glm::vec3(0.4f, 0.0f, 0.2f), 1.0f, glass, "Glass duck");

                // Clutter ducks (UI slider / ROYALGL_DUCKS env): up to 8
                // extra ducks with varied materials/positions - a clutter
                // variant of the fallback scene for spatial-reuse stress
                // tests (fragmented reuse clusters, many silhouettes,
                // glossy shift failures). Default 0 keeps every recorded
                // soak reference valid.
                // Instance cap: box + duck + 8 = 10 <= kMaxRestirInstances.
                {
                    int extra = std::clamp(m_duckCount, 0, 8);
                    struct DuckSpec { glm::vec3 pos; float scale; int mat; };
                    static const DuckSpec specs[8] = {
                        {{-0.55f, 0.00f, -0.35f}, 0.85f, 0}, // rough copper
                        {{ 0.10f, 0.00f, -0.55f}, 0.70f, 1}, // shiny conductor
                        {{-0.20f, 0.00f,  0.45f}, 0.60f, 2}, // green diffuse
                        {{ 0.65f, 0.00f, -0.25f}, 0.75f, 3}, // rough glass
                        {{-0.70f, 0.00f,  0.25f}, 0.65f, 4}, // layered coat
                        {{ 0.30f, 0.35f,  0.55f}, 0.55f, 1}, // floating, shiny
                        {{-0.15f, 0.45f, -0.15f}, 0.50f, 0}, // floating, copper
                        {{ 0.72f, 0.00f,  0.50f}, 0.60f, 2}, // blue diffuse
                    };
                    for (int i = 0; i < extra; ++i)
                    {
                        const DuckSpec& d = specs[i];
                        Material mat;
                        switch (d.mat)
                        {
                        case 0:
                            mat.type = MaterialType::Conductor;
                            mat.baseColor = glm::vec3(0.95f, 0.64f, 0.54f);
                            mat.roughness = 0.35f;
                            break;
                        case 1:
                            mat.type = MaterialType::Conductor;
                            mat.baseColor = glm::vec3(0.9f, 0.9f, 0.92f);
                            mat.roughness = 0.08f;
                            break;
                        case 2:
                            mat.baseColor = (i == 7) ? glm::vec3(0.25f, 0.35f, 0.8f)
                                                     : glm::vec3(0.25f, 0.7f, 0.3f);
                            break;
                        case 3:
                            mat.type = MaterialType::RoughDielectric;
                            mat.baseColor = glm::vec3(0.98f);
                            mat.roughness = 0.2f;
                            mat.ior = 1.5f;
                            break;
                        default:
                            mat.type = MaterialType::Layered;
                            mat.baseColor = glm::vec3(0.95f, 0.64f, 0.54f);
                            mat.metallic = 1.0f;
                            mat.roughness = 0.4f;
                            mat.coatRoughness = 0.1f;
                            mat.coatIor = 1.5f;
                            mat.coatDepth = 0.0f;
                            break;
                        }
                        m_scene->MergeInstance(duck, d.pos, d.scale, mat,
                                               ("Clutter duck " + std::to_string(i)).c_str());
                    }
                    if (extra > 0)
                        ROYALGL_LOG_INFO("Application: ", extra,
                                         " clutter ducks merged into the fallback scene.");
                }
            }
            else
            {
                ROYALGL_LOG_WARN("Application: Duck.glb not found, fallback scene has no glass object.");
            }
        }
        m_instanceDirty.assign(m_scene->instances.size(), false);
        return loaded;
    }

    void Application::OnFramebufferResize(int width, int height)
    {
        if (width <= 0 || height <= 0) return;
        m_pathTracer->Resize(width, height);
    }

    void Application::HandleCameraInput(float dt)
    {
        if (m_cameraLocked) return;
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
        // Pixels brighter than any directly visible emitter (25): reservoir
        // W blow-ups, not scene content.
        size_t hot = lum.end() - std::upper_bound(lum.begin(), lum.end(), 30.0f);

        ROYALGL_LOG_INFO("Stats @", n, " samples: mean=", mean, " relNoise=", relNoise,
                         " p50=", pct(0.5), " p99=", pct(0.99), " p99.9=", pct(0.999),
                         " p99.99=", pct(0.9999), " max=", lum.back(), " hot=", hot);

        // ROYALGL_STATS_SPMIS: per-frame check of the stochastic-MIS
        // selection identity E[sum 1/(N~ P count)] = 1 (accumulated by
        // smerge into the learning region's .w). Systematic deviation of
        // the pixel-mean from 1 localizes a selection-probability
        // accounting error.
        static const bool spmisDebug = (std::getenv("ROYALGL_STATS_SPMIS") != nullptr);
        if (spmisDebug)
        {
            std::vector<float> region = m_pathTracer->ReadLearnRegion();
            if (!region.empty())
            {
                double sum = 0.0, sum2 = 0.0;
                size_t cnt = 0;
                for (size_t i = 3; i < region.size(); i += 4)
                {
                    float v = region[i];
                    if (v > 0.0f && std::isfinite(v))
                    {
                        sum += v; sum2 += double(v) * v; cnt++;
                    }
                }
                if (cnt)
                {
                    double m = sum / cnt;
                    ROYALGL_LOG_INFO("SpmisIdent: mean=", m, " rms=",
                                     std::sqrt(sum2 / cnt), " n=", cnt);
                }
            }
        }

        if (m_statsMaskEnabled)
        {
            // Same noise measure restricted to pixels the temporal pass
            // flagged as disoccluded (mask == 1): the region where spatial
            // candidate selection quality shows. NOTE: lum was sorted above
            // for percentiles, so re-derive the unsorted luminances here.
            std::vector<float> mask = m_pathTracer->ReadDisocclusionMask();
            if (mask.size() == static_cast<size_t>(w) * static_cast<size_t>(h))
            {
                std::vector<float> lu;
                lu.reserve(mask.size());
                for (size_t i = 0; i + 3 < raw.size(); i += 4)
                    lu.push_back((raw[i] + raw[i + 1] + raw[i + 2]) / (3.0f * std::max(raw[i + 3], 1.0f)));
                double mSum = 0.0, mNoise = 0.0;
                size_t mCount = 0, mNoiseCount = 0;
                for (int y = 1; y < h - 1; ++y)
                {
                    for (int x = 1; x < w - 1; ++x)
                    {
                        size_t i = static_cast<size_t>(y) * w + x;
                        if (mask[i] < 0.5f) continue;
                        float c = lu[i];
                        mSum += c;
                        mCount++;
                        if (c > 5.0f) continue;
                        float nb = 0.25f * (lu[i - 1] + lu[i + 1] + lu[i - w] + lu[i + w]);
                        mNoise += std::abs(c - nb);
                        mNoiseCount++;
                    }
                }
                double mMean = mCount ? mSum / mCount : 0.0;
                double mRel = (mNoiseCount && mMean > 0.0) ? (mNoise / mNoiseCount) / mMean : 0.0;
                ROYALGL_LOG_INFO("MaskStats @", n, " samples: frac=",
                                 static_cast<double>(mCount) / (static_cast<double>(w) * h),
                                 " mean=", mMean, " relNoise=", mRel);
            }
        }

        if (m_settings.restirShiftDiag != 0)
        {
            // Dual-direction shift diagnostic: reduce the cross-frame
            // tallies smerge accumulates in the learn region (.y / .w -
            // semantics per sub-mode, see restir_wf_smerge.comp).
            std::vector<float> lr = m_pathTracer->ReadLearnRegion();
            if (!lr.empty())
            {
                double sumY = 0.0, sumW = 0.0;
                size_t nz = 0;
                for (size_t i = 0; i + 3 < lr.size(); i += 4)
                {
                    sumY += lr[i + 1];
                    sumW += lr[i + 3];
                    if (lr[i + 1] != 0.0f || lr[i + 3] != 0.0f) nz++;
                }
                ROYALGL_LOG_INFO("ShiftDiag mode=", m_settings.restirShiftDiag,
                                 " @", n, " samples: sumY=", sumY,
                                 " sumW=", sumW, " pixels=", nz);
            }
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
            // Scripted switch test fires through the exact UI path.
            if (m_switchTestIndex >= 0 && ++m_switchTestTick >= m_switchTestFrame)
            {
                m_pendingSceneIndex = m_switchTestIndex;
                m_switchTestIndex = -1;
            }
            // Deferred scene switches from the UI: only at a frame boundary
            // with no async BVH rebuild in flight (the worker still owns
            // scene-derived buffers while busy).
            if ((m_pendingSceneIndex >= 0 || m_pendingSceneReload) && !m_bvh->AsyncBusy())
            {
                PerformSceneSwitch(m_pendingSceneIndex >= 0 ? m_pendingSceneIndex : m_sceneIndex);
                m_pendingSceneIndex = -1;
                m_pendingSceneReload = false;
            }
            // ROYALGL_FIXED_DT=<seconds>: advance the scripted motions below
            // by a constant per-frame step instead of wall-clock time, so
            // A/B runs of configs with different frame costs see IDENTICAL
            // per-frame scene changes (same disocclusion widths etc.).
            if (m_fixedDt > 0.0f) dt = m_fixedDt;
            // Scripted rocking yaw (works with LOCK_CAMERA - that only mutes
            // input): direction flips every 2s, so scene walls repeatedly
            // enter the frame edges at grazing angles - the repro case for
            // temporal-reuse transients.
            if (m_orbitSpeed != 0.0f)
            {
                m_orbitPhase += dt;
                float sign = (std::fmod(m_orbitPhase, 4.0f) < 2.0f) ? 1.0f : -1.0f;
                m_scene->camera.Look(m_orbitSpeed * dt * sign, 0.0f);
            }
            // Scripted lateral truck (ROYALGL_DOLLY=<units/s>, flip every
            // 2s): TRANSLATION, i.e. parallax - which a rocking yaw never
            // produces. The repro case for reprojection-pairing losses
            // (fog history vs surface anchors move differently on screen).
            if (m_dollySpeed != 0.0f)
            {
                m_dollyPhase += dt;
                float sign = (std::fmod(m_dollyPhase, 4.0f) < 2.0f) ? 1.0f : -1.0f;
                glm::vec3 fwd = m_scene->camera.target - m_scene->camera.position;
                glm::vec3 right = glm::normalize(glm::cross(fwd, glm::vec3(0.0f, 1.0f, 0.0f)));
                glm::vec3 step = right * (m_dollySpeed * dt * sign);
                m_scene->camera.position += step;
                m_scene->camera.target += step;
            }
            // Scripted instance move (ROYALGL_MOVE=<rad/s>): oscillates the
            // last instance's X position - exercises the async BLAS/TLAS
            // rebuild pipeline exactly like UI transform edits do.
            if (m_moveTestSpeed != 0.0f && !m_scene->instances.empty())
            {
                m_movePhase += dt * m_moveTestSpeed;
                m_scene->instances.back().position.x = 0.5f * std::sin(m_movePhase);
                if (!m_instanceDirty.empty()) m_instanceDirty.back() = true;
            }

            bool materialsDirty = (m_scene->materials != m_lastMaterials);
            if (m_scene->camera != m_lastCamera || m_settings != m_lastSettings || materialsDirty)
            {
                ROYALGL_LOG_INFO("Application: accumulation reset (camera=",
                                 (m_scene->camera != m_lastCamera), " settings=",
                                 (m_settings != m_lastSettings), " materials=", materialsDirty, ")");
                if (materialsDirty)
                {
                    m_bvh->UpdateMaterials(*m_scene);
                    // Emissive edits re-weight (or add/remove) light tree
                    // leaves, so the tree built at startup goes stale too.
                    m_lightTree->Build(*m_scene);
                }
                if (m_settings.lens != m_lastSettings.lens || m_settings.cameraMode != m_lastSettings.cameraMode)
                {
                    if (m_settings.lens.presetIndex != m_lastSettings.lens.presetIndex &&
                        m_settings.lens.presetIndex >= 0 &&
                        m_settings.lens.presetIndex < static_cast<int>(m_lensPresetPaths.size()))
                    {
                        m_lensSystem->LoadLensFile(m_lensPresetPaths[m_settings.lens.presetIndex]);
                    }
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
                // Under a scripted orbit the camera change resets the sample
                // count every frame, so trigger on the wall-clock frame
                // counter instead (per-frame estimate statistics).
                ++m_statsFrame;
                bool fire = (m_orbitSpeed != 0.0f || m_moveTestSpeed != 0.0f || m_dollySpeed != 0.0f)
                                ? (m_statsFrame % static_cast<uint32_t>(m_statsInterval) == 0)
                                : (n > 0 && n % m_statsInterval == 0 && n != m_lastStatsSample);
                if (fire)
                {
                    m_lastStatsSample = n;
                    // Wall-clock stamp for equal-time comparisons (offline
                    // plots divide sample deltas by time deltas).
                    static const auto statsT0 = clock::now();
                    ROYALGL_LOG_INFO("StatsTime @", n, " samples: ms=",
                                     std::chrono::duration<double, std::milli>(clock::now() - statsT0).count());
                    LogAccumulationStats();
                }
            }

            // Raw float frame series for offline convergence metrics:
            // ROYALGL_EXPORT_SERIES=<prefix> dumps the averaged accum buffer
            // as raw RGBA32F (<prefix>_<n>.f32) whenever the sample count
            // reaches a multiple of ROYALGL_EXPORT_STRIDE (default 1), up to
            // ROYALGL_EXPORT_FRAMES (default 32). 8-bit PNG exports quantize
            // away exactly the low-variance differences convergence plots
            // measure, hence raw floats.
            static const char* seriesEnv = std::getenv("ROYALGL_EXPORT_SERIES");
            if (seriesEnv)
            {
                static uint32_t seriesMax = [] {
                    const char* v = std::getenv("ROYALGL_EXPORT_FRAMES");
                    return v ? static_cast<uint32_t>(std::max(std::atoi(v), 1)) : 32u;
                }();
                static uint32_t seriesStride = [] {
                    const char* v = std::getenv("ROYALGL_EXPORT_STRIDE");
                    return v ? static_cast<uint32_t>(std::max(std::atoi(v), 1)) : 1u;
                }();
                static uint32_t seriesLast = 0;
                uint32_t n = m_pathTracer->SampleCount();
                if (n != seriesLast && n <= seriesMax && n % seriesStride == 0)
                {
                    seriesLast = n;
                    std::vector<float> raw = m_pathTracer->AccumulationImage().ReadPixelsFloat();
                    AverageInPlace(raw);
                    char name[512];
                    std::snprintf(name, sizeof(name), "%s_%05u.f32", seriesEnv, n);
                    std::ofstream f(name, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(raw.data()),
                            static_cast<std::streamsize>(raw.size() * sizeof(float)));
                    ROYALGL_LOG_INFO("Application: series dump ", name, " (", m_pathTracer->Width(),
                                     "x", m_pathTracer->Height(), " RGBA32F)");
                }
            }

            // Headless render dump: ROYALGL_EXPORT=<path> exports the frame
            // once ROYALGL_EXPORT_AT samples accumulated (default 256).
            static const char* exportEnv = std::getenv("ROYALGL_EXPORT");
            static bool exported = false;
            if (exportEnv && !exported)
            {
                uint32_t at = 256;
                if (const char* v = std::getenv("ROYALGL_EXPORT_AT"))
                    at = static_cast<uint32_t>(std::max(std::atoi(v), 1));
                if (m_pathTracer->SampleCount() >= at)
                {
                    ExportPNG(exportEnv);
                    exported = true;
                }
            }

            // Headless denoise check: ROYALGL_DENOISE=1 runs the denoiser
            // once (writes denoised.png) after ROYALGL_EXPORT_AT samples
            // accumulated (default 256).
            static const char* denoiseEnv = std::getenv("ROYALGL_DENOISE");
            static bool denoised = false;
            if (denoiseEnv && denoiseEnv[0] != '0' && !denoised)
            {
                uint32_t at = 256;
                if (const char* v = std::getenv("ROYALGL_EXPORT_AT"))
                    at = static_cast<uint32_t>(std::max(std::atoi(v), 1));
                if (m_pathTracer->SampleCount() >= at)
                {
                    RunDenoiser();
                    denoised = true;
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
                                               dt * 1000.0f, Denoiser::IsAvailable(), m_lensPresetNames,
                                               m_sceneNames, m_sceneIndex, m_duckCount, m_duckMaterial);
            if (result.denoiseRequested) RunDenoiser();
            if (result.exportRequested) ExportPNG(result.exportPath);
            if (result.sceneSelected >= 0 && result.sceneSelected != m_sceneIndex)
                m_pendingSceneIndex = result.sceneSelected;
            if (result.sceneCompositionChanged && m_sceneIndex == 0)
                m_pendingSceneReload = true;
            m_ui->EndFrame();

            // ----------------------- async instance-move BVH rebuilds -----
            // UI edits only touch the instance TRS; the world triangles +
            // BLAS + TLAS are rebuilt on a worker thread (the build is CPU-
            // side), so dragging never hitches the render loop. Rendering
            // keeps using the previous consistent BVH until PumpAsync lands
            // the new one; edits arriving mid-build are coalesced through
            // the dirty flags and picked up by the next job.
            if (result.instanceMoved >= 0 &&
                result.instanceMoved < static_cast<int>(m_instanceDirty.size()))
                m_instanceDirty[result.instanceMoved] = true;
            if (m_bvh->PumpAsync(*m_scene))
            {
                // New geometry landed: emissive triangles may have moved
                // (the light tree is cheap) and accumulation restarts. The
                // ReSTIR reservoirs are kept: shifts re-trace prefixes in
                // the new scene and their bijectivity/occlusion checks
                // reject most stale reuse; residual staleness (cached
                // suffix data referencing the old placement) washes out
                // within ~confidence-cap frames - the accepted speed/
                // correctness tradeoff (the paper-faithful full re-trace
                // was measured too slow).
                m_lightTree->Build(*m_scene);
                m_pathTracer->Reset();

                // End-of-burst history clear: DURING a drag (another rebuild
                // already pending) history is kept - reuse keeps motion
                // cheap and the transient staleness is invisible while
                // things move. But when the LAST rebuild of an edit lands,
                // the restarted accumulation would bake the ~confidence-cap
                // frames of stale-history transient into the converged
                // image as a persistent shadow-like blotch near the moved
                // geometry. One reservoir clear on the settle frame costs a
                // single frame of reuse and removes the bake-in.
                bool morePending = m_bvh->AsyncBusy();
                for (size_t i = 0; i < m_instanceDirty.size() && !morePending; ++i)
                    morePending = m_instanceDirty[i];
                if (!morePending)
                    m_pathTracer->ClearRestirHistory();
            }
            if (!m_bvh->AsyncBusy())
            {
                for (size_t i = 0; i < m_instanceDirty.size(); ++i)
                {
                    if (!m_instanceDirty[i]) continue;
                    if (m_bvh->RequestInstanceRebuild(*m_scene, i))
                        m_instanceDirty[i] = false;
                    break;
                }
            }

            m_window->SwapBuffers();
        }
    }
}
