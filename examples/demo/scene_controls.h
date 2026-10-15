#pragma once

#include <imgui.h>

#include <array>
#include <string_view>
#include <vector>

#include "scene_frame.h"

namespace Incubator::Demo
{
    struct SceneObject
    {
        bool visible = true;
        SceneTransform transform;
    };

    struct SceneControls
    {
        SceneObject cube{true, {{-0.8f, 0.0f, 0.0f}, {20.0f, 30.0f, 0.0f}, {1.0f, 1.0f, 1.0f}}};
        SceneObject plane{true, {{0.8f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {1.3f, 1.3f, 1.3f}}};
        SceneCamera camera;
        std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};
    };

    inline std::vector<Render::DrawItem> makeSceneDraws(const SceneControls& scene, const Render::Mesh& cube,
                                                        const Render::Mesh& plane)
    {
        std::vector<Render::DrawItem> draws;
        draws.reserve(2);
        if (scene.cube.visible)
            draws.push_back({cube, makeSceneModel(scene.cube.transform)});
        if (scene.plane.visible)
            draws.push_back({plane, makeSceneModel(scene.plane.transform)});
        return draws;
    }

    inline void drawObjectControls(const char* name, SceneObject& object)
    {
        if (ImGui::CollapsingHeader(name, ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushID(name);
            ImGui::Checkbox("Visible", &object.visible);
            ImGui::SliderFloat3("Position", &object.transform.position.x, -3.0f, 3.0f, "%.2f",
                                ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat3("Rotation", &object.transform.rotationDegrees.x, -180.0f, 180.0f, "%.0f",
                                ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat3("Scale", &object.transform.scale.x, 0.1f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::PopID();
        }
    }

    inline void drawSceneControls(SceneControls& scene, std::string_view texturePath)
    {
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(350, 590), ImGuiCond_FirstUseEver);
        ImGui::Begin("Incubator Demo");
        ImGui::TextWrapped("Texture: %.*s", static_cast<int>(texturePath.size()), texturePath.data());
        ImGui::ColorEdit3("Clear color", scene.clearColor.data());
        drawObjectControls("Cube", scene.cube);
        drawObjectControls("Plane", scene.plane);
        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderFloat3("Target", &scene.camera.target.x, -2.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("Yaw", &scene.camera.yawDegrees, -180.0f, 180.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("Pitch", &scene.camera.pitchDegrees, -80.0f, 80.0f, "%.0f",
                               ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("Distance", &scene.camera.distance, 2.0f, 12.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("FOV", &scene.camera.fieldOfViewDegrees, 20.0f, 80.0f, "%.0f",
                               ImGuiSliderFlags_AlwaysClamp);
        }
        if (ImGui::Button("Reset scene"))
            scene = SceneControls{};
        ImGui::SameLine();
        ImGui::Text("Objects: %d", int(scene.cube.visible) + int(scene.plane.visible));
        ImGui::End();
    }
}  // namespace Incubator::Demo
