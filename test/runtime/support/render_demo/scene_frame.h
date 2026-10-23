#pragma once

#include <glm/gtc/matrix_transform.hpp>

#include "runtime/render/frame_input.h"
#include "runtime/scene/camera.h"
#include "runtime/scene/transform.h"

namespace Incubator
{
    namespace Demo
    {
        struct SceneTransform
        {
            glm::vec3 position{0.0f};
            glm::vec3 rotationDegrees{0.0f};
            glm::vec3 scale{1.0f};
        };

        struct SceneCamera
        {
            glm::vec3 target{0.0f};
            float yawDegrees = 0.0f;
            float pitchDegrees = 15.0f;
            float distance = 5.0f;
            float fieldOfViewDegrees = 45.0f;
        };

        inline glm::mat4 makeSceneModel(const SceneTransform& transform)
        {
            // GLM 的欧拉四元数顺序为 Rz * Ry * Rx，与现有角度控件一致。
            return Scene::Transform(transform.position, glm::quat(glm::radians(transform.rotationDegrees)),
                                    transform.scale)
                .matrix();
        }

        // 仅在 Renderer 提供可绘制尺寸后调用。绘制列表由调用方持有。
        inline Render::FrameInput makeSceneFrame(Render::FrameExtent extent, std::span<const Render::DrawItem> draws,
                                                 const SceneCamera& camera = {})
        {
            const float yaw = glm::radians(camera.yawDegrees);
            const float pitch = glm::radians(camera.pitchDegrees);
            const auto offset = camera.distance * glm::vec3(glm::cos(pitch) * glm::sin(yaw), glm::sin(pitch),
                                                            glm::cos(pitch) * glm::cos(yaw));
            const auto rotation = glm::quatLookAtRH(-glm::normalize(offset), glm::vec3(0, 1, 0));
            const Scene::Camera runtimeCamera(camera.target + offset, rotation,
                                              {glm::radians(camera.fieldOfViewDegrees), 0.1f, 50.0f});
            return {extent, runtimeCamera.matrices(extent), draws};
        }
    }  // namespace Demo
}  // namespace Incubator
