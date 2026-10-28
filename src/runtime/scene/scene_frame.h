#pragma once

#include <optional>
#include <vector>

#include "runtime/render/frame_input.h"

namespace Incubator::Scene
{
    class Scene;

    /// @brief 从 Scene 提取的一帧渲染数据。
    /// @note 复制或移动时不会保存指向其他帧 vector 的 span。
    struct SceneFrame
    {
        Render::FrameExtent extent{0, 0};
        Render::CameraMatrices camera{glm::mat4(1), glm::mat4(1)};
        std::vector<Render::DrawItem> draws;
        Render::Lighting lighting{};
        std::optional<Render::ShadowFrame> shadow{};
        std::vector<Render::DrawItem> shadowDraws;

        /// @brief 创建提交渲染所需的借用输入。
        /// @return 指向此帧两份绘制列表的输入视图。
        /// @note 仅在提交时借用；渲染提交返回前必须保持此帧及其存储有效。
        Render::FrameInput input() const&;
        Render::FrameInput input() const&& = delete;
    };

    /// @brief 从场景提取用于一次渲染提交的帧数据。
    /// @param scene 待提取的场景。
    /// @param extent 实际渲染尺寸。
    /// @param frustumCulling 是否执行视锥体剔除。
    /// @param shadows 消费者阴影设置；启用时从全部可见且有模型的实体提取投射物，不执行光源裁剪。
    /// @return 成功提取的场景帧；尺寸无效时返回空值。
    /// @note 不执行 I/O、资源上传或 GPU 验证。仅剔除边界可靠且完全位于当前视锥体之外的实体。
    /// @throws Error::Exception 有效尺寸下，启用的阴影设置无效时抛出 CONFIG。
    std::optional<SceneFrame> extractSceneFrame(const Scene& scene, Render::FrameExtent extent,
                                                bool frustumCulling = true, const Render::ShadowSettings& shadows = {});
}  // namespace Incubator::Scene
