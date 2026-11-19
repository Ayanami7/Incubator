#pragma once

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <glm/trigonometric.hpp>

#include "runtime/scene/scene.h"

namespace Incubator::App
{
    /// @brief 灯光控件当帧交互结果。
    struct LightingControlResult
    {
        bool changed = false;  ///< 本帧修改了至少一项光照参数。
        bool active = false;   ///< 任意灯光滑条或调色盘处于活动/拖动状态。
    };

    /// @brief 绘制并原地编辑 Lighting 值，不直接访问 Scene。
    /// @param lighting 待编辑的光照值。
    /// @return 本帧是否发生修改及控件活动状态。
    inline LightingControlResult lightingWidget(Render::Lighting& lighting)
    {
        const double length =
            std::hypot(double(lighting.direction.x), double(lighting.direction.y), double(lighting.direction.z));
        const auto direction = glm::dvec3(lighting.direction) / length;
        float yaw = glm::degrees(float(std::atan2(direction.x, -direction.z)));
        float pitch = glm::degrees(float(std::asin(std::clamp(direction.y, -1.0, 1.0))));
        bool active = false;
        ImGui::TextUnformatted("Light yaw");
        bool directionChanged = ImGui::SliderFloat("##lightYaw", &yaw, -180, 180, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        active |= ImGui::IsItemActive();
        ImGui::TextUnformatted("Light pitch");
        directionChanged |= ImGui::SliderFloat("##lightPitch", &pitch, -89, 89, "%.0f", ImGuiSliderFlags_AlwaysClamp);
        active |= ImGui::IsItemActive();
        if (directionChanged)
        {
            const float y = glm::radians(yaw), p = glm::radians(pitch);
            lighting.direction = {std::cos(p) * std::sin(y), std::sin(p), -std::cos(p) * std::cos(y)};
        }
        bool changed = directionChanged;
        ImGui::TextUnformatted("Color (linear)");
        changed |= ImGui::ColorEdit3("##lightColor", &lighting.color.x,
                                     ImGuiColorEditFlags_Float | ImGuiColorEditFlags_NoInputs);
        active |= ImGui::IsItemActive() || ImGui::IsPopupOpen("##lightColorpicker", ImGuiPopupFlags_AnyPopupId);
        ImGui::TextUnformatted("Intensity");
        changed |= ImGui::SliderFloat("##intensity", &lighting.intensity, 0, 4, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        active |= ImGui::IsItemActive();
        ImGui::TextUnformatted("Ambient");
        changed |= ImGui::SliderFloat("##ambient", &lighting.ambient, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        active |= ImGui::IsItemActive();
        return {changed, active};
    }

    /// @brief 编辑 Scene 当前光照；角度及颜色只在本次 UI 构建期间暂存。
    inline void lightingControls(Scene::Scene& scene)
    {
        auto lighting = scene.lighting();
        if (lightingWidget(lighting).changed)
            scene.setLighting(lighting);
    }

}  // namespace Incubator::App
