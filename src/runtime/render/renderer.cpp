#include "runtime/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/detail/renderer_review.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_binding_set.h"
#include "runtime/render/vulkan/vulkan_buffer.h"
#include "runtime/render/vulkan/vulkan_command_list.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_frame_scheduler.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_instance.h"
#include "runtime/render/vulkan/vulkan_material_binding.h"
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
            Vulkan::VulkanObjectData objectData(const glm::mat4& model)
            {
                Vulkan::VulkanObjectData result{model, {glm::vec4(0), glm::vec4(0), glm::vec4(0)}};
                if (model[0][3] != 0 || model[1][3] != 0 || model[2][3] != 0 || model[3][3] != 1)
                    return result;

                // 使用双精度求逆，且只拒绝零行列式，避免将正常的小比例缩放误判为奇异矩阵。
                const glm::dmat3 linear(model);
                const double determinant = glm::determinant(linear);
                if (!std::isfinite(determinant) || determinant == 0)
                    return result;
                const glm::mat3 normal(glm::transpose(glm::inverse(linear)));
                for (int column = 0; column < 3; ++column)
                    for (int row = 0; row < 3; ++row)
                        if (!std::isfinite(normal[column][row]))
                            return result;
                for (int column = 0; column < 3; ++column)
                    result.normalColumns[column] = glm::vec4(normal[column], 0);
                return result;
            }

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

        struct Mesh::Impl
        {
            Impl(const Vulkan::VulkanDevice& device, std::span<const MeshVertex> vertices,
                 std::span<const uint32_t> indices)
                : owner(&device), indexCount(static_cast<uint32_t>(indices.size()))
            {
                vertexBuffer = Vulkan::VulkanBuffer::uploadStatic(
                    device, std::as_bytes(vertices), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
                indexBuffer = Vulkan::VulkanBuffer::uploadStatic(
                    device, std::as_bytes(indices), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                    VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
            }

            const Vulkan::VulkanDevice* owner;
            uint32_t indexCount;
            std::shared_ptr<Vulkan::VulkanBuffer> vertexBuffer;
            std::shared_ptr<Vulkan::VulkanBuffer> indexBuffer;
        };

        struct Texture::Impl
        {
            Impl(const Vulkan::VulkanDevice& device, const Resource::TextureResource& texture, TextureMipMode mipMode)
                : owner(&device)
            {
                image = Vulkan::VulkanImage::uploadRgba8(device, {texture.width(), texture.height()},
                                                         VK_FORMAT_R8G8B8A8_SRGB, texture.pixels(),
                                                         VK_IMAGE_USAGE_SAMPLED_BIT, mipMode);
            }

            const Vulkan::VulkanDevice* owner;
            std::shared_ptr<Vulkan::VulkanImage> image;
        };

        struct Material::Impl
        {
            Impl(const Vulkan::VulkanDevice& device, std::shared_ptr<Texture::Impl> source,
                 VkDescriptorSetLayout layout, const std::array<float, 3>& color, SamplerDesc sampling,
                 ShadingMode shading)
                : owner(&device),
                  texture(std::move(source)),
                  samplerDesc(sampling),
                  sampler(device, sampling, texture->image->mipLevels()),
                  binding(device, layout, *texture->image, sampler, color, shading)
            {}

            const Vulkan::VulkanDevice* owner;
            std::shared_ptr<Texture::Impl> texture;
            const SamplerDesc samplerDesc;
            Vulkan::VulkanSampler sampler;
            Vulkan::VulkanMaterialBinding binding;
        };

        class Renderer::Impl
        {
        public:
            Impl(Window::Window& window, RendererValidationMode validationMode)
                : window_(window),
                  instance_(requiredRendererInstanceExtensions(), toVulkanValidationMode(validationMode)),
                  surface_(instance_, window_),
                  device_(instance_, surface_)
            {
                depthFormat_ = device_.findDepthFormat();
                for (auto& uniformBuffer : uniformBuffers_)
                {
                    uniformBuffer = std::make_shared<Vulkan::VulkanBuffer>(
                        device_, sizeof(Vulkan::VulkanFrameUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                }
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

            Mesh createMesh(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices)
            {
                if (vertices.empty() || indices.empty() || indices.size() % 3 != 0 ||
                    indices.size() > std::numeric_limits<uint32_t>::max())
                    throw Error::Exception(Error::Code::RENDER,
                                           "Mesh requires vertices and complete indexed triangles");
                for (const auto index : indices)
                    if (index >= vertices.size())
                        throw Error::Exception(Error::Code::RENDER, "Mesh index is outside the vertex array");
                std::vector<MeshVertex> uploadVertices(vertices.begin(), vertices.end());
                for (auto& vertex : uploadVertices)
                {
                    for (const float value : vertex.normal)
                        if (!std::isfinite(value))
                            throw Error::Exception(Error::Code::RENDER, "Mesh normal must be finite");
                    // 上传前以双精度长度归一化，避免有限的大法线在 GPU 矩阵乘法中溢出；零法线保持原样。
                    const double length = std::hypot(double(vertex.normal[0]), double(vertex.normal[1]),
                                                     double(vertex.normal[2]));
                    if (length > 0)
                        for (auto& value : vertex.normal)
                            value = static_cast<float>(value / length);
                }
                auto mesh = std::make_shared<Mesh::Impl>(device_, uploadVertices, indices);
                meshes_.push_back(mesh);
                Log::Registry::get("render").debug("Uploaded mesh: {} vertices ({} bytes), {} indices ({} bytes)",
                                                   vertices.size(), vertices.size_bytes(), indices.size(),
                                                   indices.size_bytes());
                return Mesh(mesh);
            }

            Texture createTexture(const Resource::TextureResource& texture, TextureMipMode mipMode)
            {
                auto gpuTexture = std::make_shared<Texture::Impl>(device_, texture, mipMode);
                textures_.push_back(gpuTexture);
                return Texture(gpuTexture);
            }

            void prepareSceneResources(const Resource::ShaderResource& vertexShader,
                                       const Resource::ShaderResource& fragmentShader)
            {
                if (bindingSet_)
                    throw Error::Exception(Error::Code::RENDER, "Scene resources are already prepared");
                if (vertexShader.words().empty() || fragmentShader.words().empty())
                    throw Error::Exception(Error::Code::RENDER, "Scene shaders must contain SPIR-V words");

                auto vertexWords = vertexShader.words();
                auto fragmentWords = fragmentShader.words();
                auto bindingSet = std::make_unique<Vulkan::VulkanBindingSet>(device_, uniformBuffers_);
                std::unique_ptr<Vulkan::VulkanPipeline> pipeline;
                if (swapchain_ && swapchain_->handle() != VK_NULL_HANDLE)
                    pipeline = std::make_unique<Vulkan::VulkanPipeline>(
                        device_, swapchain_->format(), depthFormat_, bindingSet->layout(), bindingSet->materialLayout(),
                        vertexWords, fragmentWords);

                vertexShader_ = std::move(vertexWords);
                fragmentShader_ = std::move(fragmentWords);
                bindingSet_ = std::move(bindingSet);
                pipeline_ = std::move(pipeline);
            }

            Material createMaterial(const MaterialDesc& desc)
            {
                if (!bindingSet_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare scene shaders before creating materials");
                auto texture = desc.texture.impl_.lock();
                if (!texture || texture->owner != &device_)
                    throw Error::Exception(Error::Code::RENDER, "Texture is invalid or belongs to another Renderer");
                for (const auto value : desc.color)
                    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                        throw Error::Exception(Error::Code::RENDER,
                                               "Material color must be finite linear RGB in [0, 1]");
                if (desc.shading != ShadingMode::Unlit && desc.shading != ShadingMode::Lambert)
                    throw Error::Exception(Error::Code::RENDER, "Material shading mode is invalid");
                if (desc.sampler.minFilter == TextureFilter::Linear ||
                    desc.sampler.magFilter == TextureFilter::Linear ||
                    (desc.sampler.mipFilter == MipFilter::Linear && texture->image->mipLevels() > 1))
                {
                    VkFormatProperties properties{};
                    vkGetPhysicalDeviceFormatProperties(device_.physicalDevice(), texture->image->format(),
                                                        &properties);
                    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                        throw Error::Exception(Error::Code::RENDER, "Texture format does not support linear filtering");
                }
                auto material = std::make_shared<Material::Impl>(
                    device_, std::move(texture), bindingSet_->materialLayout(), desc.color, desc.sampler, desc.shading);
                materials_.push_back(material);
                Log::Registry::get("render").debug("Created material: linear RGB ({}, {}, {})", desc.color[0],
                                                   desc.color[1], desc.color[2]);
                return Material(material);
            }

            std::optional<FrameExtent> beginUiFrame()
            {
                if (!prepareFrame())
                    return std::nullopt;
                imguiRenderer_->beginFrame();
                const auto extent = swapchain_->extent();
                return FrameExtent{extent.width, extent.height};
            }

            std::vector<Vulkan::VulkanDrawItem> resolveDraws(const FrameInput& input,
                                                             std::vector<std::shared_ptr<void>>& inFlightResources)
            {
                std::vector<Vulkan::VulkanDrawItem> draws;
                draws.reserve(input.draws.size());
                inFlightResources.reserve(input.draws.size() * 2 + 1);
                for (const auto& draw : input.draws)
                {
                    auto gpuMesh = draw.mesh.impl_.lock();
                    if (!gpuMesh || gpuMesh->owner != &device_)
                        throw Error::Exception(Error::Code::RENDER, "Mesh is invalid or belongs to another Renderer");
                    auto gpuMaterial = draw.material.impl_.lock();
                    if (!gpuMaterial || gpuMaterial->owner != &device_)
                        throw Error::Exception(Error::Code::RENDER,
                                               "Material is invalid or belongs to another Renderer");
                    draws.push_back({gpuMesh->vertexBuffer->handle(), gpuMesh->indexBuffer->handle(),
                                     gpuMesh->indexCount, gpuMaterial->binding.descriptorSet(),
                                     objectData(draw.model)});
                    inFlightResources.push_back(std::move(gpuMesh));
                    inFlightResources.push_back(std::move(gpuMaterial));
                }
                return draws;
            }

            void withReviewFrame(const FrameInput& input, const Detail::RendererReviewAccess::Record& record)
            {
                const auto frame = Vulkan::makeFrameUniform(input);
                std::vector<std::shared_ptr<void>> resources;
                const auto draws = resolveDraws(input, resources);
                if (!pipeline_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare scene shaders before GPU review");
                const std::array uniforms = {std::make_shared<Vulkan::VulkanBuffer>(
                    device_, sizeof(frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
                uniforms[0]->write(0, std::as_bytes(std::span(&frame, 1)));
                Vulkan::VulkanBindingSet bindings(device_, uniforms);
                record(device_, *pipeline_, bindings.descriptorSet(0), draws);
            }

            void drawFrame(const FrameInput& input, const std::array<float, 4>& clearColor,
                           const std::function<void(void*)>& recordUi)
            {
                const auto frame = Vulkan::makeFrameUniform(input);
                std::vector<std::shared_ptr<void>> inFlightResources;
                const auto draws = resolveDraws(input, inFlightResources);
                if (!prepareFrame())
                    return;
                const auto extent = swapchain_->extent();
                if (input.extent != FrameExtent{extent.width, extent.height})
                    return;

                const uint32_t frameIndex = scheduler_->currentFrameIndex();
                scheduler_->waitForCurrentFrame();
                uniformBuffers_[frameIndex]->write(0, std::as_bytes(std::span(&frame, 1)));

                VkClearColorValue color{};
                color.float32[0] = clearColor[0];
                color.float32[1] = clearColor[1];
                color.float32[2] = clearColor[2];
                color.float32[3] = clearColor[3];
                inFlightResources.push_back(uniformBuffers_[frameIndex]);
                if (!scheduler_->renderAndPresent(
                        *swapchain_, color,
                        [&](VkCommandBuffer commandBuffer) {
                            if (!draws.empty())
                                Vulkan::VulkanCommandList::drawIndexed(commandBuffer, *pipeline_,
                                                                       bindingSet_->descriptorSet(frameIndex), draws);
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
                    if (bindingSet_ && (!pipeline_ || previousFormat != swapchain_->format()))
                    {
                        pipeline_ = std::make_unique<Vulkan::VulkanPipeline>(
                            device_, swapchain_->format(), depthFormat_, bindingSet_->layout(),
                            bindingSet_->materialLayout(), vertexShader_, fragmentShader_);
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
            std::vector<std::shared_ptr<Mesh::Impl>> meshes_;
            std::vector<std::shared_ptr<Texture::Impl>> textures_;
            std::array<std::shared_ptr<Vulkan::VulkanBuffer>, Vulkan::VulkanFrameScheduler::FRAME_COUNT>
                uniformBuffers_{};
            std::unique_ptr<Vulkan::VulkanBindingSet> bindingSet_;
            std::vector<std::shared_ptr<Material::Impl>> materials_;
            std::unique_ptr<Vulkan::VulkanSwapchain> swapchain_;
            std::unique_ptr<Vulkan::VulkanPipeline> pipeline_;
            std::unique_ptr<Vulkan::VulkanFrameScheduler> scheduler_;
            std::unique_ptr<Integration::ImGuiRenderer> imguiRenderer_;
            VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
            int requestedWidth_ = 0;
            int requestedHeight_ = 0;
            bool needsRebuild_ = false;
        };

        Renderer::Renderer(Window::Window& window, RendererValidationMode validationMode)
            : impl_(std::make_unique<Impl>(window, validationMode))
        {}

        Renderer::~Renderer() = default;

        Renderer::Renderer(Renderer&&) noexcept = default;

        Renderer& Renderer::operator=(Renderer&&) noexcept = default;

        Mesh Renderer::createMesh(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createMesh(vertices, indices);
        }

        Texture Renderer::createTexture(const Resource::TextureResource& texture, TextureMipMode mipMode)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createTexture(texture, mipMode);
        }

        void Renderer::prepareSceneResources(const Resource::ShaderResource& vertexShader,
                                             const Resource::ShaderResource& fragmentShader)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            impl_->prepareSceneResources(vertexShader, fragmentShader);
        }

        Material Renderer::createMaterial(const MaterialDesc& desc)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createMaterial(desc);
        }

        std::optional<FrameExtent> Renderer::beginUiFrame()
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->beginUiFrame();
        }

        void Detail::RendererReviewAccess::withFrame(Renderer& renderer, const FrameInput& input, const Record& record)
        {
            renderer.impl_->withReviewFrame(input, record);
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
