#include "runtime/render/detail/renderer_impl.h"

namespace Incubator::Render
{
    Renderer::Impl::Impl(Window::Window& window, RendererValidationMode validationMode)
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

    Renderer::Impl::~Impl()
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

    bool Renderer::Impl::prepareFrame()
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

    std::optional<FrameExtent> Renderer::Impl::beginUiFrame()
    {
        if (!prepareFrame())
            return std::nullopt;
        imguiRenderer_->beginFrame();
        const auto extent = swapchain_->extent();
        return FrameExtent{extent.width, extent.height};
    }

    std::vector<Vulkan::VulkanDrawItem> Renderer::Impl::resolveDraws(const FrameInput& input,
                                                     std::vector<std::shared_ptr<void>>& inFlightResources,
                                                     const ScenePipelines& pipelines)
    {
        std::vector<Vulkan::VulkanDrawItem> draws;
        draws.reserve(input.draws.size());
        inFlightResources.reserve(input.draws.size() * 2 + 1);
        // 投影可能直接采用裁剪空间坐标。使用 XY 定向补偿不同的 Y 轴约定。
        const bool invertProjectionWinding = glm::determinant(glm::dmat2(input.camera.projection)) > 0.0;
        for (const auto& draw : input.draws)
        {
            auto gpuMesh = draw.mesh.impl_.lock();
            if (!gpuMesh || gpuMesh->owner != &device_)
                throw Error::Exception(Error::Code::RENDER, "Mesh is invalid or belongs to another Renderer");
            auto gpuMaterial = draw.material.impl_.lock();
            if (!gpuMaterial || gpuMaterial->owner != &device_)
                throw Error::Exception(Error::Code::RENDER,
                                       "Material is invalid or belongs to another Renderer");
            const auto object = objectData(draw.model);
            const auto frontFace = invertProjectionWinding != (object.tangentParams.x < 0.0f)
                                       ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
            const bool isBlend = gpuMaterial->description.alphaMode == AlphaMode::Blend;
            const auto& pair = isBlend ? pipelines.blend : pipelines.solid;
            draws.push_back({gpuMesh->vertexBuffer->handle(), gpuMesh->indexBuffer->handle(),
                             gpuMesh->indexCount, gpuMaterial->binding.descriptorSet(),
                             object, pair[gpuMaterial->description.doubleSided].get(), frontFace, isBlend});
            inFlightResources.push_back(std::move(gpuMesh));
            inFlightResources.push_back(std::move(gpuMaterial));
        }
        return draws;
    }

    std::vector<Vulkan::VulkanShadowDrawItem> Renderer::Impl::resolveShadowDraws(const FrameInput& input,
                                                                 std::vector<std::shared_ptr<void>>& resources)
    {
        std::vector<Vulkan::VulkanShadowDrawItem> draws;
        draws.reserve(input.shadowDraws.size());
        for (const auto& draw : input.shadowDraws)
        {
            // 未指定材质的旧调用方仍可提交双面实心投射物。
            auto material = draw.material.impl_.lock();
            if ((!material && !draw.material.isUnset()) || (material && material->owner != &device_))
                throw Error::Exception(Error::Code::RENDER,
                                       "Shadow material is invalid or belongs to another Renderer");
            if (material && material->description.alphaMode == AlphaMode::Blend)
                continue;
            auto mesh = draw.mesh.impl_.lock();
            if (!mesh || mesh->owner != &device_)
                throw Error::Exception(Error::Code::RENDER,
                                       "Shadow mesh is invalid or belongs to another Renderer");
            const bool masked = material && material->description.alphaMode == AlphaMode::Mask;
            if (masked && shadowMaskVertexShader_.empty())
                throw Error::Exception(Error::Code::RENDER, "Prepare shadow mask shaders before drawing masks");
            const bool doubleSided = !material || material->description.doubleSided;
            const auto frontFace = glm::determinant(glm::dmat3(draw.model)) < 0.0
                                       ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
            draws.push_back({mesh->vertexBuffer->handle(), mesh->indexBuffer->handle(), mesh->indexCount,
                             draw.model, nullptr, masked ? material->binding.descriptorSet() : VK_NULL_HANDLE,
                             doubleSided, frontFace});
            resources.push_back(std::move(mesh));
            if (material)
                resources.push_back(std::move(material));
        }
        return draws;
    }

    void Renderer::Impl::recordHdrSceneDraws(VkCommandBuffer cmd, VkDescriptorSet frameSet,
                             const std::vector<Vulkan::VulkanDrawItem>& draws, bool drawSkybox)
    {
        if (!drawSkybox)
        {
            if (!draws.empty())
                Vulkan::VulkanCommandList::drawIndexed(cmd, *hdrPipelines_.solid[0], frameSet, draws);
            return;
        }
        const auto recordSkybox = [&]() {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline_->handle());
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline_->layout(), 0, 1,
                                    &frameSet, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        };
        const bool hasBlend =
            std::any_of(draws.begin(), draws.end(), [](const auto& item) { return item.alphaBlend; });
        if (!hasBlend)
        {
            if (!draws.empty())
                Vulkan::VulkanCommandList::drawIndexed(cmd, *hdrPipelines_.solid[0], frameSet, draws);
            recordSkybox();
            return;
        }
        std::vector<Vulkan::VulkanDrawItem> solidDraws;
        std::vector<Vulkan::VulkanDrawItem> blendDraws;
        solidDraws.reserve(draws.size());
        blendDraws.reserve(draws.size());
        for (const auto& item : draws)
        {
            if (item.alphaBlend)
                blendDraws.push_back(item);
            else
                solidDraws.push_back(item);
        }
        if (!solidDraws.empty())
            Vulkan::VulkanCommandList::drawIndexed(cmd, *hdrPipelines_.solid[0], frameSet, solidDraws);
        recordSkybox();
        if (!blendDraws.empty())
            Vulkan::VulkanCommandList::drawIndexed(cmd, *hdrPipelines_.solid[0], frameSet, blendDraws);
    }

    std::vector<Vulkan::VulkanDebugLineVertex> Renderer::Impl::buildDebugLineVertices(
        std::span<const DebugLine> lines)
    {
        std::vector<Vulkan::VulkanDebugLineVertex> vertices;
        vertices.reserve(lines.size() * 2u);
        const auto finiteVec3 = [](const glm::vec3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        };
        for (const auto& line : lines)
        {
            if (!finiteVec3(line.start) || !finiteVec3(line.end) || !finiteVec3(line.color) ||
                line.color.r < 0.0f || line.color.g < 0.0f || line.color.b < 0.0f ||
                !std::isfinite(line.alpha) || line.alpha < 0.0f || line.alpha > 1.0f)
                throw Error::Exception(Error::Code::RENDER,
                                       "DebugLine endpoints must be finite, color nonnegative, and alpha in [0, 1]");
            const glm::vec4 rgba(line.color, line.alpha);
            vertices.push_back({line.start, rgba});
            vertices.push_back({line.end, rgba});
        }
        return vertices;
    }

    std::shared_ptr<Vulkan::VulkanBuffer> Renderer::Impl::ensureDebugLineBuffer(
        std::shared_ptr<Vulkan::VulkanBuffer>& slot, std::span<const Vulkan::VulkanDebugLineVertex> vertices)
    {
        if (vertices.empty())
            return {};
        const VkDeviceSize requiredBytes = vertices.size() * sizeof(Vulkan::VulkanDebugLineVertex);
        if (!slot || slot->size() < requiredBytes)
        {
            const VkDeviceSize capacity = std::max<VkDeviceSize>(requiredBytes, 4096u);
            slot = std::make_shared<Vulkan::VulkanBuffer>(
                device_, capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        slot->write(0, std::as_bytes(vertices));
        return slot;
    }

    void Renderer::Impl::recordDebugLines(VkCommandBuffer cmd, VkDescriptorSet frameSet, const Vulkan::VulkanBuffer& buffer,
                          uint32_t vertexCount)
    {
        if (!debugLinePipeline_ || vertexCount == 0u)
            return;
        const VkBuffer vb = buffer.handle();
        const VkDeviceSize offset = 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debugLinePipeline_->handle());
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, debugLinePipeline_->layout(), 0, 1,
                                &frameSet, 0, nullptr);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
        vkCmdDraw(cmd, vertexCount, 1, 0, 0);
    }

    void Renderer::Impl::drawFrame(const FrameInput& input, const std::array<float, 4>& clearColor,
                   const std::function<void(void*)>& recordUi)
    {
        if (input.iblEnabled && !iblIrradiance_)
            throw Error::Exception(Error::Code::RENDER, "Prepare IBL resources before enabling IBL");
        const auto frame = Vulkan::makeFrameUniform(input, iblMaxPrefilteredLod_);
        if (input.shadow)
            throw Error::Exception(Error::Code::RENDER,
                                   "Shadows require drawSceneTarget; drawFrame does not support shadows");
        std::vector<std::shared_ptr<void>> inFlightResources;
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
        const auto draws = resolveDraws(input, inFlightResources, scenePipelines_);

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
                        Vulkan::VulkanCommandList::drawIndexed(commandBuffer, *scenePipelines_.solid[0],
                                                               bindingSet_->descriptorSet(frameIndex), draws);
                    if (recordUi)
                        recordUi(static_cast<void*>(commandBuffer));
                },
                inFlightResources))
            needsRebuild_ = true;
    }

    SceneOutput Renderer::Impl::prepareSceneTarget(FrameExtent extent, bool fxaa)
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

    uint64_t Renderer::Impl::sceneImage(const SceneOutput& output)
    {
        const auto target = output.impl_.lock();
        const auto slot = scheduler_->currentFrameIndex();
        if (!target || target->owner != &device_ || target != sceneTargets_[slot])
            throw Error::Exception(Error::Code::RENDER, "Invalid current scene output");
        return imguiRenderer_->sceneImage(slot, target->target->color(output.fxaaEnabled()).view(),
                                          output.fxaaEnabled());
    }

    void Renderer::Impl::drawSceneTarget(const SceneOutput& output, const FrameInput& input,
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
            input.display.toneMapping != ToneMappingMode::Reinhard &&
            input.display.toneMapping != ToneMappingMode::AcesFilmic)
            throw Error::Exception(Error::Code::RENDER, "Invalid tone mapping mode");
        if (!std::isfinite(input.display.exposureEv) ||
            input.display.exposureEv < DisplaySettings::MIN_EXPOSURE_EV ||
            input.display.exposureEv > DisplaySettings::MAX_EXPOSURE_EV)
            throw Error::Exception(Error::Code::RENDER, "Exposure EV is out of range");
        if (!input.display.valid())
            throw Error::Exception(Error::Code::RENDER, "Bloom display settings are out of range");
        if (input.iblEnabled && !iblIrradiance_)
            throw Error::Exception(Error::Code::RENDER, "Prepare IBL resources before enabling IBL");
        const auto frame = Vulkan::makeFrameUniform(input, iblMaxPrefilteredLod_);
        std::vector<std::shared_ptr<void>> resources;
        if (!input.draws.empty() || !input.debugLines.empty())
        {
            if (!bindingSet_)
                throw Error::Exception(Error::Code::RENDER, "Prepare scene shaders first");
            if (!input.draws.empty())
                ensureHdrPipelines();
        }
        if (!input.debugLines.empty() && !debugLinePipeline_)
            throw Error::Exception(Error::Code::RENDER,
                                   "Prepare debug line resources before submitting debug lines");
        auto draws = resolveDraws(input, resources, hdrPipelines_);
        const auto debugVertices = buildDebugLineVertices(input.debugLines);
        const bool drawSkybox =
            input.display.skyboxEnabled && iblIrradiance_ != nullptr && skyboxPipeline_ != nullptr;
        const bool doBloom = input.display.bloomEnabled && input.display.bloomIntensity > 0.0f &&
                             bloomPrefilterPass_ != nullptr && bloomDownsamplePass_ != nullptr &&
                             bloomUpsamplePass_ != nullptr;
        std::vector<Vulkan::VulkanShadowDrawItem> shadowDraws;
        if (input.shadow)
        {
            if (shadowVertexShader_.empty())
                throw Error::Exception(Error::Code::RENDER, "Prepare shadow vertex shader before enabling shadows");
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
        auto debugBuffer = ensureDebugLineBuffer(debugLineBuffers_[index], debugVertices);
        if (debugBuffer)
            resources.push_back(debugBuffer);
        std::shared_ptr<Vulkan::VulkanShadowTarget> shadow;
        if (input.shadow)
        {
            auto& slot = shadowTargets_[index];
            if (!slot || slot->resolution() != input.shadow->resolution ||
                slot->layerCount() != input.shadow->cascadeCount)
                slot = std::make_shared<Vulkan::VulkanShadowTarget>(
                    device_, input.shadow->resolution, input.shadow->cascadeCount);
            shadow = slot;
            selectShadowPipelines(shadowDraws, shadow->image().format());
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
            static_cast<uint32_t>(input.display.toneMapping),
            std::exp2(input.display.exposureEv),
            doBloom ? input.display.bloomIntensity : 0.0f,
            0.0f};
        const Vulkan::VulkanBloomParams bloomParams{
            doBloom,
            input.display.bloomThreshold,
            input.display.bloomIntensity,
            bloomPrefilterPass_.get(),
            bloomDownsamplePass_.get(),
            bloomUpsamplePass_.get()};
        const Vulkan::ViewportSampleInput sampleInput{
            static_cast<uint32_t>(draws.size()),
            static_cast<uint32_t>(shadow ? shadowDraws.size() * shadow->layerCount() : 0)};
        if (!scheduler_->renderAndPresent(
                *swapchain_, clear,
                [&](VkCommandBuffer cmd) {
                    if (ui)
                        ui(static_cast<void*>(cmd));
                },
                resources,
                [&](VkCommandBuffer cmd) {
                    if (shadow)
                        shadow->record(cmd, [&](VkCommandBuffer depth, uint32_t cascadeIndex) {
                            Vulkan::VulkanCommandList::drawShadow(
                                depth, bindingSet_->descriptorSet(index), shadowDraws, cascadeIndex);
                        });
                    target->target->record(cmd, *displayPass_, displayParams,
                                           [&](VkCommandBuffer scene) {
                                               if (bindingSet_)
                                               {
                                                   recordHdrSceneDraws(scene, bindingSet_->descriptorSet(index),
                                                                       draws, drawSkybox);
                                                   if (debugBuffer)
                                                       recordDebugLines(
                                                           scene, bindingSet_->descriptorSet(index),
                                                           *debugBuffer,
                                                           static_cast<uint32_t>(debugVertices.size()));
                                               }
                                           },
                                           output.fxaaEnabled() ? fxaaPass_.get() : nullptr, bloomParams);
                },
                sampleInput))
        {
            needsRebuild_ = true;
            return;
        }
        lastSubmittedOutput_ = target;
        lastSubmittedFxaa_ = output.fxaaEnabled();
    }

    ViewportStats Renderer::Impl::lastViewportStats() const noexcept
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

    uint64_t Renderer::Impl::completedViewportSampleCount() const noexcept
    {
        return scheduler_->completedViewportSampleCount();
    }

    Integration::ImGuiRenderer& Renderer::Impl::imgui()
    {
        return *imguiRenderer_;
    }

    bool Renderer::Impl::rebuild(int width, int height, const char* reason)
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
            if (bindingSet_ && (!scenePipelines_.solid[0] || previousFormat != swapchain_->format()))
            {
                scenePipelines_ = makeScenePipelines(
                    swapchain_->format(), depthFormat_, bindingSet_->layout(),
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

    bool GpuResources::valid() const noexcept
    {
        return impl_ && impl_->owner;
    }

    Renderer::Renderer(Window::Window& window, RendererValidationMode validationMode)
        : impl_(std::make_unique<Impl>(window, validationMode))
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

    uint64_t Renderer::sceneImage(const SceneOutput& output)
{
    if (!impl_)
        throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
    return impl_->sceneImage(output);
}

    ViewportStats Renderer::lastViewportStats() const noexcept
{
    if (!impl_)
        return {};
    return impl_->lastViewportStats();
}
}  // namespace Incubator::Render
