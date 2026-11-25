#include "editor_scene.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <glm/matrix.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/common/lighting_controls.h"
#include "editor_theme.h"
#include "runtime/model/prepared_model.h"

namespace Incubator::Editor
{
    void EditorScene::properties(Editor::EditorApp& app)
    {
        controller_.syncPanelTransactionState();
        if (!ImGui::Begin("Properties"))
        {
            ImGui::End();
            controller_.commitPanelTransactions(app);
            return;
        }
        const auto& raw = app.window().input();
        const bool windowFocused = raw.focused() && !raw.focusLost();
        bool entityWidgetActive = false;
        auto& operations = controller_.operations();
        const auto beginEntityTransaction = [&]() {
            controller_.beginPanelTransaction(app, PanelTransactionKind::Entity);
        };
        const auto finishEntityTransaction = [&]() {
            if (controller_.activePanelTransaction() == PanelTransactionKind::Entity)
                controller_.commitPanelTransactions(app);
        };
        const auto& style = ImGui::GetStyle();
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float fontSize = ImGui::GetFontSize();
        const float labelWidth = std::clamp(availWidth * 0.34f, fontSize * 5.5f, fontSize * 8.0f);
        const float controlWidth = std::max(60.0f, availWidth - labelWidth);
        const float resetBtnWidth = ImGui::CalcTextSize("R").x + style.FramePadding.x * 2.0f;
        const float transformWidth = std::max(60.0f, controlWidth - resetBtnWidth - style.ItemSpacing.x);
        const float halfWidth = std::max(60.0f, (availWidth - style.ItemSpacing.x) * 0.5f);
        const auto rowLabel = [&](const char* label) {
            const float rowStartX = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(rowStartX + labelWidth);
        };
        const auto rowAxisLabel = [&](const char* label) {
            const float rowStartX = ImGui::GetCursorPosX();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float barX = itemMax.x + 5.0f;
            const float barH = std::max(2.0f, (itemMax.y - itemMin.y - 4.0f) / 3.0f);
            auto* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled({barX, itemMin.y + 1.0f}, {barX + 3.0f, itemMin.y + 1.0f + barH},
                                    ImGui::GetColorU32(EditorTheme::axisXColor()));
            drawList->AddRectFilled({barX, itemMin.y + 2.0f + barH}, {barX + 3.0f, itemMin.y + 2.0f + barH * 2.0f},
                                    ImGui::GetColorU32(EditorTheme::axisYColor()));
            drawList->AddRectFilled({barX, itemMin.y + 3.0f + barH * 2.0f},
                                    {barX + 3.0f, itemMin.y + 3.0f + barH * 3.0f},
                                    ImGui::GetColorU32(EditorTheme::axisZColor()));
            ImGui::SameLine(rowStartX + labelWidth);
        };

        if (ImGui::CollapsingHeader("Selected object", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (const auto* entity = app.scene().find(controller_.selected()))
            {
                rowLabel("ID");
                ImGui::Text("#%llu", static_cast<unsigned long long>(entity->id.value()));
                rowLabel("Name");
                const auto nameId = std::to_string(entity->id.value());
                ImGui::PushID(nameId.c_str());
                auto name = EditorController::pathBuffer(entity->name);
                const auto resize = [](ImGuiInputTextCallbackData* data) {
                    auto* buffer = static_cast<std::vector<char>*>(data->UserData);
                    buffer->resize(static_cast<size_t>(data->BufSize));
                    data->Buf = buffer->data();
                    return 0;
                };
                ImGui::SetNextItemWidth(controlWidth);
                const bool nameChanged = ImGui::InputText("##entity-name", name.data(), name.size(),
                                                          ImGuiInputTextFlags_CallbackResize, resize, &name);
                if (ImGui::IsItemActivated() && windowFocused)
                    beginEntityTransaction();
                if (nameChanged)
                    operations.rename(app.scene(), app.clearColor(), name.data());
                entityWidgetActive |= ImGui::IsItemActive();
                if (ImGui::IsItemDeactivated())
                    finishEntityTransaction();
                ImGui::PopID();
                auto transform = entity->transform;
                auto position = transform.position();
                auto scale = transform.scale();
                auto degrees = glm::degrees(glm::eulerAngles(transform.rotation()));
                bool visible = entity->visible;
                rowLabel("Visible");
                if (ImGui::Checkbox("##visible", &visible))
                    operations.setVisible(app.scene(), app.clearColor(), visible);
                if (entity->model)
                {
                    int shadingIndex = 1;
                    switch (entity->model->source().options().shading)
                    {
                        case Render::ShadingMode::Unlit:
                            shadingIndex = 0;
                            break;
                        case Render::ShadingMode::Lambert:
                            shadingIndex = 1;
                            break;
                        case Render::ShadingMode::Pbr:
                            shadingIndex = 2;
                            break;
                    }
                    rowLabel("Shading");
                    ImGui::SetNextItemWidth(controlWidth);
                    if (ImGui::Combo("##entity-shading", &shadingIndex, "Unlit\0Lambert\0PBR\0"))
                    {
                        const Render::ShadingMode targetShading =
                            shadingIndex == 0 ? Render::ShadingMode::Unlit
                                              : (shadingIndex == 2 ? Render::ShadingMode::Pbr
                                                                   : Render::ShadingMode::Lambert);
                        controller_.setEntityShading(app, targetShading);
                    }
                }
                const auto applyTransformWidget = [&](bool edited) {
                    const bool activateStart = ImGui::IsItemActivated();
                    const bool entityActive = controller_.activePanelTransaction() == PanelTransactionKind::Entity;
                    if ((activateStart || (edited && ImGui::IsItemActive() && !entityActive)) && windowFocused)
                        beginEntityTransaction();
                    if (edited)
                        operations.setTransform(app.scene(), app.clearColor(), transform);
                    entityWidgetActive |= ImGui::IsItemActive();
                    if (ImGui::IsItemDeactivated())
                        finishEntityTransaction();
                };
                rowAxisLabel("Position");
                ImGui::SetNextItemWidth(transformWidth);
                bool edited = ImGui::DragFloat3("##position", &position.x, 0.05f);
                if (edited)
                    transform.setPosition(position);
                applyTransformWidget(edited);
                ImGui::SameLine();
                if (ImGui::Button("R##reset-pos"))
                {
                    auto resetTransform = entity->transform;
                    resetTransform.setPosition(glm::vec3(0.0f));
                    operations.setTransform(app.scene(), app.clearColor(), resetTransform);
                }
                rowAxisLabel("Rotation");
                ImGui::SetNextItemWidth(transformWidth);
                edited = ImGui::SliderFloat3("##rotation", &degrees.x, -180, 180, "%.0f", ImGuiSliderFlags_AlwaysClamp);
                if (edited)
                    transform.setRotation(glm::quat(glm::radians(degrees)));
                applyTransformWidget(edited);
                ImGui::SameLine();
                if (ImGui::Button("R##reset-rot"))
                {
                    auto resetTransform = entity->transform;
                    resetTransform.setRotation(glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
                    operations.setTransform(app.scene(), app.clearColor(), resetTransform);
                }
                rowAxisLabel("Scale");
                ImGui::SetNextItemWidth(transformWidth);
                edited = ImGui::DragFloat3("##scale", &scale.x, 0.05f);
                if (edited)
                    transform.setScale(scale);
                applyTransformWidget(edited);
                ImGui::SameLine();
                if (ImGui::Button("R##reset-scale"))
                {
                    auto resetTransform = entity->transform;
                    resetTransform.setScale(glm::vec3(1.0f));
                    operations.setTransform(app.scene(), app.clearColor(), resetTransform);
                }
                if (ImGui::Button("Duplicate", {halfWidth, 0.0f}))
                    duplicateSelected(app);
                ImGui::SameLine();
                if (ImGui::Button("Delete selected", {halfWidth, 0.0f}))
                    deleteSelected(app);
            }
            else
                ImGui::TextWrapped("Select an object to edit.");
            ImGui::Separator();
            const float thirdWidth = std::max(40.0f, (availWidth - style.ItemSpacing.x * 2.0f) / 3.0f);
            const auto gizmoModeButton = [&](const char* label, GizmoMode mode) {
                const bool active = gizmoMode() == mode;
                if (active)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, EditorTheme::accentColor());
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorTheme::accentColor());
                }
                if (ImGui::Button(label, {thirdWidth, 0.0f}))
                    setGizmoMode(mode);
                if (active)
                    ImGui::PopStyleColor(2);
            };
            gizmoModeButton("Translate (W)", GizmoMode::Translate);
            ImGui::SameLine();
            gizmoModeButton("Rotate (E)", GizmoMode::Rotate);
            ImGui::SameLine();
            gizmoModeButton("Scale (R)", GizmoMode::Scale);
            if (const auto* selectedEntity = app.scene().find(controller_.selected());
                selectedEntity && selectedEntity->parent.valid())
            {
                if (gizmoMode() != GizmoMode::Translate)
                {
                    ImGui::TextColored(EditorTheme::mutedTextColor(),
                                       "Child Gizmo: Translate only (world axes); use Properties for Rotate/Scale.");
                }
                else
                {
                    const auto parentWorld = app.scene().worldMatrix(selectedEntity->parent);
                    const double det = parentWorld ? glm::determinant(glm::dmat3(*parentWorld)) : 0.0;
                    if (!parentWorld || !std::isfinite(det) || std::abs(det) <= 1e-8)
                    {
                        ImGui::TextColored(EditorTheme::mutedTextColor(),
                                           "Child Gizmo disabled: parent transform is singular.");
                    }
                }
            }
        }
        if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (ImGui::Button("Focus selected", {halfWidth, 0.0f}))
            {
                const auto focusExtent = (controller_.lastExtent().width > 0 && controller_.lastExtent().height > 0)
                                             ? controller_.lastExtent()
                                             : app.sceneExtent();
                focusSelected(app, focusExtent);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset view", {halfWidth, 0.0f}))
                resetCamera(app);
            rowLabel("FXAA");
            ImGui::Checkbox("##fxaa", &app.fxaaEnabled());
            rowLabel("Studio IBL");
            ImGui::Checkbox("##ibl", &app.iblEnabled());
            rowLabel("Grid");
            ImGui::Checkbox("##debug-grid", &debugOptions_.showGrid);
            rowLabel("Selection bounds");
            ImGui::Checkbox("##debug-selection-bounds", &debugOptions_.showSelectionBounds);
            rowLabel("Light gizmos");
            ImGui::Checkbox("##debug-light-gizmos", &debugOptions_.showLightGizmos);
            ImGui::Separator();
            const double cpuFrameMs = app.lastFrameTime().deltaSeconds() * 1000.0;
            const auto stats = app.viewportStats();
            rowLabel("CPU frame");
            ImGui::Text("%.2f ms", cpuFrameMs);
            rowLabel("GPU viewport");
            if (!stats.gpuTimestampAvailable)
                ImGui::TextUnformatted("Unavailable");
            else if (!stats.hasCompletedSample || !stats.gpuTimeMs.has_value())
                ImGui::TextUnformatted("--");
            else
                ImGui::Text("%.2f ms", *stats.gpuTimeMs);
            rowLabel("Scene draws");
            if (!stats.hasCompletedSample)
                ImGui::TextUnformatted("--");
            else
                ImGui::Text("%u", stats.sceneDrawCount);
            rowLabel("Shadow draws");
            if (!stats.hasCompletedSample)
                ImGui::TextUnformatted("--");
            else
                ImGui::Text("%u", stats.shadowDrawCount);
        }
        if (ImGui::CollapsingHeader("Scene settings", ImGuiTreeNodeFlags_DefaultOpen))
        {
            rowLabel("Background");
            auto clearColor = app.clearColor();
            const bool colorChanged = ImGui::ColorEdit3("##background", clearColor.data(), ImGuiColorEditFlags_NoInputs);
            ImGui::PushID("##background");
            const bool pickerOpen = ImGui::IsPopupOpen("picker");
            ImGui::PopID();
            const bool backgroundActive = (ImGui::IsItemActive() || pickerOpen) && windowFocused;
            if (backgroundActive && controller_.activePanelTransaction() != PanelTransactionKind::Background)
                controller_.beginPanelTransaction(app, PanelTransactionKind::Background);
            if (colorChanged)
                operations.setClearColor(app.scene(), app.clearColor(), clearColor);
            if (!backgroundActive && controller_.activePanelTransaction() == PanelTransactionKind::Background)
                controller_.commitPanelTransactions(app);
            auto& shadows = app.shadowSettings();
            rowLabel("Shadows");
            ImGui::Checkbox("##shadows", &shadows.enabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Preview shadow settings (not saved in scene files).");
            rowLabel("Depth bias");
            ImGui::SetNextItemWidth(controlWidth);
            ImGui::SliderFloat("##shadowBias", &shadows.depthBias, 0, 0.01f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
            rowLabel("3-cascade CSM");
            bool csmEnabled = shadows.cascadeCount > 1u;
            if (ImGui::Checkbox("##shadow-csm", &csmEnabled))
                shadows.cascadeCount = csmEnabled ? 3u : 1u;
            rowLabel("Shadow size");
            ImGui::SetNextItemWidth(controlWidth);
            int resIdx = shadows.resolution >= 2048u ? 1 : 0;
            if (ImGui::Combo("##shadow-res", &resIdx, "1024\0 2048\0"))
                shadows.resolution = resIdx == 1 ? 2048u : 1024u;
            rowLabel("Fit scene bounds");
            ImGui::Checkbox("##shadow-fit", &shadows.fitToBounds);
            rowLabel("Normal bias");
            ImGui::SetNextItemWidth(controlWidth);
            ImGui::SliderFloat("##shadow-normal-bias", &shadows.normalBias, 0.0f, 0.1f, "%.4f",
                               ImGuiSliderFlags_AlwaysClamp);
            ImGui::TextDisabled("Shadow preview settings are not saved in scene files.");
            ImGui::Separator();
            auto lighting = app.scene().lighting();
            const auto lightingState = App::lightingWidget(lighting);
            bool localChanged = false;
            bool localActive = false;
            bool discreteLightEdit = false;
            std::optional<size_t> removeLightIdx;

            ImGui::Spacing();
            ImGui::Text("Local Lights (%zu / %zu)", lighting.localLights.size(), Render::MAX_LOCAL_LIGHTS);
            const bool canAddLight = lighting.localLights.size() < Render::MAX_LOCAL_LIGHTS;
            ImGui::BeginDisabled(!canAddLight);
            if (ImGui::Button("Add Point##local-light-add-point", {halfWidth, 0.0f}))
            {
                if (controller_.addLocalLight(app, Render::LocalLightType::Point))
                    lighting = app.scene().lighting();
            }
            ImGui::SameLine();
            if (ImGui::Button("Add Spot##local-light-add-spot", {halfWidth, 0.0f}))
            {
                if (controller_.addLocalLight(app, Render::LocalLightType::Spot))
                    lighting = app.scene().lighting();
            }
            ImGui::EndDisabled();

            for (size_t i = 0; i < lighting.localLights.size(); ++i)
            {
                auto& light = lighting.localLights[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Separator();
                const char* headerLabel = light.type == Render::LocalLightType::Spot ? "Spot" : "Point";
                if (ImGui::Checkbox("##local-enabled", &light.enabled))
                    discreteLightEdit = true;
                ImGui::SameLine();
                ImGui::Text("%s #%zu", headerLabel, i + 1u);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove##local-remove"))
                    removeLightIdx = i;

                rowLabel("Position");
                ImGui::SetNextItemWidth(controlWidth);
                localChanged |= ImGui::SliderFloat3("##local-pos", &light.position.x, -25.0f, 25.0f, "%.2f");
                localActive |= ImGui::IsItemActive() && windowFocused;

                rowLabel("Color");
                localChanged |= ImGui::ColorEdit3("##local-color", &light.color.x,
                                                  ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
                ImGui::PushID("##local-color");
                const bool localPickerOpen = ImGui::IsPopupOpen("picker");
                ImGui::PopID();
                localActive |= (ImGui::IsItemActive() || localPickerOpen) && windowFocused;

                rowLabel("Intensity");
                ImGui::SetNextItemWidth(controlWidth);
                localChanged |=
                    ImGui::SliderFloat("##local-intensity", &light.intensity, 0.0f, 50.0f, "%.2f",
                                       ImGuiSliderFlags_AlwaysClamp);
                localActive |= ImGui::IsItemActive() && windowFocused;

                rowLabel("Range");
                ImGui::SetNextItemWidth(controlWidth);
                localChanged |= ImGui::SliderFloat("##local-range", &light.range, 0.5f, 50.0f, "%.2f",
                                                   ImGuiSliderFlags_AlwaysClamp);
                localActive |= ImGui::IsItemActive() && windowFocused;

                if (light.type == Render::LocalLightType::Spot)
                {
                    rowLabel("Direction");
                    ImGui::SetNextItemWidth(controlWidth);
                    if (ImGui::SliderFloat3("##local-dir", &light.direction.x, -1.0f, 1.0f, "%.2f"))
                    {
                        if (light.direction.x == 0.0f && light.direction.y == 0.0f && light.direction.z == 0.0f)
                            light.direction = {0.0f, -1.0f, 0.0f};
                        localChanged = true;
                    }
                    localActive |= ImGui::IsItemActive() && windowFocused;

                    rowLabel("Inner angle");
                    ImGui::SetNextItemWidth(controlWidth);
                    localChanged |= ImGui::SliderFloat("##local-inner", &light.innerConeAngle, 0.0f,
                                                       light.outerConeAngle, "%.2f rad", ImGuiSliderFlags_AlwaysClamp);
                    localActive |= ImGui::IsItemActive() && windowFocused;

                    rowLabel("Outer angle");
                    ImGui::SetNextItemWidth(controlWidth);
                    localChanged |= ImGui::SliderFloat("##local-outer", &light.outerConeAngle, light.innerConeAngle,
                                                       1.50f, "%.2f rad", ImGuiSliderFlags_AlwaysClamp);
                    localActive |= ImGui::IsItemActive() && windowFocused;
                }
                ImGui::PopID();
            }

            const bool anyLightingActive = (lightingState.active && windowFocused) || localActive;
            if (anyLightingActive && controller_.activePanelTransaction() != PanelTransactionKind::Lighting)
                controller_.beginPanelTransaction(app, PanelTransactionKind::Lighting);
            if (lightingState.changed || localChanged)
                operations.setLighting(app.scene(), app.clearColor(), lighting);
            if (!anyLightingActive && controller_.activePanelTransaction() == PanelTransactionKind::Lighting)
                controller_.commitPanelTransactions(app);
            if (discreteLightEdit)
                controller_.setLighting(app, lighting);
            if (removeLightIdx.has_value())
                controller_.removeLocalLight(app, *removeLightIdx);

            ImGui::Separator();
            auto display = app.scene().display();
            rowLabel("Tone mapping");
            ImGui::SetNextItemWidth(controlWidth);
            int toneIndex = display.toneMapping == Render::ToneMappingMode::Off        ? 0
                            : display.toneMapping == Render::ToneMappingMode::AcesFilmic ? 2
                                                                                         : 1;
            if (ImGui::Combo("##display-tone-mapping", &toneIndex, "Off\0Reinhard\0ACES Filmic\0"))
            {
                display.toneMapping = toneIndex == 0 ? Render::ToneMappingMode::Off
                                      : toneIndex == 2 ? Render::ToneMappingMode::AcesFilmic
                                                       : Render::ToneMappingMode::Reinhard;
                controller_.setDisplay(app, display);
            }
            const bool exposureDisabled = display.toneMapping == Render::ToneMappingMode::Off;
            rowLabel("Exposure (EV)");
            ImGui::SetNextItemWidth(controlWidth);
            if (exposureDisabled)
                ImGui::BeginDisabled(true);
            bool displaySliderChanged = ImGui::SliderFloat(
                "##display-exposure-ev", &display.exposureEv, Render::DisplaySettings::MIN_EXPOSURE_EV,
                Render::DisplaySettings::MAX_EXPOSURE_EV, "%+.2f EV", ImGuiSliderFlags_AlwaysClamp);
            bool displaySliderActive = !exposureDisabled && ImGui::IsItemActive() && windowFocused;
            if (exposureDisabled)
                ImGui::EndDisabled();

            rowLabel("Skybox");
            if (ImGui::Checkbox("##display-skybox", &display.skyboxEnabled))
                controller_.setDisplay(app, display);
            rowLabel("Bloom");
            if (ImGui::Checkbox("##display-bloom", &display.bloomEnabled))
                controller_.setDisplay(app, display);
            const bool bloomDisabled = !display.bloomEnabled;
            if (bloomDisabled)
                ImGui::BeginDisabled(true);
            rowLabel("Bloom threshold");
            ImGui::SetNextItemWidth(controlWidth);
            displaySliderChanged |= ImGui::SliderFloat("##display-bloom-threshold", &display.bloomThreshold, 0.0f, 5.0f,
                                                       "%.2f", ImGuiSliderFlags_AlwaysClamp);
            displaySliderActive |= !bloomDisabled && ImGui::IsItemActive() && windowFocused;
            rowLabel("Bloom intensity");
            ImGui::SetNextItemWidth(controlWidth);
            displaySliderChanged |=
                ImGui::SliderFloat("##display-bloom-intensity", &display.bloomIntensity, 0.0f,
                                   Render::DisplaySettings::MAX_BLOOM_INTENSITY, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            displaySliderActive |= !bloomDisabled && ImGui::IsItemActive() && windowFocused;
            if (bloomDisabled)
                ImGui::EndDisabled();

            if (displaySliderActive && controller_.activePanelTransaction() != PanelTransactionKind::Display)
                controller_.beginPanelTransaction(app, PanelTransactionKind::Display);
            if (displaySliderChanged)
                operations.setDisplay(app.scene(), app.clearColor(), display);
            if (!displaySliderActive && controller_.activePanelTransaction() == PanelTransactionKind::Display)
                controller_.commitPanelTransactions(app);
        }
        else if (controller_.activePanelTransaction() == PanelTransactionKind::Lighting ||
                 controller_.activePanelTransaction() == PanelTransactionKind::Display ||
                 controller_.activePanelTransaction() == PanelTransactionKind::Background)
        {
            controller_.commitPanelTransactions(app);
        }
        if (controller_.activePanelTransaction() == PanelTransactionKind::Entity && !entityWidgetActive)
            finishEntityTransaction();
        ImGui::End();
    }

}  // namespace Incubator::Editor
