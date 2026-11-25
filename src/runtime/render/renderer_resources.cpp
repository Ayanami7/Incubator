#include "runtime/render/detail/renderer_impl.h"

namespace Incubator::Render
{
    GpuResources Renderer::Impl::createResources()
    {
        auto group = std::make_shared<GpuResources::Impl>();
        group->owner = &device_;
        std::erase_if(resourceGroups_, [](const auto& value) { return value.expired(); });
        resourceGroups_.push_back(group);
        return GpuResources(std::move(group));
    }

    const std::shared_ptr<GpuResources::Impl>& Renderer::Impl::checkResources(const GpuResources& resources) const
    {
        if (!resources.impl_ || resources.impl_->owner != &device_)
            throw Error::Exception(Error::Code::RENDER,
                                   "GPU resources are invalid or belong to another Renderer");
        return resources.impl_;
    }

    Mesh Renderer::Impl::createMesh(std::span<const MeshVertex> vertices, std::span<const uint32_t> indices,
                    const std::shared_ptr<GpuResources::Impl>& resources)
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

    Texture Renderer::Impl::createTexture(const Resource::TextureResource& texture, TextureMipMode mipMode,
                          TextureColorSpace colorSpace,
                          const std::shared_ptr<GpuResources::Impl>& resources)
    {
        auto gpuTexture = std::make_shared<Texture::Impl>(device_, texture, mipMode, colorSpace);
        std::erase_if(textures_, [](const auto& entry) { return entry.object.expired(); });
        textures_.push_back({gpuTexture, resources ? nullptr : gpuTexture});
        if (resources)
            resources->objects.push_back(gpuTexture);
        return Texture(gpuTexture);
    }

    Texture Renderer::Impl::createTexture(const Resource::FloatTextureResource& texture,
                          const std::shared_ptr<GpuResources::Impl>& resources)
    {
        auto gpuTexture = std::make_shared<Texture::Impl>(device_, texture);
        std::erase_if(textures_, [](const auto& entry) { return entry.object.expired(); });
        textures_.push_back({gpuTexture, resources ? nullptr : gpuTexture});
        if (resources)
            resources->objects.push_back(gpuTexture);
        return Texture(gpuTexture);
    }

    Renderer::Impl::PipelinePair Renderer::Impl::makePipelines(VkFormat color, VkFormat depth, VkDescriptorSetLayout frameLayout,
                               VkDescriptorSetLayout materialLayout, const std::vector<uint32_t>& vertex,
                               const std::vector<uint32_t>& fragment, bool hdr, bool alphaBlend)
    {
        return {std::make_unique<Vulkan::VulkanPipeline>(
                    device_, color, depth, frameLayout, materialLayout, vertex, fragment, hdr,
                    VK_CULL_MODE_BACK_BIT, alphaBlend),
                std::make_unique<Vulkan::VulkanPipeline>(
                    device_, color, depth, frameLayout, materialLayout, vertex, fragment, hdr,
                    VK_CULL_MODE_NONE, alphaBlend)};
    }

    Renderer::Impl::ScenePipelines Renderer::Impl::makeScenePipelines(VkFormat color, VkFormat depth, VkDescriptorSetLayout frameLayout,
                                      VkDescriptorSetLayout materialLayout, const std::vector<uint32_t>& vertex,
                                      const std::vector<uint32_t>& fragment, bool hdr)
    {
        return {makePipelines(color, depth, frameLayout, materialLayout, vertex, fragment, hdr, false),
                makePipelines(color, depth, frameLayout, materialLayout, vertex, fragment, hdr, true)};
    }

    void Renderer::Impl::ensureHdrPipelines()
    {
        if (!hdrPipelines_.solid[0])
            hdrPipelines_ = makeScenePipelines(VK_FORMAT_R16G16B16A16_SFLOAT, depthFormat_, bindingSet_->layout(),
                                               bindingSet_->materialLayout(), vertexShader_, fragmentShader_,
                                               true);
    }

    void Renderer::Impl::prepareSceneResources(const Resource::ShaderResource& vertexShader,
                               const Resource::ShaderResource& fragmentShader)
    {
        if (bindingSet_)
            throw Error::Exception(Error::Code::RENDER, "Scene resources are already prepared");
        if (vertexShader.words().empty() || fragmentShader.words().empty())
            throw Error::Exception(Error::Code::RENDER, "Scene shaders must contain SPIR-V words");

        auto vertexWords = vertexShader.words();
        auto fragmentWords = fragmentShader.words();
        auto bindingSet = std::make_unique<Vulkan::VulkanBindingSet>(device_, uniformBuffers_);
        ScenePipelines pipelines;
        if (swapchain_ && swapchain_->handle() != VK_NULL_HANDLE)
            pipelines = makeScenePipelines(
                swapchain_->format(), depthFormat_, bindingSet->layout(), bindingSet->materialLayout(),
                vertexWords, fragmentWords);

        vertexShader_ = std::move(vertexWords);
        fragmentShader_ = std::move(fragmentWords);
        bindingSet_ = std::move(bindingSet);
        scenePipelines_ = std::move(pipelines);
    }

    void Renderer::Impl::prepareShadowResources(const Resource::ShaderResource& vertexShader,
                                const Resource::ShaderResource* maskVertex,
                                const Resource::ShaderResource* maskFragment)
    {
        if (!bindingSet_)
            throw Error::Exception(Error::Code::RENDER, "Prepare scene resources before shadow resources");
        if (!shadowVertexShader_.empty())
            throw Error::Exception(Error::Code::RENDER, "Shadow resources are already prepared");
        if (vertexShader.words().empty())
            throw Error::Exception(Error::Code::RENDER, "Shadow vertex shader must contain SPIR-V words");
        if (maskVertex && (maskVertex->words().empty() || maskFragment->words().empty()))
            throw Error::Exception(Error::Code::RENDER, "Shadow mask shaders must contain SPIR-V words");
        auto opaque = vertexShader.words();
        auto vertex = maskVertex ? maskVertex->words() : std::vector<uint32_t>{};
        auto fragment = maskFragment ? maskFragment->words() : std::vector<uint32_t>{};
        shadowVertexShader_ = std::move(opaque);
        shadowMaskVertexShader_ = std::move(vertex);
        shadowMaskFragmentShader_ = std::move(fragment);
    }

    void Renderer::Impl::prepareDisplayResources(const Resource::ShaderResource& vertexShader,
                                 const Resource::ShaderResource& fragmentShader)
    {
        if (displayPass_)
            throw Error::Exception(Error::Code::RENDER, "Display resources are already prepared");
        if (vertexShader.words().empty() || fragmentShader.words().empty())
            throw Error::Exception(Error::Code::RENDER, "Display shaders must contain SPIR-V words");
        auto vertexWords = vertexShader.words();
        displayPass_ = std::make_unique<Vulkan::VulkanDisplayPass>(
            device_, VK_FORMAT_R8G8B8A8_UNORM, vertexWords, fragmentShader.words(),
            SamplerDesc{TextureFilter::Linear, TextureFilter::Linear, MipFilter::BaseLevel},
            VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, uint32_t(sizeof(Vulkan::VulkanDisplayParams)), 2u);
        displayVertexShader_ = std::move(vertexWords);
    }

    void Renderer::Impl::prepareFxaaResources(const Resource::ShaderResource& fragmentShader)
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

    void Renderer::Impl::prepareSkyboxResources(const Resource::ShaderResource& vertexShader,
                                const Resource::ShaderResource& fragmentShader)
    {
        if (!bindingSet_)
            throw Error::Exception(Error::Code::RENDER, "Prepare scene resources before skybox resources");
        if (skyboxPipeline_)
            throw Error::Exception(Error::Code::RENDER, "Skybox resources are already prepared");
        if (vertexShader.words().empty() || fragmentShader.words().empty())
            throw Error::Exception(Error::Code::RENDER, "Skybox shaders must contain SPIR-V words");
        skyboxPipeline_ = std::make_unique<Vulkan::VulkanPipeline>(
            device_, VK_FORMAT_R16G16B16A16_SFLOAT, depthFormat_, bindingSet_->layout(), vertexShader.words(),
            fragmentShader.words(), VK_COMPARE_OP_LESS_OR_EQUAL, false, VK_CULL_MODE_NONE);
    }

    void Renderer::Impl::prepareBloomResources(const Resource::ShaderResource& prefilterShader,
                               const Resource::ShaderResource& downsampleShader,
                               const Resource::ShaderResource& upsampleShader)
    {
        if (!displayPass_ || displayVertexShader_.empty())
            throw Error::Exception(Error::Code::RENDER, "Prepare display resources before bloom resources");
        if (bloomPrefilterPass_)
            throw Error::Exception(Error::Code::RENDER, "Bloom resources are already prepared");
        if (prefilterShader.words().empty() || downsampleShader.words().empty() || upsampleShader.words().empty())
            throw Error::Exception(Error::Code::RENDER, "Bloom shaders must contain SPIR-V words");
        const SamplerDesc linearSampler{TextureFilter::Linear, TextureFilter::Linear, MipFilter::BaseLevel};
        auto prefilter = std::make_unique<Vulkan::VulkanDisplayPass>(
            device_, VK_FORMAT_R16G16B16A16_SFLOAT, displayVertexShader_, prefilterShader.words(),
            linearSampler, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 16u, 1u, false);
        auto downsample = std::make_unique<Vulkan::VulkanDisplayPass>(
            device_, VK_FORMAT_R16G16B16A16_SFLOAT, displayVertexShader_, downsampleShader.words(),
            linearSampler, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 16u, 1u, false);
        auto upsample = std::make_unique<Vulkan::VulkanDisplayPass>(
            device_, VK_FORMAT_R16G16B16A16_SFLOAT, displayVertexShader_, upsampleShader.words(),
            linearSampler, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 16u, 1u, true);
        bloomPrefilterPass_ = std::move(prefilter);
        bloomDownsamplePass_ = std::move(downsample);
        bloomUpsamplePass_ = std::move(upsample);
    }

    void Renderer::Impl::prepareDebugLineResources(const Resource::ShaderResource& vertexShader,
                                   const Resource::ShaderResource& fragmentShader)
    {
        if (!bindingSet_)
            throw Error::Exception(Error::Code::RENDER, "Prepare scene resources before debug line resources");
        if (debugLinePipeline_)
            throw Error::Exception(Error::Code::RENDER, "Debug line resources are already prepared");
        if (vertexShader.words().empty() || fragmentShader.words().empty())
            throw Error::Exception(Error::Code::RENDER, "Debug line shaders must contain SPIR-V words");
        debugLinePipeline_ = std::make_unique<Vulkan::VulkanPipeline>(
            device_, VK_FORMAT_R16G16B16A16_SFLOAT, depthFormat_, bindingSet_->layout(),
            vertexShader.words(), fragmentShader.words());
    }

    void Renderer::Impl::prepareIblResources(const Texture& irradiance, const Texture& prefiltered, const Texture& brdfLut)
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

    Material Renderer::Impl::createMaterial(const MaterialDesc& desc, const std::shared_ptr<GpuResources::Impl>& resources)
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

    void Renderer::Impl::selectShadowPipelines(std::span<Vulkan::VulkanShadowDrawItem> draws, VkFormat depthFormat)
    {
        const std::vector<uint32_t> noFragment;
        for (auto& draw : draws)
        {
            const bool masked = draw.materialSet != VK_NULL_HANDLE;
            auto& pipelines = masked ? shadowMaskPipelines_ : shadowPipelines_;
            if (!pipelines[0])
                pipelines = makePipelines(
                    VK_FORMAT_UNDEFINED, depthFormat, bindingSet_->layout(),
                    masked ? bindingSet_->materialLayout() : VK_NULL_HANDLE,
                    masked ? shadowMaskVertexShader_ : shadowVertexShader_,
                    masked ? shadowMaskFragmentShader_ : noFragment);
            draw.pipeline = pipelines[draw.doubleSided].get();
        }
    }

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

    void Renderer::prepareShadowResources(const Resource::ShaderResource& vertexShader,
                                          const Resource::ShaderResource& maskVertex,
                                          const Resource::ShaderResource& maskFragment)
    {
        if (!impl_)
            throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
        impl_->prepareShadowResources(vertexShader, &maskVertex, &maskFragment);
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

    void Renderer::prepareSkyboxResources(const Resource::ShaderResource& vertexShader,
                                          const Resource::ShaderResource& fragmentShader)
    {
        if (!impl_)
            throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
        impl_->prepareSkyboxResources(vertexShader, fragmentShader);
    }

    void Renderer::prepareBloomResources(const Resource::ShaderResource& prefilterShader,
                                         const Resource::ShaderResource& downsampleShader,
                                         const Resource::ShaderResource& upsampleShader)
    {
        if (!impl_)
            throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
        impl_->prepareBloomResources(prefilterShader, downsampleShader, upsampleShader);
    }

    void Renderer::prepareDebugLineResources(const Resource::ShaderResource& vertexShader,
                                             const Resource::ShaderResource& fragmentShader)
    {
        if (!impl_)
            throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
        impl_->prepareDebugLineResources(vertexShader, fragmentShader);
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
}  // namespace Incubator::Render
