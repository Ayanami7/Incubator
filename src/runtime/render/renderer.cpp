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
#include "runtime/render/vulkan/vulkan_scene_target.h"
#include "runtime/render/vulkan/vulkan_shadow_target.h"
#include "runtime/render/vulkan/vulkan_swapchain.h"
#include "runtime/resource/loader/float_texture_resource.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/texture_resource.h"

namespace Incubator
{
    namespace Render
    {
        struct SceneOutput::Impl
        {
            const Vulkan::VulkanDevice* owner;
            std::unique_ptr<Vulkan::VulkanSceneTarget> target;
        };
        namespace
        {
            Vulkan::VulkanObjectData objectData(const glm::mat4& model)
            {
                Vulkan::VulkanObjectData result{model, {glm::vec4(0), glm::vec4(0), glm::vec4(0)}, glm::vec4(0)};
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
                result.tangentParams.x = determinant > 0.0 ? 1.0f : -1.0f;
                return result;
            }

            std::vector<const char*> requiredRendererInstanceExtensions()
            {
                return Vulkan::GlfwSurface::requiredInstanceExtensions();
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

        struct GpuResources::Impl
        {
            const Vulkan::VulkanDevice* owner;
            std::vector<std::shared_ptr<void>> objects;
        };

        bool GpuResources::valid() const noexcept
        {
            return impl_ && impl_->owner;
        }

        // 直接创建保留强持有；资源组创建仅登记弱引用，供真实存活数量复核。
        template <class T>
        struct ResourceEntry
        {
            std::weak_ptr<T> object;
            std::shared_ptr<T> retained;
        };

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
            Impl(const Vulkan::VulkanDevice& device, const Resource::TextureResource& texture, TextureMipMode mipMode,
                 TextureColorSpace space)
                : owner(&device), colorSpace(space)
            {
                VkFormat format = VK_FORMAT_R8G8B8A8_SRGB;
                switch (colorSpace)
                {
                    case TextureColorSpace::Srgb:
                        format = VK_FORMAT_R8G8B8A8_SRGB;
                        break;
                    case TextureColorSpace::Linear:
                        format = VK_FORMAT_R8G8B8A8_UNORM;
                        break;
                    default:
                        throw Error::Exception(Error::Code::RENDER, "Invalid texture color space");
                }
                image = Vulkan::VulkanImage::uploadRgba8(device, {texture.width(), texture.height()}, format,
                                                         texture.pixels(), VK_IMAGE_USAGE_SAMPLED_BIT, mipMode);
            }

            Impl(const Vulkan::VulkanDevice& device, const Resource::FloatTextureResource& texture)
                : owner(&device), colorSpace(TextureColorSpace::Linear), isFloat(true), isCubemap(texture.isCubemap())
            {
                std::vector<std::span<const uint16_t>> mipPayloads;
                mipPayloads.reserve(texture.mips().size());
                for (const auto& mip : texture.mips())
                    mipPayloads.emplace_back(mip.rgba16f);
                image = Vulkan::VulkanImage::uploadRgba16f(device, {texture.width(), texture.height()},
                                                           texture.faceCount(), mipPayloads,
                                                           VK_IMAGE_USAGE_SAMPLED_BIT);
            }

            const Vulkan::VulkanDevice* owner;
            TextureColorSpace colorSpace = TextureColorSpace::Srgb;
            bool isFloat = false;
            bool isCubemap = false;
            std::shared_ptr<Vulkan::VulkanImage> image;
        };

        struct Material::Impl
        {
            Impl(const Vulkan::VulkanDevice& device, std::shared_ptr<Texture::Impl> source,
                 std::shared_ptr<Texture::Impl> normalSource, std::shared_ptr<Texture::Impl> mrSource,
                 std::shared_ptr<Texture::Impl> emissiveSource,
                 const Vulkan::VulkanBindingSet& bindingSet, const MaterialDesc& desc)
                : owner(&device),
                  texture(std::move(source)),
                  normalTexture(std::move(normalSource)),
                  metallicRoughnessTexture(std::move(mrSource)),
                  emissiveTexture(std::move(emissiveSource)),
                  description(desc),
                  colorSampler(std::make_shared<Vulkan::VulkanSampler>(device, desc.sampler, texture->image->mipLevels())),
                  normalSampler(selectSampler(device, desc.sampler,
                                              normalImage(bindingSet).mipLevels(), colorSampler,
                                              texture->image->mipLevels())),
                  mrSampler(selectSampler(device, desc.sampler,
                                          metallicRoughnessImage(bindingSet).mipLevels(), colorSampler,
                                          texture->image->mipLevels(), normalSampler,
                                          normalImage(bindingSet).mipLevels())),
                  binding(device, bindingSet.materialLayout(), *texture->image, *colorSampler,
                          normalImage(bindingSet), *normalSampler, metallicRoughnessImage(bindingSet), *mrSampler,
                          desc.color, desc.shading, normalTexture ? desc.normalScale : 0.0f, desc.roughness,
                          desc.metallic, desc.roughnessMode, desc.metallicMode)
            {}

            const Vulkan::VulkanImage& normalImage(const Vulkan::VulkanBindingSet& bindingSet) const noexcept
            {
                return normalTexture ? *normalTexture->image : bindingSet.normalFallback();
            }

            const Vulkan::VulkanImage& metallicRoughnessImage(const Vulkan::VulkanBindingSet& bindingSet) const noexcept
            {
                return metallicRoughnessTexture ? *metallicRoughnessTexture->image
                                                : bindingSet.metallicRoughnessFallback();
            }

            static std::shared_ptr<Vulkan::VulkanSampler> selectSampler(
                const Vulkan::VulkanDevice& device, SamplerDesc desc, uint32_t mipLevels,
                const std::shared_ptr<Vulkan::VulkanSampler>& first, uint32_t firstMips,
                const std::shared_ptr<Vulkan::VulkanSampler>& second = {}, uint32_t secondMips = 0)
            {
                if (mipLevels == firstMips)
                    return first;
                if (second && mipLevels == secondMips)
                    return second;
                return std::make_shared<Vulkan::VulkanSampler>(device, desc, mipLevels);
            }

            const Vulkan::VulkanDevice* owner;
            std::shared_ptr<Texture::Impl> texture;
            std::shared_ptr<Texture::Impl> normalTexture;
            std::shared_ptr<Texture::Impl> metallicRoughnessTexture;
            std::shared_ptr<Texture::Impl> emissiveTexture;
            const MaterialDesc description;
            std::shared_ptr<Vulkan::VulkanSampler> colorSampler;
            std::shared_ptr<Vulkan::VulkanSampler> normalSampler;
            std::shared_ptr<Vulkan::VulkanSampler> mrSampler;
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

            ~Impl()
            {
                // 先沿原有关闭路径完成 GPU 工作，再撤销外部组；设备和材质布局此时仍有效。
                imguiRenderer_.reset();
                scheduler_.reset();
                for (auto& entry : resourceGroups_)
                    if (auto group = entry.lock())
                    {
                        group->objects.clear();
                        group->owner = nullptr;
                    }
            }

            GpuResources createResources()
            {
                auto group = std::make_shared<GpuResources::Impl>();
                group->owner = &device_;
                std::erase_if(resourceGroups_, [](const auto& value) { return value.expired(); });
                resourceGroups_.push_back(group);
                return GpuResources(std::move(group));
            }

            const std::shared_ptr<GpuResources::Impl>& checkResources(const GpuResources& resources) const
            {
                if (!resources.impl_ || resources.impl_->owner != &device_)
                    throw Error::Exception(Error::Code::RENDER,
                                           "GPU resources are invalid or belong to another Renderer");
                return resources.impl_;
            }

            Detail::RendererReviewAccess::ResourceCounts resourceCounts() const
            {
                const auto live = [](const auto& entries) {
                    return size_t(
                        std::ranges::count_if(entries, [](const auto& entry) { return !entry.object.expired(); }));
                };
                return {live(meshes_), live(textures_), live(materials_)};
            }

            void waitForFrames()
            {
                scheduler_->waitForFrames();
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

            Mesh createMesh(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices,
                            const std::shared_ptr<GpuResources::Impl>& resources = {})
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
                    const double length =
                        std::hypot(double(vertex.normal[0]), double(vertex.normal[1]), double(vertex.normal[2]));
                    if (length > 0)
                        for (auto& value : vertex.normal)
                            value = static_cast<float>(value / length);

                    for (const float value : vertex.tangent)
                        if (!std::isfinite(value))
                            throw Error::Exception(Error::Code::RENDER, "Mesh tangent must be finite");
                    const double tangentLength =
                        std::hypot(double(vertex.tangent[0]), double(vertex.tangent[1]), double(vertex.tangent[2]));
                    if (tangentLength > 0.0 && vertex.tangent[3] != 0.0f)
                    {
                        for (size_t i = 0; i < 3; ++i)
                            vertex.tangent[i] = static_cast<float>(vertex.tangent[i] / tangentLength);
                        vertex.tangent[3] = vertex.tangent[3] < 0.0f ? -1.0f : 1.0f;
                    }
                    else
                    {
                        for (float& value : vertex.tangent)
                            value = 0.0f;
                    }
                }
                auto mesh = std::make_shared<Mesh::Impl>(device_, uploadVertices, indices);
                std::erase_if(meshes_, [](const auto& entry) { return entry.object.expired(); });
                meshes_.push_back({mesh, resources ? nullptr : mesh});
                if (resources)
                    resources->objects.push_back(mesh);
                Log::Registry::get("render").debug("Uploaded mesh: {} vertices ({} bytes), {} indices ({} bytes)",
                                                   vertices.size(), vertices.size_bytes(), indices.size(),
                                                   indices.size_bytes());
                return Mesh(mesh);
            }

            Texture createTexture(const Resource::TextureResource& texture, TextureMipMode mipMode,
                                  TextureColorSpace colorSpace,
                                  const std::shared_ptr<GpuResources::Impl>& resources = {})
            {
                auto gpuTexture = std::make_shared<Texture::Impl>(device_, texture, mipMode, colorSpace);
                std::erase_if(textures_, [](const auto& entry) { return entry.object.expired(); });
                textures_.push_back({gpuTexture, resources ? nullptr : gpuTexture});
                if (resources)
                    resources->objects.push_back(gpuTexture);
                return Texture(gpuTexture);
            }

            Texture createTexture(const Resource::FloatTextureResource& texture,
                                  const std::shared_ptr<GpuResources::Impl>& resources = {})
            {
                auto gpuTexture = std::make_shared<Texture::Impl>(device_, texture);
                std::erase_if(textures_, [](const auto& entry) { return entry.object.expired(); });
                textures_.push_back({gpuTexture, resources ? nullptr : gpuTexture});
                if (resources)
                    resources->objects.push_back(gpuTexture);
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

            void prepareShadowResources(const Resource::ShaderResource& vertexShader)
            {
                if (!bindingSet_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare scene resources before shadow resources");
                if (!shadowVertexShader_.empty())
                    throw Error::Exception(Error::Code::RENDER, "Shadow resources are already prepared");
                if (vertexShader.words().empty())
                    throw Error::Exception(Error::Code::RENDER, "Shadow vertex shader must contain SPIR-V words");
                shadowVertexShader_ = vertexShader.words();
            }

            void prepareDisplayResources(const Resource::ShaderResource& vertexShader,
                                         const Resource::ShaderResource& fragmentShader)
            {
                if (displayPass_)
                    throw Error::Exception(Error::Code::RENDER, "Display resources are already prepared");
                if (vertexShader.words().empty() || fragmentShader.words().empty())
                    throw Error::Exception(Error::Code::RENDER, "Display shaders must contain SPIR-V words");
                auto vertexWords = vertexShader.words();
                displayPass_ = std::make_unique<Vulkan::VulkanDisplayPass>(
                    device_, VK_FORMAT_R8G8B8A8_UNORM, vertexWords, fragmentShader.words());
                displayVertexShader_ = std::move(vertexWords);
            }

            void prepareFxaaResources(const Resource::ShaderResource& fragmentShader)
            {
                if (!displayPass_ || displayVertexShader_.empty())
                    throw Error::Exception(Error::Code::RENDER, "Prepare display resources before FXAA resources");
                if (fxaaPass_)
                    throw Error::Exception(Error::Code::RENDER, "FXAA resources are already prepared");
                if (fragmentShader.words().empty())
                    throw Error::Exception(Error::Code::RENDER, "FXAA fragment shader must contain SPIR-V words");
                fxaaPass_ = std::make_unique<Vulkan::VulkanDisplayPass>(
                    device_, VK_FORMAT_R8G8B8A8_UNORM, displayVertexShader_, fragmentShader.words(),
                    SamplerDesc{TextureFilter::Linear, TextureFilter::Linear, MipFilter::BaseLevel},
                    VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0);
            }

            void prepareIblResources(const Texture& irradiance, const Texture& prefiltered, const Texture& brdfLut)
            {
                if (!bindingSet_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare scene resources before IBL resources");
                if (iblIrradiance_)
                    throw Error::Exception(Error::Code::RENDER, "IBL resources are already prepared");
                const auto resolveFloatTexture = [&](const Texture& handle, bool expectCubemap, const char* name) {
                    auto resolved = handle.impl_.lock();
                    if (!resolved || resolved->owner != &device_)
                        throw Error::Exception(
                            Error::Code::RENDER,
                            fmt::format("IBL {} texture is invalid or belongs to another Renderer", name));
                    if (!resolved->isFloat)
                        throw Error::Exception(
                            Error::Code::RENDER,
                            fmt::format("IBL {} texture must use linear RGBA16F float format", name));
                    if (resolved->isCubemap != expectCubemap)
                        throw Error::Exception(
                            Error::Code::RENDER,
                            fmt::format("IBL {} texture dimensionality mismatch", name));
                    return resolved;
                };
                auto irr = resolveFloatTexture(irradiance, true, "irradiance");
                auto pre = resolveFloatTexture(prefiltered, true, "prefiltered");
                auto lut = resolveFloatTexture(brdfLut, false, "BRDF LUT");

                scheduler_->waitForFrames();
                iblIrradiance_ = std::move(irr);
                iblPrefiltered_ = std::move(pre);
                iblBrdfLut_ = std::move(lut);
                iblMaxPrefilteredLod_ = static_cast<float>(iblPrefiltered_->image->mipLevels() - 1u);
                bindingSet_->setIblImages(iblIrradiance_->image.get(), iblPrefiltered_->image.get(),
                                          iblBrdfLut_->image.get());
            }

            Material createMaterial(const MaterialDesc& desc, const std::shared_ptr<GpuResources::Impl>& resources = {})
            {
                if (!bindingSet_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare scene shaders before creating materials");
                auto texture = desc.texture.impl_.lock();
                if (!texture || texture->owner != &device_)
                    throw Error::Exception(Error::Code::RENDER, "Texture is invalid or belongs to another Renderer");
                if (texture->isFloat || texture->isCubemap)
                    throw Error::Exception(Error::Code::RENDER, "Material base color texture must be a 2D RGBA8 texture");
                if (texture->colorSpace != TextureColorSpace::Srgb)
                    throw Error::Exception(Error::Code::RENDER, "Material base color texture must use Srgb color space");

                const auto resolveOptionalTexture = [&](const Texture& handle, TextureColorSpace colorSpace,
                                                        std::string_view name) {
                    std::shared_ptr<Texture::Impl> resolved;
                    if (!handle.isUnset())
                    {
                        resolved = handle.impl_.lock();
                        if (!resolved || resolved->owner != &device_)
                            throw Error::Exception(
                                Error::Code::RENDER,
                                fmt::format("{} texture is invalid or belongs to another Renderer", name));
                        if (resolved->isFloat || resolved->isCubemap)
                            throw Error::Exception(
                                Error::Code::RENDER,
                                fmt::format("{} texture must be a 2D RGBA8 texture", name));
                        if (resolved->colorSpace != colorSpace)
                            throw Error::Exception(
                                Error::Code::RENDER,
                                fmt::format("{} texture must use {} color space", name,
                                            colorSpace == TextureColorSpace::Srgb ? "Srgb" : "Linear"));
                    }
                    return resolved;
                };
                auto normalTexture = resolveOptionalTexture(desc.normalTexture, TextureColorSpace::Linear, "Normal");
                auto mrTexture = resolveOptionalTexture(desc.metallicRoughnessTexture, TextureColorSpace::Linear,
                                                        "Metallic-roughness");
                auto emissiveTexture = resolveOptionalTexture(desc.emissiveTexture, TextureColorSpace::Srgb, "Emissive");

                for (const auto value : desc.color)
                    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                        throw Error::Exception(Error::Code::RENDER,
                                               "Material color must be finite linear RGB in [0, 1]");
                if (!std::isfinite(desc.normalScale) || desc.normalScale < 0.0f)
                    throw Error::Exception(Error::Code::RENDER,
                                           "Material normal scale must be finite and nonnegative");
                if (!std::isfinite(desc.metallic) || desc.metallic < 0.0f || desc.metallic > 1.0f)
                    throw Error::Exception(Error::Code::RENDER, "Material metallic factor must be finite in [0, 1]");
                if (!std::isfinite(desc.roughness) || desc.roughness < 0.0f || desc.roughness > 1.0f)
                    throw Error::Exception(Error::Code::RENDER, "Material roughness factor must be finite in [0, 1]");
                if (desc.alphaMode != AlphaMode::Opaque && desc.alphaMode != AlphaMode::Mask &&
                    desc.alphaMode != AlphaMode::Blend)
                    throw Error::Exception(Error::Code::RENDER, "Material alphaMode is invalid");
                if (!std::isfinite(desc.alphaCutoff) || desc.alphaCutoff < 0.0f)
                    throw Error::Exception(Error::Code::RENDER, "Material alphaCutoff must be finite and nonnegative");
                if (!std::isfinite(desc.baseAlpha) || desc.baseAlpha < 0.0f || desc.baseAlpha > 1.0f)
                    throw Error::Exception(Error::Code::RENDER, "Material baseAlpha must be finite in [0, 1]");
                for (const auto value : desc.emissiveFactor)
                    if (!std::isfinite(value) || value < 0.0f || value > 1.0f)
                        throw Error::Exception(Error::Code::RENDER, "Material emissiveFactor must be finite in [0, 1]");
                if (desc.shading != ShadingMode::Unlit && desc.shading != ShadingMode::Lambert &&
                    desc.shading != ShadingMode::Pbr)
                    throw Error::Exception(Error::Code::RENDER, "Material shading mode is invalid");
                if ((desc.roughnessMode != MaterialParameterMode::TextureFactor &&
                     desc.roughnessMode != MaterialParameterMode::FixedValue) ||
                    (desc.metallicMode != MaterialParameterMode::TextureFactor &&
                     desc.metallicMode != MaterialParameterMode::FixedValue))
                    throw Error::Exception(Error::Code::RENDER, "Material parameter mode is invalid");

                const auto checkLinearFiltering = [&](const Vulkan::VulkanImage& image) {
                    if (desc.sampler.minFilter == TextureFilter::Linear ||
                        desc.sampler.magFilter == TextureFilter::Linear ||
                        (desc.sampler.mipFilter == MipFilter::Linear && image.mipLevels() > 1))
                    {
                        VkFormatProperties properties{};
                        vkGetPhysicalDeviceFormatProperties(device_.physicalDevice(), image.format(), &properties);
                        if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
                            throw Error::Exception(Error::Code::RENDER,
                                                   "Texture format does not support linear filtering");
                    }
                };
                checkLinearFiltering(*texture->image);
                checkLinearFiltering(normalTexture ? *normalTexture->image : bindingSet_->normalFallback());
                checkLinearFiltering(mrTexture ? *mrTexture->image : bindingSet_->metallicRoughnessFallback());
                if (emissiveTexture)
                    checkLinearFiltering(*emissiveTexture->image);

                auto material = std::make_shared<Material::Impl>(device_, std::move(texture), std::move(normalTexture),
                                                                 std::move(mrTexture), std::move(emissiveTexture),
                                                                 *bindingSet_, desc);
                std::erase_if(materials_, [](const auto& entry) { return entry.object.expired(); });
                materials_.push_back({material, resources ? nullptr : material});
                if (resources)
                    resources->objects.push_back(material);
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

            std::vector<Vulkan::VulkanShadowDrawItem> resolveShadowDraws(const FrameInput& input,
                                                                         std::vector<std::shared_ptr<void>>& resources)
            {
                std::vector<Vulkan::VulkanShadowDrawItem> draws;
                draws.reserve(input.shadowDraws.size());
                for (const auto& draw : input.shadowDraws)
                {
                    auto mesh = draw.mesh.impl_.lock();
                    if (!mesh || mesh->owner != &device_)
                        throw Error::Exception(Error::Code::RENDER,
                                               "Shadow mesh is invalid or belongs to another Renderer");
                    draws.push_back(
                        {mesh->vertexBuffer->handle(), mesh->indexBuffer->handle(), mesh->indexCount, draw.model});
                    resources.push_back(std::move(mesh));
                }
                return draws;
            }

            void withReviewFrame(const FrameInput& input, const Detail::RendererReviewAccess::Record& record)
            {
                if (input.shadow)
                    throw Error::Exception(Error::Code::RENDER,
                                           "Shadow review requires the independent scene target path");
                if (input.iblEnabled && !iblIrradiance_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare IBL resources before enabling IBL");
                const auto frame = Vulkan::makeFrameUniform(input, iblMaxPrefilteredLod_);
                std::vector<std::shared_ptr<void>> resources;
                const auto draws = resolveDraws(input, resources);
                if (!pipeline_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare scene shaders before GPU review");
                const std::array uniforms = {std::make_shared<Vulkan::VulkanBuffer>(
                    device_, sizeof(frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
                uniforms[0]->write(0, std::as_bytes(std::span(&frame, 1)));
                Vulkan::VulkanBindingSet bindings(device_, uniforms);
                if (iblIrradiance_)
                    bindings.setIblImages(iblIrradiance_->image.get(), iblPrefiltered_->image.get(),
                                          iblBrdfLut_->image.get());
                record(device_, *pipeline_, bindings.descriptorSet(0), draws);
            }

            void withHdrSceneTarget(
                const FrameInput& input, const std::array<float, 4>& clearColor,
                const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
            {
                if (!bindingSet_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare scene shaders before GPU review");
                if (input.iblEnabled && !iblIrradiance_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare IBL resources before enabling IBL");
                const auto frame = Vulkan::makeFrameUniform(input, iblMaxPrefilteredLod_);
                std::vector<std::shared_ptr<void>> resources;
                const auto draws = resolveDraws(input, resources);
                std::vector<Vulkan::VulkanShadowDrawItem> shadowDraws;
                std::unique_ptr<Vulkan::VulkanShadowTarget> shadowTarget;
                if (input.shadow)
                {
                    if (shadowVertexShader_.empty())
                        throw Error::Exception(Error::Code::RENDER,
                                               "Prepare shadow vertex shader before enabling shadows");
                    shadowDraws = resolveShadowDraws(input, resources);
                    shadowTarget = std::make_unique<Vulkan::VulkanShadowTarget>(device_);
                    if (!shadowPipeline_)
                        shadowPipeline_ = std::make_unique<Vulkan::VulkanPipeline>(
                            device_, VK_FORMAT_UNDEFINED, shadowTarget->image().format(), bindingSet_->layout(),
                            VK_NULL_HANDLE, shadowVertexShader_, std::vector<uint32_t>{});
                }
                if (!sceneHdrPipeline_)
                    sceneHdrPipeline_ = std::make_unique<Vulkan::VulkanPipeline>(
                        device_, VK_FORMAT_R16G16B16A16_SFLOAT, depthFormat_, bindingSet_->layout(),
                        bindingSet_->materialLayout(), vertexShader_, fragmentShader_, true);
                Vulkan::VulkanSceneTarget hdrTarget(device_, {input.extent.width, input.extent.height}, depthFormat_,
                                                    VK_FORMAT_R16G16B16A16_SFLOAT);
                const std::array uniforms = {std::make_shared<Vulkan::VulkanBuffer>(
                    device_, sizeof(frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
                uniforms[0]->write(0, std::as_bytes(std::span(&frame, 1)));
                Vulkan::VulkanBindingSet bindings(device_, uniforms);
                bindings.setShadowImage(0, shadowTarget ? &shadowTarget->image() : nullptr);
                if (iblIrradiance_)
                    bindings.setIblImages(iblIrradiance_->image.get(), iblPrefiltered_->image.get(),
                                          iblBrdfLut_->image.get());
                VkClearColorValue clear{};
                std::copy(clearColor.begin(), clearColor.end(), clear.float32);
                device_.executeImmediate([&](VkCommandBuffer cmd) {
                    if (shadowTarget)
                        shadowTarget->record(cmd, [&](VkCommandBuffer depth) {
                            Vulkan::VulkanCommandList::drawShadow(depth, *shadowPipeline_, bindings.descriptorSet(0),
                                                                  shadowDraws);
                        });
                    hdrTarget.record(cmd, clear, [&](VkCommandBuffer scene) {
                        if (!draws.empty())
                            Vulkan::VulkanCommandList::drawIndexed(scene, *sceneHdrPipeline_, bindings.descriptorSet(0),
                                                                   draws);
                    });
                });
                record(device_, hdrTarget.color());
            }

            void drawFrame(const FrameInput& input, const std::array<float, 4>& clearColor,
                           const std::function<void(void*)>& recordUi)
            {
                if (input.iblEnabled && !iblIrradiance_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare IBL resources before enabling IBL");
                const auto frame = Vulkan::makeFrameUniform(input, iblMaxPrefilteredLod_);
                if (input.shadow)
                    throw Error::Exception(Error::Code::RENDER,
                                           "Shadows require drawSceneTarget; drawFrame does not support shadows");
                std::vector<std::shared_ptr<void>> inFlightResources;
                const auto draws = resolveDraws(input, inFlightResources);
                if (recordUi)
                {
                    int width, height;
                    window_.getFramebufferSize(width, height);
                    // UI 数据中的描述符只属于当前后端，重建延后到下一轮 UI 开始前。
                    if (needsRebuild_ || width != requestedWidth_ || height != requestedHeight_)
                        return;
                }
                if (!prepareFrame())
                    return;
                const auto extent = swapchain_->extent();
                if (input.extent != FrameExtent{extent.width, extent.height})
                    return;

                const uint32_t frameIndex = scheduler_->currentFrameIndex();
                scheduler_->waitForCurrentFrame();
                uniformBuffers_[frameIndex]->write(0, std::as_bytes(std::span(&frame, 1)));
                if (bindingSet_)
                    bindingSet_->setShadowImage(frameIndex);

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

            void reviewOutput(
                const SceneOutput& output,
                const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
            {
                const auto target = output.impl_.lock();
                if (!target || target->owner != &device_)
                    throw Error::Exception(Error::Code::RENDER, "Invalid scene output");
                scheduler_->waitForFrames();
                record(device_, target->target->color(output.fxaaEnabled()));
            }
            void reviewShadowOutput(
                const SceneOutput& output,
                const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
            {
                const auto target = output.impl_.lock();
                const auto found = std::find(sceneTargets_.begin(), sceneTargets_.end(), target);
                if (!target || target->owner != &device_ || found == sceneTargets_.end())
                    throw Error::Exception(Error::Code::RENDER, "Invalid scene output for shadow review");
                const auto& shadow = shadowTargets_[size_t(found - sceneTargets_.begin())];
                if (!shadow)
                    throw Error::Exception(Error::Code::RENDER, "No shadow target was prepared for this frame slot");
                scheduler_->waitForFrames();
                record(device_, shadow->image());
            }

            SceneOutput prepareSceneTarget(FrameExtent extent, bool fxaa = false)
            {
                SceneOutput output;
                if (!extent.width || !extent.height)
                    return output;
                scheduler_->waitForCurrentFrame();
                const auto frameIndex = scheduler_->currentFrameIndex();
                auto& slot = sceneTargets_[frameIndex];
                if (!slot || slot->target->color().extent().width != extent.width ||
                    slot->target->color().extent().height != extent.height)
                {
                    const auto oldExtent = slot ? slot->target->color().extent() : VkExtent2D{};
                    auto next = std::make_shared<SceneOutput::Impl>();
                    next->owner = &device_;
                    next->target = std::make_unique<Vulkan::VulkanSceneTarget>(
                        device_, VkExtent2D{extent.width, extent.height}, depthFormat_,
                        VK_FORMAT_R16G16B16A16_SFLOAT, true);
                    imguiRenderer_->releaseSceneImage(frameIndex);
                    slot = std::move(next);
                    Log::Registry::get("render").debug(
                        "Scene target {}: slot {}, {}x{} -> {}x{}, color format {}, depth format {}",
                        oldExtent.width ? "resized" : "created", frameIndex, oldExtent.width, oldExtent.height,
                        extent.width, extent.height, static_cast<int>(slot->target->color().format()),
                        static_cast<int>(depthFormat_));
                }
                if (fxaa)
                    slot->target->ensureFxaaTarget();
                output.impl_ = slot;
                output.extent_ = extent;
                output.fxaaEnabled_ = fxaa;
                return output;
            }
            uint64_t sceneImage(const SceneOutput& output)
            {
                const auto target = output.impl_.lock();
                const auto slot = scheduler_->currentFrameIndex();
                if (!target || target->owner != &device_ || target != sceneTargets_[slot])
                    throw Error::Exception(Error::Code::RENDER, "Invalid current scene output");
                return imguiRenderer_->sceneImage(slot, target->target->color(output.fxaaEnabled()).view(),
                                                  output.fxaaEnabled());
            }
            void drawSceneTarget(const SceneOutput& output, const FrameInput& input,
                                 const std::array<float, 4>& clearColor, const std::function<void(void*)>& ui)
            {
                auto target = output.impl_.lock();
                const auto index = scheduler_->currentFrameIndex();
                if (!target || target->owner != &device_ || target != sceneTargets_[index] ||
                    output.extent() != input.extent)
                    throw Error::Exception(Error::Code::RENDER, "Invalid current scene output");
                if (!displayPass_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare display shaders before drawSceneTarget");
                if (output.fxaaEnabled() && !fxaaPass_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare FXAA resources before drawSceneTarget with FXAA");
                if (input.display.toneMapping != ToneMappingMode::Off &&
                    input.display.toneMapping != ToneMappingMode::Reinhard)
                    throw Error::Exception(Error::Code::RENDER, "Invalid tone mapping mode");
                if (!std::isfinite(input.display.exposureEv) ||
                    input.display.exposureEv < DisplaySettings::MIN_EXPOSURE_EV ||
                    input.display.exposureEv > DisplaySettings::MAX_EXPOSURE_EV)
                    throw Error::Exception(Error::Code::RENDER, "Exposure EV is out of range");
                if (input.iblEnabled && !iblIrradiance_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare IBL resources before enabling IBL");
                const auto frame = Vulkan::makeFrameUniform(input, iblMaxPrefilteredLod_);
                std::vector<std::shared_ptr<void>> resources;
                const auto draws = resolveDraws(input, resources);
                if (!draws.empty() && !bindingSet_)
                    throw Error::Exception(Error::Code::RENDER, "Prepare scene shaders first");
                std::vector<Vulkan::VulkanShadowDrawItem> shadowDraws;
                if (input.shadow)
                {
                    if (shadowVertexShader_.empty())
                        throw Error::Exception(Error::Code::RENDER,
                                               "Prepare shadow vertex shader before enabling shadows");
                    shadowDraws = resolveShadowDraws(input, resources);
                }
                int width, height;
                window_.getFramebufferSize(width, height);
                // UI 命令已生成；尺寸变化时丢弃本帧，下轮开始前再重建后端及绑定。
                if (needsRebuild_ || width != requestedWidth_ || height != requestedHeight_)
                    return;
                scheduler_->waitForCurrentFrame();
                if (bindingSet_)
                    uniformBuffers_[index]->write(0, std::as_bytes(std::span(&frame, 1)));
                if (!draws.empty() && !sceneHdrPipeline_)
                    sceneHdrPipeline_ = std::make_unique<Vulkan::VulkanPipeline>(
                        device_, VK_FORMAT_R16G16B16A16_SFLOAT, depthFormat_, bindingSet_->layout(),
                        bindingSet_->materialLayout(), vertexShader_, fragmentShader_, true);
                std::shared_ptr<Vulkan::VulkanShadowTarget> shadow;
                if (input.shadow)
                {
                    auto& slot = shadowTargets_[index];
                    if (!slot)
                        slot = std::make_shared<Vulkan::VulkanShadowTarget>(device_);
                    shadow = slot;
                    if (!shadowPipeline_)
                        shadowPipeline_ = std::make_unique<Vulkan::VulkanPipeline>(
                            device_, VK_FORMAT_UNDEFINED, shadow->image().format(), bindingSet_->layout(),
                            VK_NULL_HANDLE, shadowVertexShader_, std::vector<uint32_t>{});
                    resources.push_back(shadow);
                }
                if (bindingSet_)
                    bindingSet_->setShadowImage(index, shadow ? &shadow->image() : nullptr);
                resources.push_back(target);
                resources.push_back(uniformBuffers_[index]);
                VkClearColorValue clear{};
                std::copy(clearColor.begin(), clearColor.end(), clear.float32);
                const Vulkan::VulkanDisplayParams displayParams{
                    clearColor,
                    input.display.toneMapping == ToneMappingMode::Off ? 0u : 1u,
                    std::exp2(input.display.exposureEv)};
                const Vulkan::ViewportSampleInput sampleInput{
                    static_cast<uint32_t>(draws.size()),
                    static_cast<uint32_t>(shadow ? shadowDraws.size() : 0)};
                if (!scheduler_->renderAndPresent(
                        *swapchain_, clear,
                        [&](VkCommandBuffer cmd) {
                            if (ui)
                                ui(static_cast<void*>(cmd));
                        },
                        resources,
                        [&](VkCommandBuffer cmd) {
                            if (shadow)
                                shadow->record(cmd, [&](VkCommandBuffer depth) {
                                    Vulkan::VulkanCommandList::drawShadow(
                                        depth, *shadowPipeline_, bindingSet_->descriptorSet(index), shadowDraws);
                                });
                            target->target->record(cmd, *displayPass_, displayParams,
                                                   [&](VkCommandBuffer scene) {
                                                       if (!draws.empty())
                                                           Vulkan::VulkanCommandList::drawIndexed(
                                                               scene, *sceneHdrPipeline_,
                                                               bindingSet_->descriptorSet(index), draws);
                                                   },
                                                   output.fxaaEnabled() ? fxaaPass_.get() : nullptr);
                        },
                        sampleInput))
                    needsRebuild_ = true;
            }

            ViewportStats lastViewportStats() const noexcept
            {
                ViewportStats stats;
                stats.gpuTimestampAvailable = scheduler_->supportsTimestamps();
                if (const auto& sample = scheduler_->lastCompletedViewportSample())
                {
                    stats.hasCompletedSample = true;
                    stats.gpuTimeMs = sample->gpuTimeMs;
                    stats.sceneDrawCount = sample->sceneDrawCount;
                    stats.shadowDrawCount = sample->shadowDrawCount;
                }
                return stats;
            }

            uint64_t completedViewportSampleCount() const noexcept
            {
                return scheduler_->completedViewportSampleCount();
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
            std::vector<uint32_t> shadowVertexShader_;
            std::vector<uint32_t> displayVertexShader_;
            std::vector<ResourceEntry<Mesh::Impl>> meshes_;
            std::vector<ResourceEntry<Texture::Impl>> textures_;
            // 会话共享 IBL 纹理先声明，保证其存活至帧绑定描述符释放之后。
            std::shared_ptr<Texture::Impl> iblIrradiance_;
            std::shared_ptr<Texture::Impl> iblPrefiltered_;
            std::shared_ptr<Texture::Impl> iblBrdfLut_;
            float iblMaxPrefilteredLod_ = 0.0f;
            std::array<std::shared_ptr<Vulkan::VulkanBuffer>, Vulkan::VulkanFrameScheduler::FRAME_COUNT>
                uniformBuffers_{};
            // 阴影图先声明，保证其存活至帧绑定描述符释放之后。
            std::array<std::shared_ptr<Vulkan::VulkanShadowTarget>, Vulkan::VulkanFrameScheduler::FRAME_COUNT>
                shadowTargets_{};
            std::unique_ptr<Vulkan::VulkanBindingSet> bindingSet_;
            std::vector<ResourceEntry<Material::Impl>> materials_;
            std::vector<std::weak_ptr<GpuResources::Impl>> resourceGroups_;
            std::unique_ptr<Vulkan::VulkanSwapchain> swapchain_;
            std::unique_ptr<Vulkan::VulkanPipeline> pipeline_;
            std::array<std::shared_ptr<SceneOutput::Impl>, Vulkan::VulkanFrameScheduler::FRAME_COUNT> sceneTargets_{};
            std::unique_ptr<Vulkan::VulkanPipeline> sceneHdrPipeline_;
            std::unique_ptr<Vulkan::VulkanDisplayPass> displayPass_;
            std::unique_ptr<Vulkan::VulkanDisplayPass> fxaaPass_;
            std::unique_ptr<Vulkan::VulkanPipeline> shadowPipeline_;
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

        GpuResources Renderer::createResources()
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createResources();
        }

        Mesh Renderer::createMesh(GpuResources& resources, std::span<const MeshVertex> vertices,
                                  std::span<const uint32_t> indices)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createMesh(vertices, indices, impl_->checkResources(resources));
        }

        Texture Renderer::createTexture(GpuResources& resources, const Resource::TextureResource& texture,
                                        TextureMipMode mipMode, TextureColorSpace colorSpace)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createTexture(texture, mipMode, colorSpace, impl_->checkResources(resources));
        }

        Texture Renderer::createTexture(const Resource::FloatTextureResource& texture)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createTexture(texture);
        }

        Texture Renderer::createTexture(GpuResources& resources, const Resource::FloatTextureResource& texture)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createTexture(texture, impl_->checkResources(resources));
        }

        Material Renderer::createMaterial(GpuResources& resources, const MaterialDesc& desc)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createMaterial(desc, impl_->checkResources(resources));
        }

        Detail::RendererReviewAccess::ResourceCounts Detail::RendererReviewAccess::resourceCounts(Renderer& renderer)
        {
            return renderer.impl_->resourceCounts();
        }

        MaterialDesc Detail::RendererReviewAccess::materialDescription(const Material& material)
        {
            const auto impl = material.impl_.lock();
            if (!impl)
                throw Error::Exception(Error::Code::RENDER, "Cannot inspect an expired material");
            return impl->description;
        }

        void Detail::RendererReviewAccess::waitForFrames(Renderer& renderer)
        {
            renderer.impl_->waitForFrames();
        }

        Mesh Renderer::createMesh(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createMesh(vertices, indices);
        }

        Texture Renderer::createTexture(const Resource::TextureResource& texture, TextureMipMode mipMode,
                                        TextureColorSpace colorSpace)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            return impl_->createTexture(texture, mipMode, colorSpace);
        }

        void Renderer::prepareSceneResources(const Resource::ShaderResource& vertexShader,
                                             const Resource::ShaderResource& fragmentShader)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            impl_->prepareSceneResources(vertexShader, fragmentShader);
        }

        void Renderer::prepareShadowResources(const Resource::ShaderResource& vertexShader)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            impl_->prepareShadowResources(vertexShader);
        }

        void Renderer::prepareDisplayResources(const Resource::ShaderResource& vertexShader,
                                               const Resource::ShaderResource& fragmentShader)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            impl_->prepareDisplayResources(vertexShader, fragmentShader);
        }

        void Renderer::prepareFxaaResources(const Resource::ShaderResource& fragmentShader)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            impl_->prepareFxaaResources(fragmentShader);
        }

        void Renderer::prepareIblResources(const Texture& irradiance, const Texture& prefiltered,
                                           const Texture& brdfLut)
        {
            if (!impl_)
                throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
            impl_->prepareIblResources(irradiance, prefiltered, brdfLut);
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

namespace Incubator::Render
{
    SceneOutput Renderer::prepareSceneTarget(FrameExtent extent, bool fxaa)
    {
        if (!impl_)
            throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
        return impl_->prepareSceneTarget(extent, fxaa);
    }
    void Renderer::drawSceneTarget(const SceneOutput& output, const FrameInput& input,
                                   const std::array<float, 4>& color, const std::function<void(void*)>& ui)
    {
        if (!impl_)
            throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
        impl_->drawSceneTarget(output, input, color, ui);
    }
}  // namespace Incubator::Render

void Incubator::Render::Detail::RendererReviewAccess::withSceneOutput(
    Renderer& renderer, const SceneOutput& output,
    const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
{
    renderer.impl_->reviewOutput(output, record);
}

uint64_t Incubator::Render::Renderer::sceneImage(const SceneOutput& output)
{
    if (!impl_)
        throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
    return impl_->sceneImage(output);
}

Incubator::Render::ViewportStats Incubator::Render::Renderer::lastViewportStats() const noexcept
{
    if (!impl_)
        return {};
    return impl_->lastViewportStats();
}

uint64_t Incubator::Render::Detail::RendererReviewAccess::completedViewportSampleCount(Renderer& renderer)
{
    if (!renderer.impl_)
        throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
    return renderer.impl_->completedViewportSampleCount();
}

void Incubator::Render::Detail::RendererReviewAccess::withShadowOutput(
    Renderer& renderer, const SceneOutput& output,
    const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
{
    renderer.impl_->reviewShadowOutput(output, record);
}

void Incubator::Render::Detail::RendererReviewAccess::withHdrSceneTarget(
    Renderer& renderer, const FrameInput& input, const std::array<float, 4>& clearColor,
    const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
{
    if (!renderer.impl_)
        throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
    renderer.impl_->withHdrSceneTarget(input, clearColor, record);
}
