#include "runtime/render/renderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/cube_mesh.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_binding_set.h"
#include "runtime/render/vulkan/vulkan_buffer.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_frame_scheduler.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_instance.h"
#include "runtime/render/vulkan/vulkan_pipeline.h"
#include "runtime/render/vulkan/vulkan_sampler.h"
#include "runtime/render/vulkan/vulkan_swapchain.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/texture_resource.h"

namespace Incubator
{
    namespace Render
    {
        namespace
        {
            using Detail::CUBE_INDICES;
            using Detail::CUBE_VERTICES;
            using Detail::CubeVertex;

            struct alignas(16) TransformUniform
            {
                glm::mat4 model;
                glm::mat4 view;
                glm::mat4 projection;
            };
            static_assert(sizeof(TransformUniform) == 3 * 16 * sizeof(float));

            std::vector<const char*> requiredRendererInstanceExtensions()
            {
                std::vector<const char*> extensions = Vulkan::GlfwSurface::requiredInstanceExtensions();
                const auto addIfMissing = [&extensions](const char* extension) {
                    const bool alreadyPresent = std::any_of(
                        extensions.begin(), extensions.end(),
                        [extension](const char* current) { return std::string_view(current) == extension; });
                    if (!alreadyPresent)
                        extensions.push_back(extension);
                };
                addIfMissing(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
                addIfMissing(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
                return extensions;
            }

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
            Impl(Window::Window& window, const Resource::ShaderResource& vertexShader,
                 const Resource::ShaderResource& fragmentShader, const Resource::TextureResource& texture,
                 RendererValidationMode validationMode)
                : window_(window),
                  instance_(requiredRendererInstanceExtensions(), toVulkanValidationMode(validationMode)),
                  surface_(instance_, window_),
                  device_(instance_, surface_),
                  vertexShader_(vertexShader.words()),
                  fragmentShader_(fragmentShader.words())
            {
                depthFormat_ = device_.findDepthFormat();
                vertexBuffer_ = Vulkan::VulkanBuffer::uploadStatic(
                    device_, std::as_bytes(std::span<const CubeVertex>(CUBE_VERTICES)),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
                indexBuffer_ = Vulkan::VulkanBuffer::uploadStatic(
                    device_, std::as_bytes(std::span<const uint16_t>(CUBE_INDICES)), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
                texture_ = Vulkan::VulkanImage::uploadRgba8(device_, {texture.width(), texture.height()},
                                                            VK_FORMAT_R8G8B8A8_SRGB, texture.pixels(),
                                                            VK_IMAGE_USAGE_SAMPLED_BIT);
                sampler_ = std::make_shared<Vulkan::VulkanSampler>(device_);
                for (auto& uniformBuffer : uniformBuffers_)
                {
                    uniformBuffer = std::make_shared<Vulkan::VulkanBuffer>(
                        device_, sizeof(TransformUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                }
                bindingSet_ =
                    std::make_unique<Vulkan::VulkanBindingSet>(device_, uniformBuffers_, *texture_, *sampler_);
                scheduler_ = std::make_unique<Vulkan::VulkanFrameScheduler>(device_);
                imguiRenderer_ = std::make_unique<Integration::ImGuiRenderer>(window_);
                int width = 0;
                int height = 0;
                window_.getFramebufferSize(width, height);
                if (width > 0 && height > 0)
                    rebuild(width, height, "initialization");
            }

            bool prepareFrame()
            {
                int width = 0;
                int height = 0;
                window_.getFramebufferSize(width, height);
                if (width <= 0 || height <= 0 || window_.isIconified())
                    return false;

                const bool sizeChanged = requestedWidth_ != width || requestedHeight_ != height;
                const char* rebuildReason = !swapchain_     ? "initialization"
                                            : needsRebuild_ ? "status recovery"
                                                            : "framebuffer size changed";
                if ((!swapchain_ || needsRebuild_ || sizeChanged) && !rebuild(width, height, rebuildReason))
                    return false;
                return swapchain_ && swapchain_->handle() != VK_NULL_HANDLE;
            }

            std::optional<FrameExtent> beginUiFrame()
            {
                if (!prepareFrame())
                    return std::nullopt;
                imguiRenderer_->beginFrame();
                const auto extent = swapchain_->extent();
                return FrameExtent{extent.width, extent.height};
            }

            void drawFrame(const FrameInput& input, const std::array<float, 4>& clearColor,
                           const std::function<void(void*)>& recordUi)
            {
                if (!prepareFrame())
                    return;
                const auto extent = swapchain_->extent();
                if (input.extent != FrameExtent{extent.width, extent.height})
                    return;

                const uint32_t frameIndex = scheduler_->currentFrameIndex();
                scheduler_->waitForCurrentFrame();
                const TransformUniform transforms{input.model, input.camera.view, input.camera.projection};
                uniformBuffers_[frameIndex]->write(0, std::as_bytes(std::span<const TransformUniform>(&transforms, 1)));

                VkClearColorValue color{};
                color.float32[0] = clearColor[0];
                color.float32[1] = clearColor[1];
                color.float32[2] = clearColor[2];
                color.float32[3] = clearColor[3];
                const std::array<std::shared_ptr<void>, 5> inFlightResources = {
                    std::static_pointer_cast<void>(vertexBuffer_), std::static_pointer_cast<void>(indexBuffer_),
                    std::static_pointer_cast<void>(uniformBuffers_[frameIndex]),
                    std::static_pointer_cast<void>(texture_), std::static_pointer_cast<void>(sampler_)};
                if (!scheduler_->renderAndPresent(
                        *swapchain_, color, *pipeline_, *vertexBuffer_, *indexBuffer_,
                        bindingSet_->descriptorSet(frameIndex), static_cast<uint32_t>(CUBE_INDICES.size()),
                        [&recordUi](VkCommandBuffer commandBuffer) {
                            if (recordUi)
                                recordUi(static_cast<void*>(commandBuffer));
                        },
                        inFlightResources))
                    needsRebuild_ = true;
            }

            Integration::ImGuiRenderer& imgui()
            {
                return *imguiRenderer_;
            }

        private:
            bool rebuild(int width, int height, const char* reason)
            {
                scheduler_->waitForFrames();
                scheduler_->waitForPresentation();
                const VkFormat previousFormat = swapchain_ ? swapchain_->format() : VK_FORMAT_UNDEFINED;
                bool recreated = false;
                if (swapchain_)
                {
                    recreated = swapchain_->recreate(width, height);
                }
                else
                {
                    swapchain_ = std::make_unique<Vulkan::VulkanSwapchain>(device_, surface_, width, height);
                    recreated = swapchain_->handle() != VK_NULL_HANDLE;
                }
                if (!recreated || swapchain_->handle() == VK_NULL_HANDLE)
                {
                    needsRebuild_ = true;
                    return false;
                }

                if (swapchain_->handle() != VK_NULL_HANDLE)
                {
                    scheduler_->initializeSwapchainImages(swapchain_->imageCount(), swapchain_->extent(), depthFormat_);
                    if (!pipeline_ || previousFormat != swapchain_->format())
                    {
                        pipeline_ = std::make_unique<Vulkan::VulkanPipeline>(device_, swapchain_->format(),
                                                                             depthFormat_, bindingSet_->layout(),
                                                                             vertexShader_, fragmentShader_);
                    }
                    imguiRenderer_->initializeVulkan(instance_.handle(), device_.physicalDevice(), device_.handle(),
                                                     device_.queueFamilies().graphics, device_.graphicsQueue(),
                                                     swapchain_->format(), depthFormat_, swapchain_->imageCount());
                }
                requestedWidth_ = width;
                requestedHeight_ = height;
                needsRebuild_ = false;
                Log::Registry::get("render").info(
                    "Swapchain {}: {}x{}, format {}, colorSpace {}, {} images, present mode FIFO", reason,
                    swapchain_->extent().width, swapchain_->extent().height, static_cast<int>(swapchain_->format()),
                    static_cast<int>(swapchain_->colorSpace()), swapchain_->imageCount());
                return true;
            }

            Window::Window& window_;
            Vulkan::VulkanInstance instance_;
            Vulkan::GlfwSurface surface_;
            Vulkan::VulkanDevice device_;
            std::vector<uint32_t> vertexShader_;
            std::vector<uint32_t> fragmentShader_;
            std::shared_ptr<Vulkan::VulkanBuffer> vertexBuffer_;
            std::shared_ptr<Vulkan::VulkanBuffer> indexBuffer_;
            std::array<std::shared_ptr<Vulkan::VulkanBuffer>, Vulkan::VulkanFrameScheduler::FRAME_COUNT>
                uniformBuffers_{};
            std::shared_ptr<Vulkan::VulkanImage> texture_;
            std::shared_ptr<Vulkan::VulkanSampler> sampler_;
            std::unique_ptr<Vulkan::VulkanBindingSet> bindingSet_;
            std::unique_ptr<Vulkan::VulkanSwapchain> swapchain_;
            std::unique_ptr<Vulkan::VulkanPipeline> pipeline_;
            std::unique_ptr<Vulkan::VulkanFrameScheduler> scheduler_;
            std::unique_ptr<Integration::ImGuiRenderer> imguiRenderer_;
            VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
            int requestedWidth_ = 0;
            int requestedHeight_ = 0;
            bool needsRebuild_ = false;
        };

        Renderer::Renderer(Window::Window& window, const Resource::ShaderResource& vertexShader,
                           const Resource::ShaderResource& fragmentShader, const Resource::TextureResource& texture,
                           RendererValidationMode validationMode)
            : impl_(std::make_unique<Impl>(window, vertexShader, fragmentShader, texture, validationMode))
        {}

        Renderer::~Renderer() = default;

        Renderer::Renderer(Renderer&&) noexcept = default;

        Renderer& Renderer::operator=(Renderer&&) noexcept = default;

        std::optional<FrameExtent> Renderer::beginUiFrame()
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->beginUiFrame();
        }

        void Renderer::drawFrame(const FrameInput& input, const std::array<float, 4>& clearColor,
                                 const std::function<void(void*)>& recordUi)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            impl_->drawFrame(input, clearColor, recordUi);
        }

        Integration::ImGuiRenderer& Renderer::imgui()
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->imgui();
        }
    }  // namespace Render
}  // namespace Incubator
