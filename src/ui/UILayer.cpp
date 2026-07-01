#include "ui/UILayer.h"
#include "optics/LensPrescription.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <algorithm>
#include <cstring>
#include <cfloat>
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

    UIFrameResult UILayer::Draw(RenderSettings& settings, CameraSettings& cameraSettings, LensSystem& lensSystem,
                                 Scene& scene, uint32_t sampleCount, float frameTimeMs, bool oidnAvailable)
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

        // Camera model / physical lens system
        ImGui::Text("Camera Model");
        CameraSettings beforeCameraSettings = cameraSettings;
        LensSystem beforeLensSystem = lensSystem;

        int modeIdx = static_cast<int>(cameraSettings.mode);
        const char* modeNames[] = {"Pinhole", "Physical Lens"};
        if (ImGui::Combo("Mode", &modeIdx, modeNames, 2))
            cameraSettings.mode = static_cast<CameraMode>(modeIdx);

        if (cameraSettings.mode == CameraMode::LensSystem)
        {
            static std::vector<std::string> presetNames = LensPrescription::BuiltinPresetNames();
            if (ImGui::BeginCombo("Preset", cameraSettings.activeLensPreset.c_str()))
            {
                for (const std::string& p : presetNames)
                {
                    bool selected = (p == cameraSettings.activeLensPreset);
                    if (ImGui::Selectable(p.c_str(), selected))
                    {
                        cameraSettings.activeLensPreset = p;
                        result.lensPresetLoadRequested = true;
                        result.lensPresetToLoad = p;
                    }
                }
                ImGui::EndCombo();
            }

            float fStop = static_cast<float>(lensSystem.fStop);
            if (ImGui::SliderFloat("F-stop", &fStop, 0.9f, 32.0f, "f/%.1f", ImGuiSliderFlags_Logarithmic))
                lensSystem.fStop = fStop;
            ImGui::SliderInt("Aperture blades", &lensSystem.apertureBlades, 3, 12);
            float bladeRot = static_cast<float>(lensSystem.apertureBladeRotationDeg);
            if (ImGui::SliderFloat("Blade rotation", &bladeRot, 0.0f, 60.0f, "%.1f deg"))
                lensSystem.apertureBladeRotationDeg = bladeRot;
            float focusDist = static_cast<float>(lensSystem.focusDistanceMm);
            if (ImGui::SliderFloat("Focus distance (mm)", &focusDist, 200.0f, 100000.0f, "%.0f",
                                    ImGuiSliderFlags_Logarithmic))
                lensSystem.focusDistanceMm = focusDist;
            float sensorSize[2] = {static_cast<float>(lensSystem.sensorWidthMm), static_cast<float>(lensSystem.sensorHeightMm)};
            if (ImGui::InputFloat2("Sensor size (mm)", sensorSize))
            {
                lensSystem.sensorWidthMm = sensorSize[0];
                lensSystem.sensorHeightMm = sensorSize[1];
            }
            float scale = static_cast<float>(lensSystem.scale);
            if (ImGui::SliderFloat("Scale factor", &scale, 0.1f, 10.0f))
                lensSystem.scale = scale;

            if (ImGui::TreeNode("Surfaces"))
            {
                if (ImGui::BeginTable("LensSurfaces", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Radius (mm)");
                    ImGui::TableSetupColumn("Thickness (mm)");
                    ImGui::TableSetupColumn("Material");
                    ImGui::TableSetupColumn("Semi-diam (mm)");
                    ImGui::TableSetupColumn("Aperture?");
                    ImGui::TableHeadersRow();

                    for (size_t i = 0; i < lensSystem.surfaces.size(); ++i)
                    {
                        LensSurface& s = lensSystem.surfaces[i];
                        ImGui::TableNextRow();
                        ImGui::PushID(static_cast<int>(i));

                        float radius = static_cast<float>(s.radiusMm);
                        ImGui::TableSetColumnIndex(0);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::InputFloat("##radius", &radius, 0.0f, 0.0f, "%.3f")) s.radiusMm = radius;

                        float thickness = static_cast<float>(s.thicknessMm);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::InputFloat("##thickness", &thickness, 0.0f, 0.0f, "%.3f")) s.thicknessMm = thickness;

                        char materialBuf[32];
                        std::strncpy(materialBuf, s.material.c_str(), sizeof(materialBuf) - 1);
                        materialBuf[sizeof(materialBuf) - 1] = '\0';
                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::InputText("##material", materialBuf, sizeof(materialBuf))) s.material = materialBuf;

                        float semiDiam = static_cast<float>(s.semiDiameterMm);
                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::InputFloat("##semidiam", &semiDiam, 0.0f, 0.0f, "%.2f")) s.semiDiameterMm = semiDiam;

                        ImGui::TableSetColumnIndex(4);
                        ImGui::Checkbox("##isAperture", &s.isAperture);

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if (ImGui::Button("Add surface"))
                    lensSystem.surfaces.push_back(LensSurface{});
                ImGui::TreePop();
            }
        }
        result.lensChanged = (cameraSettings != beforeCameraSettings) || (lensSystem != beforeLensSystem);

        ImGui::Separator();

        // Render settings
        RenderSettings before = settings;
        ImGui::SliderInt("Max bounces", &settings.maxBounces, 1, 32);
        ImGui::SliderFloat("Exposure", &settings.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
        ImGui::ColorEdit3("Background", &settings.backgroundColor.x);
        ImGui::SliderFloat("Background intensity", &settings.backgroundIntensity, 0.0f, 5.0f);
        ImGui::SliderInt("Max samples (0=inf)", &settings.maxSamples, 0, 20000);

        ImGui::Separator();
        ImGui::Text("Lens Flare / Diffraction");
        ImGui::Checkbox("Enable flare/ghosts", &settings.enableFlare);
        ImGui::SliderInt("Flare samples/frame", &settings.flareSamplesPerFrame, 1024, 262144);
        ImGui::SliderFloat("Flare intensity", &settings.flareIntensity, 0.0f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Enable aperture diffraction", &settings.enableDiffraction);
        ImGui::SliderFloat("Diffraction edge epsilon (mm)", &settings.diffractionEdgeEpsilonMM, 0.005f, 0.5f, "%.3f",
                            ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Diffraction branch prob.", &settings.diffractionBranchProbability, 0.05f, 0.95f);
        ImGui::SliderFloat("Diffraction intensity", &settings.diffractionIntensity, 0.0f, 10.0f);

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
                ImGui::SliderFloat("Metallic", &mat.metallic, 0.0f, 1.0f);
                ImGui::SliderFloat("Roughness", &mat.roughness, 0.0f, 1.0f);
            }
            ImGui::PopID();
        }
        ImGui::End();

        result.materialsChanged = (scene.materials != materialsBefore);

        return result;
    }
}
