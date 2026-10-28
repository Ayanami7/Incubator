#pragma once

#include <algorithm>
#include <cmath>
#include <glm/gtc/constants.hpp>
#include <limits>
#include <optional>

#include "runtime/core/math/aabb.h"
#include "runtime/scene/camera.h"
#include "ui_input.h"

namespace Incubator::Demo
{
    class OrbitCamera
    {
    public:
        void cancel()
        {
            gesture_ = Gesture::None;
        }

        void synchronize(const Scene::Camera& camera)
        {
            cancel();
            distance_ = std::max(glm::length(camera.position()), 0.01f);
            const auto forward = camera.rotation() * glm::vec3(0, 0, -1);
            target_ = camera.position() + forward * distance_;
            yaw_ = std::atan2(-forward.x, -forward.z);
            pitch_ = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
            observed_ = camera;
            synchronized_ = true;
        }

        /// @brief 根据 UI 输入更新轨道相机。
        /// @param camera 待更新的相机。
        /// @param input 本帧 UI 输入。
        /// @param windowSize 交互区域的逻辑尺寸；视口消费者传入图像逻辑尺寸，而非帧缓冲尺寸。
        /// @return 相机发生变化时返回 true。
        bool apply(Scene::Camera& camera, const UiInput& input, Input::PointerValue windowSize)
        {
            if (!synchronized_ || changed(camera))
                synchronize(camera);
            if (windowSize.x <= 0 || windowSize.y <= 0)
            {
                cancel();
                return false;
            }
            const bool ending = (gesture_ == Gesture::Rotate && (input.rotate.released || !input.rotate.down)) ||
                                (gesture_ == Gesture::Pan && (input.pan.released || !input.pan.down));
            bool moved = false;
            auto rotation = camera.rotation();
            if (gesture_ != Gesture::None && (input.delta.x != 0 || input.delta.y != 0))
            {
                if (gesture_ == Gesture::Rotate)
                {
                    yaw_ -= static_cast<float>(input.delta.x) * 0.005f;
                    pitch_ = std::clamp(pitch_ - static_cast<float>(input.delta.y) * 0.005f,
                                        -glm::half_pi<float>() + 0.01f, glm::half_pi<float>() - 0.01f);
                    rotation = glm::angleAxis(yaw_, glm::vec3(0, 1, 0)) * glm::angleAxis(pitch_, glm::vec3(1, 0, 0));
                }
                else
                {
                    const float units = static_cast<float>(
                        2 * distance_ * std::tan(camera.perspective().verticalFov / 2) / windowSize.y);
                    target_ += rotation * glm::vec3(-input.delta.x * units, input.delta.y * units, 0);
                }
                moved = true;
            }
            if ((!input.captureMouse || gesture_ != Gesture::None) && input.scroll.y != 0)
            {
                distance_ =
                    std::clamp(distance_ * static_cast<float>(std::exp(-input.scroll.y * 0.1)), 0.01f, 1000000.0f);
                moved = true;
            }
            // NewFrame 可能会把末段位移和释放事件放在同一批输入中；这段位移仍属于
            // 当前手势；显式取消操作已清除 gesture_。
            if (ending)
                cancel();
            // 旧手势结束后，本批次的按下事件可以启动下一个手势。
            // 该批位移已在上方处理，不得再次用于移动新手势。
            if (gesture_ == Gesture::None && !input.captureMouse)
            {
                if (input.rotate.pressed && input.rotate.down)
                    gesture_ = Gesture::Rotate;
                else if (input.pan.pressed && input.pan.down)
                    gesture_ = Gesture::Pan;
            }
            if (moved)
            {
                camera = Scene::Camera(target_ + rotation * glm::vec3(0, 0, distance_), rotation, camera.perspective());
                observed_ = camera;
            }
            return moved;
        }

        bool focus(Scene::Camera& camera, const std::optional<Math::Aabb>& bounds, Render::FrameExtent extent)
        {
            if (!bounds || extent.width == 0 || extent.height == 0)
                return false;
            const glm::dvec3 center = (glm::dvec3(bounds->min) + glm::dvec3(bounds->max)) * 0.5;
            const auto inverse = glm::conjugate(glm::dquat(camera.rotation()));
            const double tanY = std::tan(camera.perspective().verticalFov / 2);
            const double tanX = tanY * extent.width / extent.height;
            double distance = 0.01, minZ = std::numeric_limits<double>::max(), maxZ = -minZ;
            for (int i = 0; i < 8; ++i)
            {
                const glm::dvec3 corner((i & 1) ? bounds->max.x : bounds->min.x,
                                        (i & 2) ? bounds->max.y : bounds->min.y,
                                        (i & 4) ? bounds->max.z : bounds->min.z);
                const auto local = inverse * (corner - center);
                distance = std::max({distance, local.z + std::abs(local.x) * 1.1 / tanX,
                                     local.z + std::abs(local.y) * 1.1 / tanY, local.z + 0.01});
                minZ = std::min(minZ, local.z);
                maxZ = std::max(maxZ, local.z);
            }
            auto perspective = camera.perspective();
            perspective.nearPlane =
                static_cast<float>(std::min(double(perspective.nearPlane), (distance - maxZ) * 0.5));
            perspective.farPlane = static_cast<float>(std::max(double(perspective.farPlane), (distance - minZ) * 1.1));
            const glm::vec3 position(center + glm::dquat(camera.rotation()) * glm::dvec3(0, 0, distance));
            if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z) ||
                !std::isfinite(perspective.farPlane) || perspective.nearPlane <= 0)
                return false;
            const Scene::Camera focused(position, camera.rotation(), perspective);
            synchronize(focused);
            target_ = glm::vec3(center);
            distance_ = static_cast<float>(distance);
            camera = focused;
            return true;
        }

    private:
        bool changed(const Scene::Camera& camera) const
        {
            const auto& a = camera.perspective();
            const auto& b = observed_.perspective();
            return camera.position() != observed_.position() || camera.rotation() != observed_.rotation() ||
                   a.verticalFov != b.verticalFov || a.nearPlane != b.nearPlane || a.farPlane != b.farPlane;
        }
        enum class Gesture
        {
            None,
            Rotate,
            Pan
        };
        Gesture gesture_ = Gesture::None;
        Scene::Camera observed_;
        glm::vec3 target_{0};
        float distance_ = 6, yaw_ = 0, pitch_ = 0;
        bool synchronized_ = false;
    };
}  // namespace Incubator::Demo
