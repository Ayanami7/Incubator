#pragma once

#include <glm/gtc/quaternion.hpp>

#include "runtime/render/frame_input.h"

namespace Incubator::Scene
{
    /// @brief 透视投影参数。
    struct Perspective
    {
        /// @brief 垂直视野角，单位为弧度，且严格处于 0 到 pi 之间。
        float verticalFov = glm::radians(45.0f);
        /// @brief 近裁剪面距离，必须有限且大于 0。
        float nearPlane = 0.1f;
        /// @brief 远裁剪面距离，必须大于 nearPlane。
        float farPlane = 100.0f;
    };

    /// @brief 表示世界空间中的相机刚体位姿和透视参数。
    /// @note 局部前方向为 -Z，上方向为 +Y；不包含缩放、轨道或控制器状态。旋转遵循 Transform 的归一化及 CONFIG 失败规则。
    class Camera
    {
    public:
        explicit Camera(glm::vec3 position = glm::vec3(0), glm::quat rotation = glm::quat(1, 0, 0, 0),
                        Perspective perspective = {});

        const glm::vec3& position() const;
        const glm::quat& rotation() const;
        const Perspective& perspective() const;
        void setPosition(glm::vec3 position);
        void setRotation(glm::quat rotation);
        void setPerspective(Perspective perspective);

        /// @brief 根据相机位姿和透视参数计算视图及投影矩阵。
        /// @param extent 实际渲染尺寸，宽度和高度都必须非零。
        /// @return 世界位姿的逆矩阵，以及仅应用一次最终 Y 轴适配的 RH/ZO 透视矩阵。
        /// @throws Error::Exception 渲染尺寸无效时抛出 CONFIG 错误。
        Render::CameraMatrices matrices(Render::FrameExtent extent) const;

    private:
        glm::vec3 position_;
        glm::quat rotation_;
        Perspective perspective_;
    };
}  // namespace Incubator::Scene
