#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <optional>

namespace Incubator::Render
{
    /// @brief 消费者提供的单方向光阴影预览设置，不参与场景持久化。
    struct ShadowSettings
    {
        bool enabled = false;
        glm::vec3 center{0};         ///< 固定光源观察体的世界空间中心。
        glm::vec2 halfExtent{6, 6};  ///< 光源观察平面上横纵方向的正半尺寸。
        float nearPlane = 0.1f;      ///< 光源观察位置到近面的正距离。
        float farPlane = 20.1f;      ///< 光源观察位置到远面的距离，大于近面距离。
        float depthBias = 0.001f;    ///< 非负接收偏移，以零到一光源深度为单位。
    };

    /// @brief 本帧独立光源投影与接收偏移的值快照；存在时表示启用阴影。
    struct ShadowFrame
    {
        /// @brief 右手、零到一深度的光源 projection × view，已翻转投影 Y。
        /// @note 片元执行透视除法后使用 uv = ndc.xy * 0.5 + 0.5，不再次翻转 Y。
        glm::mat4 viewProjection{1};
        float depthBias = 0.001f;
    };

    /// @brief 根据固定观察体与世界空间光线传播方向构造阴影帧参数。
    /// @param settings 阴影设置；关闭时直接返回空值，不校验未使用的参数。
    /// @param direction 有限非零传播方向，计算时归一化，不修改输入。
    /// @return 光源位置为 center - normalize(direction) * (nearPlane + farPlane) / 2 的投影快照。
    /// @throws Error::Exception 启用时参数无效或结果无法用有限矩阵表示，抛出 CONFIG。
    /// @note 默认使用世界 Y 上轴；与方向的绝对点积大于 0.99 时改用世界 Z 上轴。
    std::optional<ShadowFrame> makeShadowFrame(const ShadowSettings& settings, const glm::vec3& direction);
}  // namespace Incubator::Render
