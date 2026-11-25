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
    void EditorScene::assets(Editor::EditorApp& app)
    {
        ImGui::SetNextWindowSize({ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 22}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Assets");
        // 可点击路径面包屑导航栏
        std::string breadcrumbTarget;
        bool hasBreadcrumbTarget = false;
        ImGui::PushID("breadcrumb");
        if (ImGui::SmallButton("/"))
        {
            breadcrumbTarget.clear();
            hasBreadcrumbTarget = true;
        }
        if (!controller_.assetDirectory().empty())
        {
            const std::string& dir = controller_.assetDirectory();
            size_t start = 0;
            int segIndex = 0;
            while (start < dir.size())
            {
                const size_t slash = dir.find('/', start);
                const size_t end = slash == std::string::npos ? dir.size() : slash;
                const std::string segment = dir.substr(start, end - start);
                const std::string cumulative = dir.substr(0, end);
                if (!segment.empty())
                {
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::TextDisabled(">");
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::PushID(segIndex++);
                    if (ImGui::SmallButton(segment.c_str()))
                    {
                        breadcrumbTarget = cumulative;
                        hasBreadcrumbTarget = true;
                    }
                    ImGui::PopID();
                }
                if (slash == std::string::npos)
                    break;
                start = slash + 1;
            }
        }
        ImGui::PopID();
        if (hasBreadcrumbTarget)
            browseDirectory(breadcrumbTarget);

        if (!controller_.assetDirectory().empty())
        {
            if (ImGui::Button("Up"))
                parentDirectory();
            ImGui::SameLine();
        }
        bool revealImported = false;
        if (ImGui::Button("Import..."))
        {
            try
            {
                if (const auto path = pickExternalModel_(app))
                    revealImported = controller_.importExternalModel(app, *path).has_value();
            }
            catch (const Error::Exception& error)
            {
                controller_.operations().setStatus(error.message(), true);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Refresh"))
            browseDirectory(controller_.assetDirectory());
        ImGui::Text("Listed: %zu | Cached: %zu", controller_.assetEntries().size(), cachedModels());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Listed: directory entries only. Cached: prepared models.\n"
                "Neither count represents CPU loads or live GPU objects.\n"
                "Submitted frames retain GPU resources until completion.");
        if (ImGui::Button("Clear unused models", {-1, 0}))
            collectUnusedModels();
        if (!controller_.assetError().empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::errorColor());
            ImGui::TextWrapped("%s", controller_.assetError().c_str());
            ImGui::PopStyleColor();
        }
        if (controller_.messageError())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, EditorTheme::errorColor());
            ImGui::TextWrapped("%s", controller_.message().c_str());
            ImGui::PopStyleColor();
        }
        std::string enter;
        const ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                           ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("##assets", 2, tableFlags))
        {
            ImGui::TableSetupColumn("Name / type", ImGuiTableColumnFlags_WidthStretch);
            const auto& style = ImGui::GetStyle();
            const auto actionsWidth = ImGui::CalcTextSize("Add").x + ImGui::CalcTextSize("Reload").x +
                                      4 * style.FramePadding.x + style.ItemSpacing.x;
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, actionsWidth);
            ImGui::TableHeadersRow();
            for (const auto& entry : controller_.assetEntries())
            {
                ImGui::PushID(entry.path.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool focused = entry.path == controller_.focusedAssetPath();
                if (focused)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header));
                const char* kind = "File";
                const char* badge = "[FILE]";
                ImVec4 badgeColor = EditorTheme::mutedTextColor();
                if (entry.kind == Resource::AssetKind::Directory)
                {
                    kind = "Directory";
                    badge = "[DIR]";
                    badgeColor = EditorTheme::badgeDirColor();
                }
                else if (entry.kind == Resource::AssetKind::Model)
                {
                    kind = "Model candidate";
                    badge = "[MODEL]";
                    badgeColor = EditorTheme::badgeModelColor();
                }
                else if (entry.kind == Resource::AssetKind::Texture)
                {
                    kind = "Texture";
                    badge = "[TEX]";
                    badgeColor = EditorTheme::badgeTextureColor();
                }
                else if (entry.kind == Resource::AssetKind::Dependency)
                {
                    kind = "Dependency";
                    badge = "[DEP]";
                }
                ImGui::PushStyleColor(ImGuiCol_Text, badgeColor);
                ImGui::TextUnformatted(badge);
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0f, 6.0f);
                ImGui::TextUnformatted(entry.name.c_str());
                if (focused && revealImported)
                    ImGui::SetScrollHereY();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s", entry.path.c_str(), kind);
                ImGui::TableNextColumn();
                if (entry.kind == Resource::AssetKind::Directory && ImGui::Button("Enter"))
                    enter = entry.path;
                if (entry.kind == Resource::AssetKind::Model)
                {
                    if (ImGui::Button("Add"))
                        addModel(app, entry.path);
                    ImGui::SameLine();
                    if (ImGui::Button("Reload"))
                        controller_.reloadAsset(app, entry.path);
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!enter.empty())
            browseDirectory(enter);
        ImGui::End();
    }

    void EditorScene::objects(Editor::EditorApp& app)
    {
        ImGui::Begin("Objects");
        ImGui::TextUnformatted("New model");
        ImGui::SetNextItemWidth(-1);
        int modelIndex = controller_.useObjModel() ? 1 : 0;
        if (ImGui::Combo("##model", &modelIndex, "Avocado\0Teapot\0"))
            controller_.selectModel(modelIndex == 1);
        if (ImGui::Button("Create object", {-1, 0}))
            createEntity(app);
        if (ImGui::Button("More..."))
            ImGui::OpenPopup("Object actions");
        if (ImGui::BeginPopup("Object actions"))
        {
            const auto selectedId = controller_.selected();
            const auto* selectedEntity = app.scene().find(selectedId);
            const auto isDescendantOrSelf = [&](Scene::EntityId candidateId) {
                if (!selectedEntity || !candidateId.valid())
                    return true;
                Scene::EntityId cursor = candidateId;
                const auto total = app.scene().entities().size();
                for (size_t steps = 0; cursor.valid() && steps <= total; ++steps)
                {
                    if (cursor == selectedId)
                        return true;
                    const auto* node = app.scene().find(cursor);
                    cursor = node ? node->parent : Scene::EntityId{};
                }
                return false;
            };
            if (ImGui::BeginMenu("Parent selected to... (keep local)", selectedEntity != nullptr))
            {
                bool anyTarget = false;
                for (const auto& candidate : app.scene().entities())
                {
                    if (isDescendantOrSelf(candidate.id))
                        continue;
                    anyTarget = true;
                    const std::string label =
                        (candidate.name.empty() ? std::string("未命名") : candidate.name) + " (#" +
                        std::to_string(candidate.id.value()) + ")";
                    ImGui::PushID(std::to_string(candidate.id.value()).c_str());
                    if (ImGui::MenuItem(label.c_str(), nullptr, selectedEntity->parent == candidate.id))
                        controller_.setParent(app, selectedId, candidate.id);
                    ImGui::PopID();
                }
                if (!anyTarget)
                    ImGui::MenuItem("(No valid parent)", nullptr, false, false);
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Unparent selected (keep local)", nullptr, false,
                                selectedEntity != nullptr && selectedEntity->parent.valid()))
                controller_.setParent(app, selectedId, {});
            ImGui::Separator();
            if (ImGui::MenuItem("Clear scene", nullptr, false, !app.scene().entities().empty()))
                requestClear(app);
            ImGui::EndPopup();
        }
        ImGui::Separator();
        const auto entities = app.scene().entities();
        ImGui::Text("Objects: %zu", entities.size());
        if (ImGui::BeginListBox("##entities", {-1, -1}))
        {
            std::unordered_map<uint64_t, std::vector<size_t>> childrenByParent;
            std::vector<size_t> rootIndices;
            rootIndices.reserve(entities.size());
            for (size_t i = 0; i < entities.size(); ++i)
            {
                const auto parent = entities[i].parent;
                if (parent.valid() && app.scene().find(parent) != nullptr)
                    childrenByParent[parent.value()].push_back(i);
                else
                    rootIndices.push_back(i);
            }

            auto* drawList = ImGui::GetWindowDrawList();
            const auto renderNode = [&](auto& self, size_t index, int depth) -> void {
                const auto& entity = entities[index];
                const auto id = std::to_string(entity.id.value());
                const std::string displayName = entity.name.empty() ? std::string("未命名") : entity.name;
                const std::string idSuffix = " (#" + id + ")";
                const auto childIt = childrenByParent.find(entity.id.value());
                const bool hasKids = childIt != childrenByParent.end() && !childIt->second.empty();
                const bool collapsed = collapsedEntities_.contains(entity.id.value());

                ImGui::PushID(id.c_str());
                bool visible = entity.visible;
                if (ImGui::Checkbox("##vis", &visible))
                    controller_.setVisible(app, entity.id, visible);
                ImGui::SameLine();
                if (depth > 0)
                {
                    ImGui::Dummy({static_cast<float>(depth) * 14.0f, 0.0f});
                    ImGui::SameLine(0.0f, 4.0f);
                }
                if (hasKids)
                {
                    if (ImGui::SmallButton(collapsed ? ">##expand" : "v##expand"))
                    {
                        if (collapsed)
                            collapsedEntities_.erase(entity.id.value());
                        else
                            collapsedEntities_.insert(entity.id.value());
                    }
                    ImGui::SameLine(0.0f, 4.0f);
                }
                if (ImGui::Selectable("##entity", controller_.selected() == entity.id))
                    select(app, entity.id);
                const auto pos = ImGui::GetItemRectMin();
                const float textY = pos.y + (ImGui::GetItemRectSize().y - ImGui::GetFontSize()) * 0.5f;
                const ImU32 nameColor = entity.visible
                                            ? ImGui::GetColorU32(ImGuiCol_Text)
                                            : ImGui::GetColorU32(EditorTheme::mutedTextColor());
                drawList->AddText({pos.x + 2.0f, textY}, nameColor, displayName.data(),
                                  displayName.data() + displayName.size());
                const float nameWidth = ImGui::CalcTextSize(displayName.c_str()).x;
                drawList->AddText({pos.x + 2.0f + nameWidth, textY},
                                  ImGui::GetColorU32(EditorTheme::mutedTextColor()), idSuffix.data(),
                                  idSuffix.data() + idSuffix.size());
                ImGui::PopID();

                if (hasKids && !collapsed)
                {
                    for (const size_t childIdx : childIt->second)
                        self(self, childIdx, depth + 1);
                }
            };
            for (const size_t rootIdx : rootIndices)
                renderNode(renderNode, rootIdx, 0);
            ImGui::EndListBox();
        }
        ImGui::End();
    }

    void EditorScene::materialPanel(Editor::EditorApp& app)
    {
        controller_.syncPanelTransactionState();
        if (!materialOpen_)
        {
            if (controller_.activePanelTransaction() == PanelTransactionKind::Material)
                controller_.commitPanelTransactions(app);
            return;
        }
        if (focusMaterialNextFrame_)
        {
            ImGui::SetNextWindowFocus();
            focusMaterialNextFrame_ = false;
        }
        if (!ImGui::Begin("Material", &materialOpen_))
        {
            if (controller_.activePanelTransaction() == PanelTransactionKind::Material)
                controller_.commitPanelTransactions(app);
            ImGui::End();
            return;
        }

        const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const float fontSize = ImGui::GetFontSize();
        const float labelWidth = fontSize * 7.8f;
        const float controlWidth = std::max(120.0f, ImGui::GetContentRegionAvail().x - labelWidth -
                                                        ImGui::GetStyle().ItemSpacing.x);
        const auto rowLabel = [&](const char* label) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::SameLine(labelWidth);
        };

        const auto selectedId = controller_.selected();
        if (selectedId != lastMaterialEntity_)
        {
            lastMaterialEntity_ = selectedId;
            selectedMaterialSlot_ = 0;
        }
        const auto* entity = app.scene().find(selectedId);
        if (!entity)
        {
            if (controller_.activePanelTransaction() == PanelTransactionKind::Material)
                controller_.commitPanelTransactions(app);
            ImGui::TextColored(EditorTheme::mutedTextColor(), "Select an entity to edit materials.");
            ImGui::End();
            return;
        }
        const std::string entityLabel =
            entity->name.empty() ? fmt::format("Unnamed (#{})", entity->id.value())
                                 : fmt::format("{} (#{})", entity->name, entity->id.value());
        ImGui::TextUnformatted(entityLabel.c_str());
        ImGui::Separator();

        if (!entity->model || entity->model->materialSlots().empty())
        {
            if (controller_.activePanelTransaction() == PanelTransactionKind::Material)
                controller_.commitPanelTransactions(app);
            ImGui::TextColored(EditorTheme::mutedTextColor(), "Selected entity has no model materials.");
            ImGui::End();
            return;
        }

        const auto& slots = entity->model->materialSlots();
        if (selectedMaterialSlot_ >= slots.size())
            selectedMaterialSlot_ = 0;

        auto shadingLabel = [](Render::ShadingMode mode) -> const char* {
            switch (mode)
            {
                case Render::ShadingMode::Unlit:
                    return "Unlit";
                case Render::ShadingMode::Lambert:
                    return "Lambert";
                case Render::ShadingMode::Pbr:
                default:
                    return "PBR";
            }
        };

        const auto& activeSlot = slots[selectedMaterialSlot_];
        const std::string activeSlotLabel =
            fmt::format("Slot {} ({})", activeSlot.sourceMaterialIndex, shadingLabel(activeSlot.sourceDesc.shading));
        rowLabel("Material slot");
        ImGui::SetNextItemWidth(controlWidth);
        if (ImGui::BeginCombo("##mat-slot", activeSlotLabel.c_str()))
        {
            for (size_t i = 0; i < slots.size(); ++i)
            {
                const bool isSelected = (i == selectedMaterialSlot_);
                const std::string itemLabel =
                    fmt::format("Slot {} ({})", slots[i].sourceMaterialIndex, shadingLabel(slots[i].sourceDesc.shading));
                if (ImGui::Selectable(itemLabel.c_str(), isSelected))
                {
                    if (controller_.activePanelTransaction() == PanelTransactionKind::Material)
                        controller_.commitPanelTransactions(app);
                    selectedMaterialSlot_ = i;
                }
                if (isSelected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const auto& slot = slots[selectedMaterialSlot_];
        auto alphaSummary = [&]() -> std::string {
            switch (slot.sourceDesc.alphaMode)
            {
                case Render::AlphaMode::Mask:
                    return fmt::format("Mask ({:.2f})", slot.sourceDesc.alphaCutoff);
                case Render::AlphaMode::Blend:
                    return fmt::format("Blend ({:.2f})", slot.sourceDesc.baseAlpha);
                case Render::AlphaMode::Opaque:
                default:
                    return "Opaque";
            }
        };
        const bool hasEmissive = slot.sourceDesc.emissiveTexture.valid() ||
                                 slot.sourceDesc.emissiveFactor[0] > 0.0f ||
                                 slot.sourceDesc.emissiveFactor[1] > 0.0f ||
                                 slot.sourceDesc.emissiveFactor[2] > 0.0f;
        rowLabel("Alpha");
        ImGui::TextUnformatted(alphaSummary().c_str());
        rowLabel("Double-sided");
        ImGui::TextUnformatted(slot.sourceDesc.doubleSided ? "Yes" : "No");
        rowLabel("Emissive");
        ImGui::TextUnformatted(hasEmissive ? "Yes" : "No");

        if (slot.sourceDesc.shading != Render::ShadingMode::Pbr)
        {
            if (controller_.activePanelTransaction() == PanelTransactionKind::Material)
                controller_.commitPanelTransactions(app);
            ImGui::Spacing();
            ImGui::TextColored(
                EditorTheme::mutedTextColor(),
                slot.sourceDesc.shading == Render::ShadingMode::Unlit
                    ? "Source Unlit material preserves imported appearance and cannot be overridden."
                    : "Switch entity shading to PBR in Properties to edit PBR material parameters.");
            ImGui::End();
            return;
        }

        auto& operations = controller_.operations();
        const Scene::MaterialOverride* currentOverride = nullptr;
        for (const auto& entry : entity->materialOverrides)
        {
            if (entry.value.materialSlot == slot.sourceMaterialIndex)
            {
                currentOverride = &entry.value;
                break;
            }
        }

        Scene::MaterialOverride edited{
            slot.sourceMaterialIndex,
            currentOverride ? currentOverride->color : slot.sourceDesc.color,
            currentOverride ? currentOverride->roughnessMode : slot.sourceDesc.roughnessMode,
            currentOverride ? currentOverride->roughness : slot.sourceDesc.roughness,
            currentOverride ? currentOverride->metallicMode : slot.sourceDesc.metallicMode,
            currentOverride ? currentOverride->metallic : slot.sourceDesc.metallic,
        };

        ImGui::Separator();
        rowLabel("Base color");
        const bool colorChanged = ImGui::ColorEdit3("##mat-color", edited.color.data(), ImGuiColorEditFlags_NoInputs);
        ImGui::PushID("##mat-color");
        const bool colorPickerOpen = ImGui::IsPopupOpen("picker");
        ImGui::PopID();
        const bool colorActive = (ImGui::IsItemActive() || colorPickerOpen) && windowFocused;

        rowLabel("Roughness mode");
        ImGui::SetNextItemWidth(controlWidth);
        int roughnessModeIdx = edited.roughnessMode == Render::MaterialParameterMode::FixedValue ? 1 : 0;
        const bool roughnessModeChanged =
            ImGui::Combo("##mat-roughness-mode", &roughnessModeIdx, "Texture x Factor\0Fixed value\0");

        rowLabel("Roughness");
        ImGui::SetNextItemWidth(controlWidth);
        const bool roughnessChanged =
            ImGui::SliderFloat("##mat-roughness", &edited.roughness, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        const bool roughnessActive = ImGui::IsItemActive() && windowFocused;

        rowLabel("Metallic mode");
        ImGui::SetNextItemWidth(controlWidth);
        int metallicModeIdx = edited.metallicMode == Render::MaterialParameterMode::FixedValue ? 1 : 0;
        const bool metallicModeChanged =
            ImGui::Combo("##mat-metallic-mode", &metallicModeIdx, "Texture x Factor\0Fixed value\0");

        rowLabel("Metallic");
        ImGui::SetNextItemWidth(controlWidth);
        const bool metallicChanged =
            ImGui::SliderFloat("##mat-metallic", &edited.metallic, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
        const bool metallicActive = ImGui::IsItemActive() && windowFocused;

        const bool matWidgetActive = colorActive || roughnessActive || metallicActive;
        if (matWidgetActive && controller_.activePanelTransaction() != PanelTransactionKind::Material)
            controller_.beginPanelTransaction(app, PanelTransactionKind::Material);

        if (colorChanged || roughnessChanged || metallicChanged)
            operations.setMaterialOverride(app.scene(), app.clearColor(), edited);

        if (!matWidgetActive && controller_.activePanelTransaction() == PanelTransactionKind::Material)
            controller_.commitPanelTransactions(app);

        if (roughnessModeChanged || metallicModeChanged)
        {
            edited.roughnessMode = roughnessModeIdx == 1 ? Render::MaterialParameterMode::FixedValue
                                                         : Render::MaterialParameterMode::TextureFactor;
            edited.metallicMode = metallicModeIdx == 1 ? Render::MaterialParameterMode::FixedValue
                                                       : Render::MaterialParameterMode::TextureFactor;
            controller_.setMaterialOverride(app, edited);
        }

        ImGui::Spacing();
        const float halfWidth = std::max(60.0f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
        ImGui::BeginDisabled(currentOverride == nullptr);
        if (ImGui::Button("Reset slot##mat-reset-slot", {halfWidth, 0.0f}))
            controller_.resetMaterialOverride(app, slot.sourceMaterialIndex);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(entity->materialOverrides.empty());
        if (ImGui::Button("Reset all##mat-reset-all", {halfWidth, 0.0f}))
            controller_.resetAllMaterialOverrides(app);
        ImGui::EndDisabled();

        ImGui::End();
    }

}  // namespace Incubator::Editor
