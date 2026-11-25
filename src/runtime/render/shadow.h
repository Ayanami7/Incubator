#pragma once

#include <array>
#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <optional>

#include "runtime/core/math/aabb.h"

namespace Incubator::Render
{
    struct CameraMatrices;

    /// @brief 单方向光 CSM 阴影最大支持级联层数。
    inline constexpr uint32_t MAX_SHADOW_CASCADES = 4u;

    /// @brief 消费者提供的单方向光阴影预览设置，不参与场景持久化。
    struct ShadowSettings
    {
        bool enabled = false;
        glm::vec3 center{0};               ///< 固定光源观察体的世界空间中心。
        glm::vec2 halfExtent{6, 6};        ///< 光源观察平面上横纵方向的正半尺寸。
        float nearPlane = 0.1f;            ///< 光源观察位置到近面的正距离。
        float farPlane = 20.1f;            ///< 光源观察位置到远面的距离，大于近面距离。
        float depthBias = 0.001f;          ///< 非负接收偏移，以零到一光源深度为单位。
        uint32_t resolution = 1024u;       ///< 每级联阴影图分辨率（合法 2 的幂次：512..4096）。
        float normalBias = 0.0f;           ///< 非负法线偏移，沿几何世界法线偏移采样位置。
        bool fitToBounds = false;          ///< 单级模式下是否按投射物世界包围盒自适应拟合。
        uint32_t cascadeCount = 1u;        ///< 级联层数，范围 [1, MAX_SHADOW_CASCADES]。
        float cascadeSplitLambda = 0.65f;  ///< 对数-线性切分混合因子，范围 [0, 1]。
        float maxDistance = 60.0f;         ///< 阴影最大有效视图距离（> 0）。
    };

    /// @brief 本帧独立光源投影与接收偏移的值快照；存在时表示启用阴影。
    struct ShadowFrame
    {
        /// @brief 首级（或单级）右手、零到一深度的光源 projection × view，已翻转投影 Y。
        /// @note 片元执行透视除法后使用 uv = ndc.xy * 0.5 + 0.5，不再次翻转 Y。
        glm::mat4 viewProjection{1};
        float depthBias = 0.001f;
        float normalBias = 0.0f;
        uint32_t resolution = 1024u;
        uint32_t cascadeCount = 1u;
        float maxDistance = 60.0f;
        std::array<glm::mat4, MAX_SHADOW_CASCADES> cascadeViewProjections{
            glm::mat4(1), glm::mat4(1), glm::mat4(1), glm::mat4(1)};
        /// @brief 各级联在相机视图空间中的正远深度阈值（单调递增）；单级兼容模式默认覆盖 maxDistance。
        std::array<float, MAX_SHADOW_CASCADES> cascadeSplits{60.0f, 0.0f, 0.0f, 0.0f};
    };

    /// @brief 根据固定观察体与世界空间光线传播方向构造单级阴影帧参数。
    /// @param settings 阴影设置；关闭时直接返回空值，不校验未使用的参数。
    /// @param direction 有限非零传播方向，计算时归一化，不修改输入。
    /// @return 光源位置为 center - normalize(direction) * (nearPlane + farPlane) / 2 的投影快照。
    /// @throws Error::Exception 启用时参数无效或结果无法用有限矩阵表示，抛出 CONFIG。
    /// @note 默认使用世界 Y 上轴；与方向的绝对点积大于 0.99 时改用世界 Z 上轴。
    std::optional<ShadowFrame> makeShadowFrame(const ShadowSettings& settings, const glm::vec3& direction);

    /// @brief 按给定投射物世界包围盒自适应拟合并执行光源空间纹素对齐的单级阴影帧参数。
    /// @note 若包围盒退化为单点或存在非有限端点，则安全回退到 makeShadowFrame 的固定范围。
    std::optional<ShadowFrame> makeShadowFrameForBounds(const ShadowSettings& settings, const glm::vec3& direction,
                                                        const Math::Aabb& casterBounds);

    /// @brief 按相机视锥实用对数-线性切分生成多级联阴影帧参数，并执行包围球与纹素对齐防抖。
    /// @param cameraNear 相机近裁面正距离。
    /// @param cameraFar 相机远裁面正距离，须大于 cameraNear。
    /// @param casterBounds 可选投射物世界包围盒；提供时用于收紧光源深度范围以提高深度精度。
    /// @throws Error::Exception 启用时配置、相机矩阵或裁面参数无效抛出 CONFIG。
    std::optional<ShadowFrame> makeCascadedShadowFrame(
        const ShadowSettings& settings, const glm::vec3& direction, const CameraMatrices& camera, float cameraNear,
        float cameraFar, const std::optional<Math::Aabb>& casterBounds = std::nullopt);
}  // namespace Incubator::Render
