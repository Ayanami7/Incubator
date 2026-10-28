#pragma once

#include <array>
#include <string>

#include "runtime/platform/window/window.h"
#include "runtime/render/renderer.h"

namespace Incubator::Demo
{
    /// @brief Demo 会话创建参数，不包含场景内容或交互状态。
    struct DemoOptions
    {
        Window::WindowDesc window = [] {
            Window::WindowDesc desc;
            desc.width = 1920;
            desc.height = 1080;
            return desc;
        }();
        Render::RendererValidationMode validation = Render::RendererValidationMode::Disabled;
        std::string vertexShader = "shaders/cube.vert.spv";
        std::string fragmentShader = "shaders/cube.frag.spv";
        std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1};
    };
}  // namespace Incubator::Demo
