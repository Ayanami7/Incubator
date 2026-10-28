#pragma once
#include <memory>

#include "runtime/render/frame_input.h"
namespace Incubator::Render::Detail
{
    struct RendererReviewAccess;
}
namespace Incubator::Render
{
    /// @brief 当前帧槽场景图像的非拥有引用；尺寸重建或 Renderer 关闭后失效。
    class SceneOutput
    {
    public:
        bool valid() const noexcept
        {
            return !impl_.expired();
        }
        FrameExtent extent() const noexcept
        {
            return extent_;
        }

    private:
        friend class Renderer;
        friend struct Detail::RendererReviewAccess;
        struct Impl;
        std::weak_ptr<Impl> impl_;
        FrameExtent extent_{};
    };
}  // namespace Incubator::Render
