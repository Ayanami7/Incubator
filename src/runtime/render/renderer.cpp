#include "runtime/render/renderer.h"

#include <memory>

#include "runtime/core/error/error.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_frame_scheduler.h"
#include "runtime/render/vulkan/vulkan_instance.h"
#include "runtime/render/vulkan/vulkan_swapchain.h"

namespace Incubator
{
    namespace Render
    {
        namespace
        {
            Vulkan::ValidationMode toVulkanValidationMode(RendererValidationMode mode)
            {
                switch (mode)
                {
                    case RendererValidationMode::Optional:
                        return Vulkan::ValidationMode::Optional;
                    case RendererValidationMode::Required:
                        return Vulkan::ValidationMode::Required;
                    case RendererValidationMode::Disabled:
                    default:
                        return Vulkan::ValidationMode::Disabled;
                }
            }
        }  // namespace

        class Renderer::Impl
        {
        public:
            Impl(Window::Window& window, RendererValidationMode validationMode)
                : window_(window), instance_(Vulkan::GlfwSurface::requiredInstanceExtensions(),
                                             toVulkanValidationMode(validationMode)),
                  surface_(instance_, window_), device_(instance_, surface_)
            {
                scheduler_ = std::make_unique<Vulkan::VulkanFrameScheduler>(device_);
                int width = 0;
                int height = 0;
                window_.getFramebufferSize(width, height);
                if (width > 0 && height > 0)
                    rebuild(width, height);
            }

            void drawFrame(const std::array<float, 4>& clearColor)
            {
                int width = 0;
                int height = 0;
                window_.getFramebufferSize(width, height);
                if (width <= 0 || height <= 0)
                    return;

                if (!swapchain_ || needsRebuild_ || requestedWidth_ != width || requestedHeight_ != height)
                    rebuild(width, height);
                if (!swapchain_ || swapchain_->handle() == VK_NULL_HANDLE)
                    return;

                VkClearColorValue color{};
                color.float32[0] = clearColor[0];
                color.float32[1] = clearColor[1];
                color.float32[2] = clearColor[2];
                color.float32[3] = clearColor[3];
                if (!scheduler_->renderAndPresent(*swapchain_, color))
                    needsRebuild_ = true;
            }

        private:
            void rebuild(int width, int height)
            {
                scheduler_->waitForFrames();
                scheduler_->waitForPresentation();
                if (swapchain_)
                {
                    swapchain_->recreate(width, height);
                }
                else
                {
                    swapchain_ = std::make_unique<Vulkan::VulkanSwapchain>(device_, surface_, width, height);
                }
                if (swapchain_->handle() != VK_NULL_HANDLE)
                    scheduler_->initializeSwapchainImages(swapchain_->imageCount());
                requestedWidth_ = width;
                requestedHeight_ = height;
                needsRebuild_ = false;
            }

            Window::Window& window_;
            Vulkan::VulkanInstance instance_;
            Vulkan::GlfwSurface surface_;
            Vulkan::VulkanDevice device_;
            std::unique_ptr<Vulkan::VulkanSwapchain> swapchain_;
            std::unique_ptr<Vulkan::VulkanFrameScheduler> scheduler_;
            int requestedWidth_ = 0;
            int requestedHeight_ = 0;
            bool needsRebuild_ = false;
        };

        Renderer::Renderer(Window::Window& window, RendererValidationMode validationMode)
            : impl_(std::make_unique<Impl>(window, validationMode))
        {
        }

        Renderer::~Renderer() = default;

        Renderer::Renderer(Renderer&&) noexcept = default;

        Renderer& Renderer::operator=(Renderer&&) noexcept = default;

        void Renderer::drawFrame(const std::array<float, 4>& clearColor)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            impl_->drawFrame(clearColor);
        }
    }  // namespace Render
}  // namespace Incubator
