#pragma once

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>

namespace Incubator::Scene
{
    /// @brief 由平移、旋转和缩放组成的 TRS 变换值。
    /// @note 输入旋转会归一化；零缩放和负缩放均有效。旋转非有限或长度为零时抛出 CONFIG，且不修改当前值。
    class Transform
    {
    public:
        explicit Transform(glm::vec3 position = glm::vec3(0), glm::quat rotation = glm::quat(1, 0, 0, 0),
                           glm::vec3 scale = glm::vec3(1));

        const glm::vec3& position() const;
        const glm::quat& rotation() const;
        const glm::vec3& scale() const;
        void setPosition(glm::vec3 position);
        void setRotation(glm::quat rotation);
        void setScale(glm::vec3 scale);

        /// @brief 计算此变换的矩阵。
        /// @return 使用列向量约定的 T * R * S 矩阵。
        /// @note 单位由使用该变换的场景或模型决定。
        glm::mat4 matrix() const;

    private:
        glm::vec3 position_;
        glm::quat rotation_;
        glm::vec3 scale_;
    };
}  // namespace Incubator::Scene
