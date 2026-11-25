#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/detail/renderer_review.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"
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

namespace Incubator::Render
{
    struct SceneOutput::Impl
    {
        const Vulkan::VulkanDevice* owner;
        std::unique_ptr<Vulkan::VulkanSceneTarget> target;
    };

    inline Vulkan::VulkanObjectData objectData(const glm::mat4& model)
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

    inline std::vector<const char*> requiredRendererInstanceExtensions()
    {
        return Vulkan::GlfwSurface::requiredInstanceExtensions();
    }

    inline Vulkan::ValidationMode toVulkanValidationMode(RendererValidationMode mode)
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

    struct GpuResources::Impl
    {
        const Vulkan::VulkanDevice* owner;
        std::vector<std::shared_ptr<void>> objects;
    };

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
                                                       texture.faceCount(), mipPayloads, VK_IMAGE_USAGE_SAMPLED_BIT);
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
             std::shared_ptr<Texture::Impl> emissiveSource, const Vulkan::VulkanBindingSet& bindingSet,
             const MaterialDesc& desc)
            : owner(&device),
              texture(std::move(source)),
              normalTexture(std::move(normalSource)),
              metallicRoughnessTexture(std::move(mrSource)),
              emissiveTexture(std::move(emissiveSource)),
              description(desc),
              colorSampler(std::make_shared<Vulkan::VulkanSampler>(device, desc.sampler, texture->image->mipLevels())),
              normalSampler(selectSampler(device, desc.sampler, normalImage(bindingSet).mipLevels(), colorSampler,
                                          texture->image->mipLevels())),
              mrSampler(selectSampler(device, desc.sampler, metallicRoughnessImage(bindingSet).mipLevels(),
                                      colorSampler, texture->image->mipLevels(), normalSampler,
                                      normalImage(bindingSet).mipLevels())),
              emissiveSampler(selectSampler(device, desc.sampler, emissiveImage(bindingSet).mipLevels(),
                                            colorSampler, texture->image->mipLevels())),
              binding(device, bindingSet.materialLayout(), *texture->image, *colorSampler, normalImage(bindingSet),
                      *normalSampler, metallicRoughnessImage(bindingSet), *mrSampler, emissiveImage(bindingSet),
                      *emissiveSampler, desc)
        {}

        const Vulkan::VulkanImage& normalImage(const Vulkan::VulkanBindingSet& bindingSet) const noexcept
        {
            return normalTexture ? *normalTexture->image : bindingSet.normalFallback();
        }

        const Vulkan::VulkanImage& metallicRoughnessImage(const Vulkan::VulkanBindingSet& bindingSet) const noexcept
        {
            return metallicRoughnessTexture ? *metallicRoughnessTexture->image : bindingSet.metallicRoughnessFallback();
        }

        const Vulkan::VulkanImage& emissiveImage(const Vulkan::VulkanBindingSet& bindingSet) const noexcept
        {
            return emissiveTexture ? *emissiveTexture->image : bindingSet.emissiveFallback();
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
        std::shared_ptr<Vulkan::VulkanSampler> emissiveSampler;
        Vulkan::VulkanMaterialBinding binding;
    };

    class Renderer::Impl
    {
    public:
        using PipelinePair = std::array<std::unique_ptr<Vulkan::VulkanPipeline>, 2>;

        struct ScenePipelines
        {
            PipelinePair solid;
            PipelinePair blend;
        };

        Impl(Window::Window& window, RendererValidationMode validationMode);
        ~Impl();

        GpuResources createResources();
        const std::shared_ptr<GpuResources::Impl>& checkResources(const GpuResources& resources) const;
        Detail::RendererReviewAccess::ResourceCounts resourceCounts() const;
        void waitForFrames();
        bool prepareFrame();

        Mesh createMesh(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices,
                        const std::shared_ptr<GpuResources::Impl>& resources = {});
        Texture createTexture(const Resource::TextureResource& texture, TextureMipMode mipMode,
                              TextureColorSpace colorSpace,
                              const std::shared_ptr<GpuResources::Impl>& resources = {});
        Texture createTexture(const Resource::FloatTextureResource& texture,
                              const std::shared_ptr<GpuResources::Impl>& resources = {});
        Material createMaterial(const MaterialDesc& desc, const std::shared_ptr<GpuResources::Impl>& resources = {});

        PipelinePair makePipelines(VkFormat color, VkFormat depth, VkDescriptorSetLayout frameLayout,
                                   VkDescriptorSetLayout materialLayout, const std::vector<uint32_t>& vertex,
                                   const std::vector<uint32_t>& fragment, bool hdr = false, bool alphaBlend = false);
        ScenePipelines makeScenePipelines(VkFormat color, VkFormat depth, VkDescriptorSetLayout frameLayout,
                                          VkDescriptorSetLayout materialLayout, const std::vector<uint32_t>& vertex,
                                          const std::vector<uint32_t>& fragment, bool hdr = false);
        void ensureHdrPipelines();

        void prepareSceneResources(const Resource::ShaderResource& vertexShader,
                                   const Resource::ShaderResource& fragmentShader);
        void prepareShadowResources(const Resource::ShaderResource& vertexShader,
                                    const Resource::ShaderResource* maskVertex = nullptr,
                                    const Resource::ShaderResource* maskFragment = nullptr);
        void prepareDisplayResources(const Resource::ShaderResource& vertexShader,
                                     const Resource::ShaderResource& fragmentShader);
        void prepareFxaaResources(const Resource::ShaderResource& fragmentShader);
        void prepareSkyboxResources(const Resource::ShaderResource& vertexShader,
                                    const Resource::ShaderResource& fragmentShader);
        void prepareBloomResources(const Resource::ShaderResource& prefilterShader,
                                   const Resource::ShaderResource& downsampleShader,
                                   const Resource::ShaderResource& upsampleShader);
        void prepareDebugLineResources(const Resource::ShaderResource& vertexShader,
                                       const Resource::ShaderResource& fragmentShader);
        void prepareIblResources(const Texture& irradiance, const Texture& prefiltered, const Texture& brdfLut);

        std::optional<FrameExtent> beginUiFrame();
        std::vector<Vulkan::VulkanDrawItem> resolveDraws(const FrameInput& input,
                                                         std::vector<std::shared_ptr<void>>& inFlightResources,
                                                         const ScenePipelines& pipelines);
        std::vector<Vulkan::VulkanShadowDrawItem> resolveShadowDraws(const FrameInput& input,
                                                                     std::vector<std::shared_ptr<void>>& resources);
        void selectShadowPipelines(std::span<Vulkan::VulkanShadowDrawItem> draws, VkFormat depthFormat);
        void withReviewFrame(const FrameInput& input, const Detail::RendererReviewAccess::Record& record);
        void recordHdrSceneDraws(VkCommandBuffer cmd, VkDescriptorSet frameSet,
                                 const std::vector<Vulkan::VulkanDrawItem>& draws, bool drawSkybox);
        static std::vector<Vulkan::VulkanDebugLineVertex> buildDebugLineVertices(std::span<const DebugLine> lines);
        std::shared_ptr<Vulkan::VulkanBuffer> ensureDebugLineBuffer(
            std::shared_ptr<Vulkan::VulkanBuffer>& slot, std::span<const Vulkan::VulkanDebugLineVertex> vertices);
        void recordDebugLines(VkCommandBuffer cmd, VkDescriptorSet frameSet, const Vulkan::VulkanBuffer& buffer,
                              uint32_t vertexCount);
        void withHdrSceneTarget(
            const FrameInput& input, const std::array<float, 4>& clearColor,
            const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record);
        void drawFrame(const FrameInput& input, const std::array<float, 4>& clearColor,
                       const std::function<void(void*)>& recordUi);
        void reviewOutput(
            const SceneOutput& output,
            const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record);
        void reviewShadowOutput(
            const SceneOutput& output,
            const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record);
        SceneOutput prepareSceneTarget(FrameExtent extent, bool fxaa = false);
        uint64_t sceneImage(const SceneOutput& output);
        void drawSceneTarget(const SceneOutput& output, const FrameInput& input,
                             const std::array<float, 4>& clearColor, const std::function<void(void*)>& ui);
        std::optional<CapturedViewportImage> captureLastSceneViewport();
        ViewportStats lastViewportStats() const noexcept;
        uint64_t completedViewportSampleCount() const noexcept;
        Integration::ImGuiRenderer& imgui();

    private:
        bool rebuild(int width, int height, const char* reason);

        Window::Window& window_;
        Vulkan::VulkanInstance instance_;
        Vulkan::GlfwSurface surface_;
        Vulkan::VulkanDevice device_;
        std::vector<uint32_t> vertexShader_;
        std::vector<uint32_t> fragmentShader_;
        std::vector<uint32_t> shadowVertexShader_;
        std::vector<uint32_t> shadowMaskVertexShader_;
        std::vector<uint32_t> shadowMaskFragmentShader_;
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
        ScenePipelines scenePipelines_;
        std::array<std::shared_ptr<SceneOutput::Impl>, Vulkan::VulkanFrameScheduler::FRAME_COUNT> sceneTargets_{};
        ScenePipelines hdrPipelines_;
        std::unique_ptr<Vulkan::VulkanDisplayPass> displayPass_;
        std::unique_ptr<Vulkan::VulkanDisplayPass> fxaaPass_;
        std::unique_ptr<Vulkan::VulkanDisplayPass> bloomPrefilterPass_;
        std::unique_ptr<Vulkan::VulkanDisplayPass> bloomDownsamplePass_;
        std::unique_ptr<Vulkan::VulkanDisplayPass> bloomUpsamplePass_;
        std::unique_ptr<Vulkan::VulkanPipeline> skyboxPipeline_;
        std::unique_ptr<Vulkan::VulkanPipeline> debugLinePipeline_;
        std::array<std::shared_ptr<Vulkan::VulkanBuffer>, Vulkan::VulkanFrameScheduler::FRAME_COUNT>
            debugLineBuffers_{};
        std::shared_ptr<SceneOutput::Impl> lastSubmittedOutput_;
        bool lastSubmittedFxaa_ = false;
        PipelinePair shadowPipelines_;
        PipelinePair shadowMaskPipelines_;
        std::unique_ptr<Vulkan::VulkanFrameScheduler> scheduler_;
        std::unique_ptr<Integration::ImGuiRenderer> imguiRenderer_;
        VkFormat depthFormat_ = VK_FORMAT_UNDEFINED;
        int requestedWidth_ = 0;
        int requestedHeight_ = 0;
        bool needsRebuild_ = false;
    };
}  // namespace Incubator::Render
