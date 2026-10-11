#include "runtime/render/renderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/resource/loader/spirv_reader.h"
#include "runtime/render/vulkan/vulkan_buffer.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_frame_scheduler.h"
#include "runtime/render/vulkan/vulkan_instance.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_pipeline.h"
#include "runtime/render/vulkan/vulkan_swapchain.h"

namespace Incubator
{
    namespace Render
    {
        namespace
        {
            struct TriangleVertex
            {
                float position[2];
                float color[3];
            };
            static_assert(sizeof(TriangleVertex) == 5 * sizeof(float));

            constexpr std::array<TriangleVertex, 3> TRIANGLE_VERTICES = {{
                {{0.0f, -0.65f}, {1.0f, 0.25f, 0.18f}},
                {{0.65f, 0.55f}, {0.20f, 0.85f, 0.35f}},
                {{-0.65f, 0.55f}, {0.20f, 0.45f, 1.0f}},
            }};
            constexpr std::array<uint16_t, 3> TRIANGLE_INDICES = {0, 1, 2};
            constexpr std::array<uint8_t, 16> CHECKER_TEXTURE = {
                245, 245, 245, 255, 30, 90, 220, 255,
                30, 90, 220, 255, 245, 245, 245, 255};

            std::vector<const char*> requiredRendererInstanceExtensions()
            {
                std::vector<const char*> extensions = Vulkan::GlfwSurface::requiredInstanceExtensions();
                const auto addIfMissing = [&extensions](const char* extension) {
                    const bool alreadyPresent = std::any_of(
                        extensions.begin(), extensions.end(), [extension](const char* current) {
                            return std::string_view(current) == extension;
                        });
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
            Impl(Window::Window& window, RendererValidationMode validationMode)
                : window_(window), instance_(requiredRendererInstanceExtensions(),
                                             toVulkanValidationMode(validationMode)),
                  surface_(instance_, window_), device_(instance_, surface_),
                  vertexShader_(Resource::Loader::readSpirv(*fileIO_, "assets/shaders/triangle.vert.spv")),
                  fragmentShader_(Resource::Loader::readSpirv(*fileIO_, "assets/shaders/triangle.frag.spv"))
            {
                vertexBuffer_ = Vulkan::VulkanBuffer::uploadStatic(
                    device_, std::as_bytes(std::span<const TriangleVertex>(TRIANGLE_VERTICES)),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
                indexBuffer_ = Vulkan::VulkanBuffer::uploadStatic(
                    device_, std::as_bytes(std::span<const uint16_t>(TRIANGLE_INDICES)),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                    VK_ACCESS_2_INDEX_READ_BIT);
                checkerTexture_ = Vulkan::VulkanImage::uploadRgba8(
                    device_, {2, 2}, VK_FORMAT_R8G8B8A8_SRGB, CHECKER_TEXTURE, VK_IMAGE_USAGE_SAMPLED_BIT);
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
                const std::array<std::shared_ptr<void>, 3> inFlightResources = {
                    std::static_pointer_cast<void>(vertexBuffer_), std::static_pointer_cast<void>(indexBuffer_),
                    std::static_pointer_cast<void>(checkerTexture_)};
                if (!scheduler_->renderAndPresent(*swapchain_, color, *pipeline_, *vertexBuffer_, *indexBuffer_,
                                                  inFlightResources))
                    needsRebuild_ = true;
            }

        private:
            void rebuild(int width, int height)
            {
                scheduler_->waitForFrames();
                scheduler_->waitForPresentation();
                const VkFormat previousFormat = swapchain_ ? swapchain_->format() : VK_FORMAT_UNDEFINED;
                if (swapchain_)
                {
                    swapchain_->recreate(width, height);
                }
                else
                {
                    swapchain_ = std::make_unique<Vulkan::VulkanSwapchain>(device_, surface_, width, height);
                }
                if (swapchain_->handle() != VK_NULL_HANDLE)
                {
                    scheduler_->initializeSwapchainImages(swapchain_->imageCount());
                    if (!pipeline_ || previousFormat != swapchain_->format())
                    {
                        pipeline_ = std::make_unique<Vulkan::VulkanPipeline>(device_, swapchain_->format(),
                                                                             vertexShader_, fragmentShader_);
                    }
                }
                requestedWidth_ = width;
                requestedHeight_ = height;
                needsRebuild_ = false;
            }

            Window::Window& window_;
            Vulkan::VulkanInstance instance_;
            Vulkan::GlfwSurface surface_;
            Vulkan::VulkanDevice device_;
            std::unique_ptr<FileIO::FileIO> fileIO_ = FileIO::createPlatformFileIO();
            std::vector<uint32_t> vertexShader_;
            std::vector<uint32_t> fragmentShader_;
            std::shared_ptr<Vulkan::VulkanBuffer> vertexBuffer_;
            std::shared_ptr<Vulkan::VulkanBuffer> indexBuffer_;
            std::shared_ptr<Vulkan::VulkanImage> checkerTexture_;
            std::unique_ptr<Vulkan::VulkanSwapchain> swapchain_;
            std::unique_ptr<Vulkan::VulkanPipeline> pipeline_;
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
