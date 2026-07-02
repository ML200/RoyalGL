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
                                 bool oidnAvailable, const std::vector<std::string>& lensPresetNames)
    {
        UIFrameResult result;

        ImGui::Begin("RoyalGL");

        // Stats
        ImGui::Text("Samples: %u", sampleCount);
        ImGui::Text("Frame time: %.2f ms (%.1f FPS)", frameTimeMs, 1000.0f / std::max(frameTimeMs, 0.001f));
        ImGui::Text("Triangles: %zu", scene.triangles.size());

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
                if (!settings.enableBidir)
                    ImGui::TextDisabled("Flares need the bidirectional pipeline.");
            }
        }

        ImGui::Separator();
        ImGui::Checkbox("Bidirectional (BDPT)", &settings.enableBidir);
        ImGui::BeginDisabled(settings.enableBidir);
        ImGui::Checkbox("NEE (light tree) + MIS", &settings.enableNEE);
        ImGui::EndDisabled();
        if (settings.enableBidir && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Unidirectional-only: BDPT has its own light sampling strategy.");

        ImGui::Checkbox("ReSTIR BDPT (WIP)", &settings.enableRestir);
        if (settings.enableRestir)
        {
            if (settings.cameraMode == CameraMode::Lens)
                ImGui::TextDisabled("Pinhole only - lens mode falls back to plain BDPT.");
            ImGui::Checkbox("Temporal reuse", &settings.restirTemporal);
            ImGui::Checkbox("Spatial reuse", &settings.restirSpatial);
            ImGui::Checkbox("Accumulate frames", &settings.restirAccumulate);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Off: show the raw per-frame estimate (1spp + reuse)\ninstead of the progressive average.");
            if (settings.restirSpatial)
            {
                ImGui::SliderInt("Spatial neighbors", &settings.restirSpatialNeighbors, 1, 8);
                ImGui::SliderFloat("Spatial radius (px)", &settings.restirSpatialRadius, 4.0f, 100.0f);
            }
            const char* dbgNames[] = {"Off", "G-buffer normals", "G-buffer depth", "Motion vectors",
                                      "Reservoir W", "Confidence", "Technique (s,t)"};
            ImGui::Combo("ReSTIR debug view", &settings.restirDebugView, dbgNames, 7);
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
                const char* typeNames[] = {"Diffuse", "Glass"};
                if (ImGui::Combo("Type", &typeIdx, typeNames, 2))
                    mat.type = static_cast<MaterialType>(typeIdx);
                if (mat.type == MaterialType::Glass)
                {
                    ImGui::SliderFloat("IOR", &mat.ior, 1.01f, 2.5f);
                }
                else
                {
                    ImGui::SliderFloat("Metallic", &mat.metallic, 0.0f, 1.0f);
                    ImGui::SliderFloat("Roughness", &mat.roughness, 0.0f, 1.0f);
                }
            }
            ImGui::PopID();
        }
        ImGui::End();

        result.materialsChanged = (scene.materials != materialsBefore);

        return result;
    }
}
