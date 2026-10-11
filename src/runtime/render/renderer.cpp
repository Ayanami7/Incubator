#include "runtime/render/renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "runtime/core/error/error.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/resource/loader/spirv_reader.h"
#include "runtime/render/vulkan/vulkan_binding_set.h"
#include "runtime/render/vulkan/vulkan_buffer.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_frame_scheduler.h"
#include "runtime/render/vulkan/vulkan_instance.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_pipeline.h"
#include "runtime/render/vulkan/vulkan_sampler.h"
#include "runtime/render/vulkan/vulkan_swapchain.h"

namespace Incubator
{
    namespace Render
    {
        namespace
        {
            struct CubeVertex
            {
                float position[3];
                float uv[2];
            };
            static_assert(sizeof(CubeVertex) == 5 * sizeof(float));

            constexpr std::array<CubeVertex, 24> makeCubeVertices()
            {
                constexpr std::array<std::array<float, 3>, 8> positions = {{{-0.5f, -0.5f, -0.5f},
                                                                              {0.5f, -0.5f, -0.5f},
                                                                              {0.5f, 0.5f, -0.5f},
                                                                              {-0.5f, 0.5f, -0.5f},
                                                                              {-0.5f, -0.5f, 0.5f},
                                                                              {0.5f, -0.5f, 0.5f},
                                                                              {0.5f, 0.5f, 0.5f},
                                                                              {-0.5f, 0.5f, 0.5f}}};
                constexpr std::array<std::array<uint8_t, 4>, 6> faces = {{{4, 5, 6, 7}, {1, 0, 3, 2},
                                                                           {5, 1, 2, 6}, {0, 4, 7, 3},
                                                                           {7, 6, 2, 3}, {0, 1, 5, 4}}};
                constexpr std::array<std::array<float, 2>, 4> uv = {
                    {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};
                std::array<CubeVertex, 24> vertices{};
                for (size_t face = 0; face < faces.size(); ++face)
                {
                    for (size_t corner = 0; corner < 4; ++corner)
                    {
                        const auto& position = positions[faces[face][corner]];
                        vertices[face * 4 + corner] = {{position[0], position[1], position[2]},
                                                       {uv[corner][0], uv[corner][1]}};
                    }
                }
                return vertices;
            }

            constexpr std::array<uint16_t, 36> makeCubeIndices()
            {
                std::array<uint16_t, 36> indices{};
                for (uint16_t face = 0; face < 6; ++face)
                {
                    const uint16_t vertex = face * 4;
                    const size_t index = face * 6;
                    indices[index + 0] = vertex + 0;
                    indices[index + 1] = vertex + 1;
                    indices[index + 2] = vertex + 2;
                    indices[index + 3] = vertex + 0;
                    indices[index + 4] = vertex + 2;
                    indices[index + 5] = vertex + 3;
                }
                return indices;
            }

            constexpr std::array<CubeVertex, 24> CUBE_VERTICES = makeCubeVertices();
            constexpr std::array<uint16_t, 36> CUBE_INDICES = makeCubeIndices();

            constexpr std::array<uint8_t, 8 * 8 * 4> makeCheckerTexture()
            {
                std::array<uint8_t, 8 * 8 * 4> pixels{};
                for (uint32_t y = 0; y < 8; ++y)
                {
                    for (uint32_t x = 0; x < 8; ++x)
                    {
                        const bool light = ((x / 2) + (y / 2)) % 2 == 0;
                        const size_t pixel = (y * 8 + x) * 4;
                        pixels[pixel + 0] = light ? 245 : 30;
                        pixels[pixel + 1] = light ? 210 : 90;
                        pixels[pixel + 2] = light ? 70 : 220;
                        pixels[pixel + 3] = 255;
                    }
                }
                return pixels;
            }

            constexpr auto CHECKER_TEXTURE = makeCheckerTexture();

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
                  vertexShader_(Resource::Loader::readSpirv(*fileIO_, "assets/shaders/cube.vert.spv")),
                  fragmentShader_(Resource::Loader::readSpirv(*fileIO_, "assets/shaders/cube.frag.spv"))
            {
                depthFormat_ = device_.findDepthFormat();
                vertexBuffer_ = Vulkan::VulkanBuffer::uploadStatic(
                    device_, std::as_bytes(std::span<const CubeVertex>(CUBE_VERTICES)),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
                indexBuffer_ = Vulkan::VulkanBuffer::uploadStatic(
                    device_, std::as_bytes(std::span<const uint16_t>(CUBE_INDICES)),
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                    VK_ACCESS_2_INDEX_READ_BIT);
                checkerTexture_ = Vulkan::VulkanImage::uploadRgba8(
                    device_, {8, 8}, VK_FORMAT_R8G8B8A8_SRGB, CHECKER_TEXTURE, VK_IMAGE_USAGE_SAMPLED_BIT);
                sampler_ = std::make_shared<Vulkan::VulkanSampler>(device_);
                for (auto& uniformBuffer : uniformBuffers_)
                {
                    uniformBuffer = std::make_shared<Vulkan::VulkanBuffer>(
                        device_, sizeof(TransformUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                }
                bindingSet_ = std::make_unique<Vulkan::VulkanBindingSet>(
                    device_, uniformBuffers_, *checkerTexture_, *sampler_);
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

                const uint32_t frameIndex = scheduler_->currentFrameIndex();
                scheduler_->waitForCurrentFrame();
                const TransformUniform transforms = makeTransforms(swapchain_->extent());
                uniformBuffers_[frameIndex]->write(0, std::as_bytes(std::span<const TransformUniform>(&transforms, 1)));

                VkClearColorValue color{};
                color.float32[0] = clearColor[0];
                color.float32[1] = clearColor[1];
                color.float32[2] = clearColor[2];
                color.float32[3] = clearColor[3];
                const std::array<std::shared_ptr<void>, 5> inFlightResources = {
                    std::static_pointer_cast<void>(vertexBuffer_), std::static_pointer_cast<void>(indexBuffer_),
                    std::static_pointer_cast<void>(uniformBuffers_[frameIndex]),
                    std::static_pointer_cast<void>(checkerTexture_), std::static_pointer_cast<void>(sampler_)};
                if (!scheduler_->renderAndPresent(*swapchain_, color, *pipeline_, *vertexBuffer_, *indexBuffer_,
                                                  bindingSet_->descriptorSet(frameIndex),
                                                  static_cast<uint32_t>(CUBE_INDICES.size()), inFlightResources))
                    needsRebuild_ = true;
            }

        private:
            TransformUniform makeTransforms(VkExtent2D extent) const
            {
                const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - startTime_)
                                          .count();
                TransformUniform transforms{};
                transforms.model = glm::rotate(glm::mat4(1.0f), seconds * 0.8f, glm::vec3(0.0f, 1.0f, 0.0f));
                transforms.model = glm::rotate(transforms.model, seconds * 0.55f, glm::vec3(1.0f, 0.0f, 0.0f));
                transforms.view = glm::lookAt(glm::vec3(2.5f, 1.9f, 3.2f), glm::vec3(0.0f),
                                              glm::vec3(0.0f, 1.0f, 0.0f));
                transforms.projection = glm::perspectiveRH_ZO(glm::radians(45.0f),
                                                              static_cast<float>(extent.width) / extent.height,
                                                              0.1f, 10.0f);
                transforms.projection[1][1] *= -1.0f;
                return transforms;
            }

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
                    scheduler_->initializeSwapchainImages(swapchain_->imageCount(), swapchain_->extent(), depthFormat_);
                    if (!pipeline_ || previousFormat != swapchain_->format())
                    {
                        pipeline_ = std::make_unique<Vulkan::VulkanPipeline>(
                            device_, swapchain_->format(), depthFormat_, bindingSet_->layout(), vertexShader_,
                            fragmentShader_);
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
            std::array<std::shared_ptr<Vulkan::VulkanBuffer>, Vulkan::VulkanFrameScheduler::FRAME_COUNT>
                uniformBuffers_{};
            std::shared_ptr<Vulkan::VulkanImage> checkerTexture_;
            std::shared_ptr<Vulkan::VulkanSampler> sampler_;
            std::unique_ptr<Vulkan::VulkanBindingSet> bindingSet_;
            std::unique_ptr<Vulkan::VulkanSwapchain> swapchain_;
            std::unique_ptr<Vulkan::VulkanPipeline> pipeline_;
            std::unique_ptr<Vulkan::VulkanFrameScheduler> scheduler_;
            VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
            const std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();
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
