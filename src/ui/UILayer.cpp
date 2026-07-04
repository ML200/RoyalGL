#include "ui/UILayer.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <algorithm>
#include <vector>
#include <string>

namespace RoyalGL
{
    UILayer::UILayer(GLFWwindow* window)
        : m_window(window)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

        ImGui::StyleColorsDark();

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 460");
    }

    UILayer::~UILayer()
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    void UILayer::BeginFrame() const
    {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void UILayer::EndFrame() const
    {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    UIFrameResult UILayer::Draw(RenderSettings& settings, Scene& scene, uint32_t sampleCount, float frameTimeMs,
                                 bool oidnAvailable, const std::vector<std::string>& lensPresetNames,
                                 const std::vector<std::string>& sceneNames, int sceneIndex,
                                 int& duckCount, int& duckMaterial)
    {
        UIFrameResult result;

        ImGui::Begin("RoyalGL");

        // Stats
        ImGui::Text("Samples: %u", sampleCount);
        ImGui::Text("Frame time: %.2f ms (%.1f FPS)", frameTimeMs, 1000.0f / std::max(frameTimeMs, 0.001f));
        ImGui::Text("Triangles: %zu", scene.triangles.size());

        ImGui::Separator();

        // Scene picker: built-in Cornell + the bundled assets/scenes .glbs.
        // Switching is deferred by Application to a safe frame boundary
        // (full BVH/light-tree rebuild + ReSTIR history reset).
        if (!sceneNames.empty())
        {
            int idx = std::clamp(sceneIndex, 0, static_cast<int>(sceneNames.size()) - 1);
            if (ImGui::BeginCombo("Scene", sceneNames[idx].c_str()))
            {
                for (int i = 0; i < static_cast<int>(sceneNames.size()); ++i)
                {
                    if (ImGui::Selectable(sceneNames[i].c_str(), i == idx) && i != idx)
                        result.sceneSelected = i;
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Bundled scenes load with their recommended camera (and fog,\n"
                                  "for LensFog). Switching rebuilds the BVH and restarts\n"
                                  "accumulation and ReSTIR history.");
            if (idx == 0)
            {
                // Built-in scene composition - takes effect via a reload.
                bool comp = false;
                comp |= ImGui::SliderInt("Clutter ducks", &duckCount, 0, 8);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Extra ducks with varied materials (the spatial-reuse\n"
                                      "clutter stress scene). 0 = the recorded-soak-reference\n"
                                      "scene. Applies by reloading the scene.");
                const char* duckMats[] = {"Glass", "Conductor (copper)", "Rough glass",
                                          "Layered coat", "Layered + medium"};
                comp |= ImGui::Combo("Duck material", &duckMaterial, duckMats, 5);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Main duck material preset (the ROYALGL_MAT soak presets).");
                result.sceneCompositionChanged = comp;
            }
        }

        ImGui::Separator();

        // Camera (read-only - mouse-drag already controls it)
        ImGui::Text("Pos: %.2f %.2f %.2f", scene.camera.position.x, scene.camera.position.y, scene.camera.position.z);
        ImGui::Text("Target: %.2f %.2f %.2f", scene.camera.target.x, scene.camera.target.y, scene.camera.target.z);
        ImGui::Text("FOV: %.1f deg", scene.camera.verticalFovDegrees);

        ImGui::Separator();

        // Render settings
        RenderSettings before = settings;
        ImGui::SliderInt("Max bounces", &settings.maxBounces, 1, 32);
        ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::ColorEdit3("Background", &settings.backgroundColor.x);
        ImGui::SliderFloat("Background intensity", &settings.backgroundIntensity, 0.0f, 5.0f);
        ImGui::SliderInt("Max samples (0=inf)", &settings.maxSamples, 0, 20000);
        ImGui::Checkbox("Accumulate frames", &settings.accumulate);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: every pipeline shows its latest raw sample instead of\nthe progressive average - for live per-frame comparisons.");

        ImGui::Separator();
        ImGui::Text("Camera Model");
        int camIdx = static_cast<int>(settings.cameraMode);
        const char* camNames[] = {"Pinhole", "Physical Lens (Steinert 2011)"};
        if (ImGui::Combo("Camera", &camIdx, camNames, 2))
            settings.cameraMode = static_cast<CameraMode>(camIdx);
        if (settings.cameraMode == CameraMode::Lens)
        {
            if (!lensPresetNames.empty())
            {
                int idx = settings.lens.presetIndex;
                const char* current = (idx >= 0 && idx < static_cast<int>(lensPresetNames.size()))
                    ? lensPresetNames[idx].c_str() : "?";
                if (ImGui::BeginCombo("Prescription", current))
                {
                    for (int i = 0; i < static_cast<int>(lensPresetNames.size()); ++i)
                    {
                        if (ImGui::Selectable(lensPresetNames[i].c_str(), i == idx))
                            settings.lens.presetIndex = i;
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::SliderFloat("F-number", &settings.lens.fNumber, 1.0f, 22.0f, "f/%.1f",
                                ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Focus shift (mm)", &settings.lens.focusShiftMm, -5.0f, 20.0f);
            ImGui::SliderFloat("Prescription scale", &settings.lens.scale, 0.1f, 2.0f);
            ImGui::SliderInt("Aperture blades", &settings.lens.apertureBlades, 0, 12);
            ImGui::SliderFloat("Blade rotation", &settings.lens.bladeRotationDeg, 0.0f, 60.0f, "%.1f deg");
            ImGui::SliderFloat("Sensor height (mm)", &settings.lens.sensorHeightMm, 8.0f, 60.0f);
            ImGui::Checkbox("Lens flares (light-traced)", &settings.lens.enableFlare);
            if (settings.lens.enableFlare)
            {
                ImGui::SliderInt("Flare samples", &settings.lens.flareSamples, 1, 64);
                ImGui::SliderFloat("Flare intensity", &settings.lens.flareIntensity, 0.1f, 100.0f, "%.1f",
                                    ImGuiSliderFlags_Logarithmic);
                ImGui::Checkbox("Aperture diffraction streaks", &settings.lens.enableDiffraction);
                if (settings.lens.enableDiffraction)
                {
                    ImGui::SliderFloat("Diffraction intensity", &settings.lens.diffractionIntensity,
                                        0.1f, 100.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
                    ImGui::SliderFloat("Diffraction edge width (mm)", &settings.lens.diffractionEdgeWidthMm,
                                        0.005f, 0.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
                }
            }
        }

        ImGui::Separator();
        ImGui::Checkbox("Fog (homogeneous medium)", &settings.fogEnable);
        if (settings.fogEnable)
        {
            ImGui::SliderFloat("Scattering sigma_s", &settings.fogSigmaS, 0.0f, 1.0f, "%.3f",
                                ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Absorption sigma_a", &settings.fogSigmaA, 0.0f, 1.0f, "%.3f",
                                ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Anisotropy g (HG)", &settings.fogG, -0.9f, 0.9f);
        }

        ImGui::Separator();
        ImGui::Checkbox("ReSTIR BDPT", &settings.enableRestir);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: plain progressive BDPT (the unbiased reference pipeline).");
        if (settings.enableRestir)
        {
            if (settings.cameraMode == CameraMode::Lens)
                ImGui::TextDisabled("Pinhole only - lens mode falls back to plain BDPT.");
            ImGui::Checkbox("Light tracing (t=1)", &settings.restirLightTracing);
            ImGui::Checkbox("Vertex connections (s>=2)", &settings.restirConnections);
            ImGui::Checkbox("Recompute shift MIS (unbiased)", &settings.restirRecomputeMis);
            if (settings.restirLightTracing || settings.restirConnections)
            {
                ImGui::SliderInt("Light subpaths (N_L)", &settings.restirLightPaths, 4096, 262144,
                                 "%d", ImGuiSliderFlags_Logarithmic);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Light subpaths traced per frame (paper Alg. 1). Enters the\n"
                                      "t<=1 MIS weights as 1/N_L; sizes the LVC/LRM buffers\n"
                                      "(reallocated live). Lower = cheaper light pass, sparser\n"
                                      "t=1 coverage per pixel.");
            }
            if (settings.fogEnable)
            {
                int volIdx = settings.restirVolumeMode;
                const char* volNames[] = {"Naive port (volume paths unshiftable)",
                                          "Replay (masks + distance replay)",
                                          "Volume-anchored (ours)"};
                if (ImGui::Combo("Volumetric shifts", &volIdx, volNames, 3))
                    settings.restirVolumeMode = volIdx;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("How shift mappings treat paths with volume scattering\n"
                                      "vertices (all modes unbiased, they differ in reuse):\n"
                                      "naive = every volume-bearing shift fails (ReSTIR PT port),\n"
                                      "replay = free-flight replay + classification masks,\n"
                                      "anchored = + volume vertices as reconnection anchors.");
            }
            ImGui::Checkbox("Temporal reuse", &settings.restirTemporal);
            if (settings.restirTemporal)
            {
                ImGui::Checkbox("Decorrelate (duplication map)", &settings.restirDecorrelate);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Adaptively lowers the temporal confidence cap where many\n"
                                      "pixels share one sample (ReSTIR PT Enhanced). Kills firefly\n"
                                      "blobs/streaks at the cost of a small bias; off = unbiased.");
                if (settings.restirLightTracing)
                {
                    ImGui::Checkbox("Caustic temporal reuse", &settings.restirCausticReuse);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Replay + re-bin the caustic/airlight reservoirs across\n"
                                          "frames (Phase 3). Off = per-frame canonical RIS only -\n"
                                          "the isolation switch for temporal transients.");
                }
                if (settings.fogEnable)
                {
                    ImGui::Checkbox("Fog-parallax history pairing", &settings.restirFogPairing);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("When the surface-anchor validation fails (disocclusion\n"
                                          "under camera motion), re-pair history by reprojecting the\n"
                                          "mean in-scatter depth and reuse the airlight family only.\n"
                                          "Keeps fog history alive across silhouettes; off = ablation.");
                }
            }
            ImGui::Checkbox("Spatial reuse", &settings.restirSpatial);
            if (settings.restirSpatial)
            {
                ImGui::SliderInt("Spatial candidates", &settings.restirSpatialNeighbors, 1, 24);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Reuse candidates per pixel per frame. SPMIS modes batch 4\n"
                                      "per shift-pipeline sweep, so 16-24 is affordable (the\n"
                                      "temporal-free wide-reuse regime); mode 0 costs one sweep\n"
                                      "per candidate.");
                ImGui::Checkbox("Stratified candidate picks", &settings.restirStratified);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Histogram-stratified antithetic selection (Salaün 2025) in\n"
                                      "mode 0, antithetic CDF positions in mode 2. Off = uniform\n"
                                      "random picks (A/B isolation; both unbiased).");
                const char* spatialModes[] = {"Pair mixture (stratified ranks)",
                                              "SPMIS (Hedstrom 2026)",
                                              "History-guided SPMIS (ablation)"};
                ImGui::Combo("Selection & MIS", &settings.restirSpatialMode, spatialModes, 3);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Spatial candidate selection & MIS family (all unbiased):\n"
                                      "pair mixture = one candidate/round vs frozen canonical;\n"
                                      "SPMIS = with-replacement draws prop. to c*pHat*W from the\n"
                                      "cluster-run CDF, one chain w/ stochastic pairwise MIS;\n"
                                      "history-guided = SPMIS weights attenuated by a learned\n"
                                      "shift-survival ratio (ablation arm; a wash on these scenes).");
                ImGui::Checkbox("Normal-octant cluster keys", &settings.restirClusterNormal);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Split reuse clusters by the world-normal octant in addition\n"
                                      "to instance+material (paper-faithful cells; small consistent\n"
                                      "win on curved geometry).");
                if (settings.restirSpatialMode >= 1)
                {
                    ImGui::Checkbox("Neighborhood search", &settings.restirCellSearch);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Probe foreign cluster runs when the own run cannot serve\n"
                                          "(Hedstrom Sec. 5.1 machinery; the gate below decides WHEN).");
                }
                if (settings.restirCellSearch && settings.restirSpatialMode >= 1)
                {
                    const char* gateNames[] = {"Unconditional (paper, taxes motion)",
                                               "Tiny-run gate (default)",
                                               "Tiny-run + disocclusion flag",
                                               "Contribution-aware (BIASED ablation)"};
                    ImGui::Combo("Search gate", &settings.restirSearchGate, gateNames, 4);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("When the search may replace the local run. Unconditional\n"
                                          "probing measures 23-45% noise tax under motion; the\n"
                                          "tiny-run gate (count <= 2) equals no-search everywhere\n"
                                          "measured while keeping rescue. Gate 3 is a deliberately\n"
                                          "kept NEGATIVE RESULT: it brightens sparse scenes +16%.");
                    ImGui::SliderInt("Search probes", &settings.restirSearchIters, 1, 16);
                    ImGui::SliderInt("Search radius (px)", &settings.restirSearchRadius, 4, 128);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Initial probe radius; grows x1.25 per probe. Also the\n"
                                          "probe schedule of probe-guided selection below.");
                }
                if (settings.restirSpatialMode == 1)
                {
                    ImGui::Checkbox("Probe-guided selection (PSEL)", &settings.restirProbeSelection);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Research: per-block probe shifts measure the ideal\n"
                                          "selection weight per candidate run; draws span the union\n"
                                          "of own + probed runs with exact compensated P. Unbiased;\n"
                                          "currently SLOWER than plain SPMIS (the chain functional\n"
                                          "dilutes heterogeneous pools - see the research log).");
                }
                if (settings.restirSpatialMode >= 2 || settings.restirProbeSelection)
                {
                    ImGui::SliderFloat("Defensive mix", &settings.restirScoreDefMix, 0.0f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Share of plain luminance selection mixed into guided\n"
                                          "draws (bounds worst-case weight inflation at 1/mix;\n"
                                          "keeps P > 0 on the contribution support).");
                }
                if (settings.restirSpatialMode >= 2)
                {
                    ImGui::SliderFloat("Survival-score EMA", &settings.restirScoreEmaRate, 0.01f, 1.0f);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Learning rate of the per-pixel shift-survival score\n"
                                          "(mode 2's history-guided attenuation).");
                }
            }
            if (settings.restirTemporal || settings.restirSpatial)
            {
                ImGui::SliderFloat("Confidence cap (M)", &settings.restirConfidenceCap, 1.0f, 64.0f,
                                   "%.0f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Max effective sample count a reservoir can claim (paper: 20).\n"
                                      "Lower = outliers/stale history wash out faster; higher = smoother\n"
                                      "but temporal correlation lingers ~cap frames.");
            }
            const char* dbgNames[] = {"Off", "G-buffer normals", "G-buffer depth", "Motion vectors",
                                      "Reservoir W", "Confidence", "Technique (s,t)",
                                      "Caustic W", "LRM entries", "Caustic confidence"};
            ImGui::Combo("ReSTIR debug view", &settings.restirDebugView, dbgNames, 10);
        }

        result.settingsChanged = (settings != before);

        ImGui::Separator();

        // Denoise
        ImGui::BeginDisabled(!oidnAvailable);
        if (ImGui::Button("Denoise"))
        {
            result.denoiseRequested = true;
        }
        ImGui::EndDisabled();
        if (!oidnAvailable)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("(OIDN not available)");
        }

        ImGui::Separator();

        // Export
        static char exportPathBuf[260] = "render.png";
        ImGui::InputText("File", exportPathBuf, sizeof(exportPathBuf));
        if (ImGui::Button("Export PNG"))
        {
            result.exportRequested = true;
            result.exportPath = exportPathBuf;
        }

        ImGui::End();

        std::vector<Material> materialsBefore = scene.materials;

        ImGui::Begin("Material Editor");
        if (scene.materials.empty())
        {
            ImGui::TextDisabled("No materials in the current scene.");
        }
        for (size_t i = 0; i < scene.materials.size(); ++i)
        {
            Material& mat = scene.materials[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::CollapsingHeader(("Material " + std::to_string(i)).c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::ColorEdit3("Base color", &mat.baseColor.x);
                ImGui::ColorEdit3("Emissive", &mat.emissive.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
                int typeIdx = static_cast<int>(mat.type);
                const char* typeNames[] = {"Diffuse", "Glass", "Conductor", "Rough dielectric", "Layered"};
                if (ImGui::Combo("Type", &typeIdx, typeNames, 5))
                    mat.type = static_cast<MaterialType>(typeIdx);
                switch (mat.type)
                {
                case MaterialType::Glass:
                    ImGui::SliderFloat("IOR", &mat.ior, 1.01f, 2.5f);
                    break;
                case MaterialType::Conductor:
                    ImGui::SliderFloat("Roughness", &mat.roughness, 0.0f, 1.0f);
                    break;
                case MaterialType::RoughDielectric:
                    ImGui::SliderFloat("Roughness", &mat.roughness, 0.0f, 1.0f);
                    ImGui::SliderFloat("IOR", &mat.ior, 1.01f, 2.5f);
                    break;
                case MaterialType::Layered:
                    ImGui::SliderFloat("Metallic", &mat.metallic, 0.0f, 1.0f);
                    ImGui::SliderFloat("Base roughness", &mat.roughness, 0.0f, 1.0f);
                    ImGui::SeparatorText("Coat");
                    ImGui::SliderFloat("Coat roughness", &mat.coatRoughness, 0.0f, 1.0f);
                    ImGui::SliderFloat("Coat IOR", &mat.coatIor, 1.01f, 2.5f);
                    ImGui::SliderFloat("Optical depth", &mat.coatDepth, 0.0f, 8.0f);
                    ImGui::SliderFloat("HG g", &mat.coatG, -0.95f, 0.95f);
                    ImGui::ColorEdit3("Medium albedo", &mat.coatAlbedo.x);
                    break;
                default:
                    ImGui::SliderFloat("Metallic", &mat.metallic, 0.0f, 1.0f);
                    ImGui::SliderFloat("Roughness", &mat.roughness, 0.0f, 1.0f);
                    break;
                }
            }
            ImGui::PopID();
        }
        ImGui::End();

        result.materialsChanged = (scene.materials != materialsBefore);

        // ----------------------------------------------- instances -------
        // Transform editor per scene instance. Edits only mutate the
        // instance's TRS here; the world triangles + BVH are rebuilt
        // asynchronously by Application/BVHBuilder (the image lags a frame
        // or two behind the sliders while the CPU rebuild runs).
        ImGui::Begin("Instances");
        if (scene.instances.empty())
        {
            ImGui::TextDisabled("No instances in the current scene.");
        }
        static int selectedInstance = 0;
        if (selectedInstance >= static_cast<int>(scene.instances.size()))
            selectedInstance = 0;
        for (size_t i = 0; i < scene.instances.size(); ++i)
        {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(scene.instances[i].name.c_str(), selectedInstance == static_cast<int>(i)))
                selectedInstance = static_cast<int>(i);
            ImGui::PopID();
        }
        if (!scene.instances.empty())
        {
            SceneInstance& inst = scene.instances[selectedInstance];
            ImGui::Separator();
            ImGui::Text("%s (%u triangles)", inst.name.c_str(), inst.triangleCount);
            bool moved = false;
            moved |= ImGui::DragFloat3("Position", &inst.position.x, 0.01f);
            moved |= ImGui::DragFloat3("Rotation (deg)", &inst.rotationDeg.x, 0.5f);
            moved |= ImGui::DragFloat("Scale", &inst.scale, 0.01f, 0.01f, 100.0f);
            if (ImGui::Button("Reset transform") && !inst.IsIdentity())
            {
                inst.position = glm::vec3(0.0f);
                inst.rotationDeg = glm::vec3(0.0f);
                inst.scale = 1.0f;
                moved = true;
            }
            if (moved)
                result.instanceMoved = selectedInstance;
        }
        ImGui::End();

        return result;
    }
}
