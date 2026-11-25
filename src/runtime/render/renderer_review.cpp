#include "runtime/render/detail/renderer_impl.h"

namespace Incubator::Render
{
    Detail::RendererReviewAccess::ResourceCounts Renderer::Impl::resourceCounts() const
    {
        const auto live = [](const auto& entries) {
            return size_t(
                std::ranges::count_if(entries, [](const auto& entry) { return !entry.object.expired(); }));
        };
        return {live(meshes_), live(textures_), live(materials_)};
    }

    void Renderer::Impl::waitForFrames()
    {
        scheduler_->waitForFrames();
    }

    void Renderer::Impl::withReviewFrame(const FrameInput& input, const Detail::RendererReviewAccess::Record& record)
    {
        if (input.shadow)
            throw Error::Exception(Error::Code::RENDER,
                                   "Shadow review requires the independent scene target path");
        if (input.iblEnabled && !iblIrradiance_)
            throw Error::Exception(Error::Code::RENDER, "Prepare IBL resources before enabling IBL");
        const auto frame = Vulkan::makeFrameUniform(input, iblMaxPrefilteredLod_);
        std::vector<std::shared_ptr<void>> resources;
        if (!scenePipelines_.solid[0])
            throw Error::Exception(Error::Code::RENDER, "Prepare scene shaders before GPU review");
        const auto draws = resolveDraws(input, resources, scenePipelines_);
        const std::array uniforms = {std::make_shared<Vulkan::VulkanBuffer>(
            device_, sizeof(frame), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        uniforms[0]->write(0, std::as_bytes(std::span(&frame, 1)));
        Vulkan::VulkanBindingSet bindings(device_, uniforms);
        if (iblIrradiance_)
            bindings.setIblImages(iblIrradiance_->image.get(), iblPrefiltered_->image.get(),
                                  iblBrdfLut_->image.get());
        record(device_, *scenePipelines_.solid[0], bindings.descriptorSet(0), draws);
    }

    void Renderer::Impl::withHdrSceneTarget(
        const FrameInput& input, const std::array<float, 4>& clearColor,
        const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
    {
        if (!bindingSet_)
            throw Error::Exception(Error::Code::RENDER, "Prepare scene shaders before GPU review");
        if (input.iblEnabled && !iblIrradiance_)
            throw Error::Exception(Error::Code::RENDER, "Prepare IBL resources before enabling IBL");
        if (!input.debugLines.empty() && !debugLinePipeline_)
            throw Error::Exception(Error::Code::RENDER,
                                   "Prepare debug line resources before submitting debug lines");
        const auto frame = Vulkan::makeFrameUniform(input, iblMaxPrefilteredLod_);
        std::vector<std::shared_ptr<void>> resources;
        ensureHdrPipelines();
        auto draws = resolveDraws(input, resources, hdrPipelines_);
        const auto debugVerts = buildDebugLineVertices(input.debugLines);
        std::shared_ptr<Vulkan::VulkanBuffer> debugBuffer;
        if (!debugVerts.empty())
        {
            debugBuffer = std::make_shared<Vulkan::VulkanBuffer>(
                device_, debugVerts.size() * sizeof(Vulkan::VulkanDebugLineVertex),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            debugBuffer->write(0, std::as_bytes(std::span(debugVerts)));
        }
        const bool drawSkybox =
            input.display.skyboxEnabled && iblIrradiance_ != nullptr && skyboxPipeline_ != nullptr;
        std::vector<Vulkan::VulkanShadowDrawItem> shadowDraws;
        std::unique_ptr<Vulkan::VulkanShadowTarget> shadowTarget;
        if (input.shadow)
        {
            if (shadowVertexShader_.empty())
                throw Error::Exception(Error::Code::RENDER,
                                       "Prepare shadow vertex shader before enabling shadows");
            shadowDraws = resolveShadowDraws(input, resources);
            shadowTarget = std::make_unique<Vulkan::VulkanShadowTarget>(
                device_, input.shadow->resolution, input.shadow->cascadeCount);
            selectShadowPipelines(shadowDraws, shadowTarget->image().format());
        }
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
                shadowTarget->record(cmd, [&](VkCommandBuffer depth, uint32_t cascadeIndex) {
                    Vulkan::VulkanCommandList::drawShadow(depth, bindings.descriptorSet(0), shadowDraws,
                                                          cascadeIndex);
                });
            hdrTarget.record(cmd, clear, [&](VkCommandBuffer scene) {
                recordHdrSceneDraws(scene, bindings.descriptorSet(0), draws, drawSkybox);
                if (debugBuffer)
                    recordDebugLines(scene, bindings.descriptorSet(0), *debugBuffer,
                                     static_cast<uint32_t>(debugVerts.size()));
            });
        });
        record(device_, hdrTarget.color());
    }

    void Renderer::Impl::reviewOutput(
        const SceneOutput& output,
        const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
    {
        const auto target = output.impl_.lock();
        if (!target || target->owner != &device_)
            throw Error::Exception(Error::Code::RENDER, "Invalid scene output");
        scheduler_->waitForFrames();
        record(device_, target->target->color(output.fxaaEnabled()));
    }

    void Renderer::Impl::reviewShadowOutput(
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

    std::optional<CapturedViewportImage> Renderer::Impl::captureLastSceneViewport()
    {
        if (!lastSubmittedOutput_ || !lastSubmittedOutput_->target)
            return std::nullopt;
        scheduler_->waitForFrames();
        const auto& image = lastSubmittedOutput_->target->color(lastSubmittedFxaa_);
        const VkExtent2D ext = image.extent();
        if (ext.width == 0u || ext.height == 0u)
            return std::nullopt;
        const size_t byteSize = size_t(ext.width) * ext.height * 4u;
        Vulkan::VulkanBuffer staging(device_, byteSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        device_.executeImmediate([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier2 toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            toTransfer.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toTransfer.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            toTransfer.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toTransfer.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = image.handle();
            toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &toTransfer;
            vkCmdPipelineBarrier2(cmd, &dep);

            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {ext.width, ext.height, 1};
            vkCmdCopyImageToBuffer(cmd, image.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle(), 1,
                                   &copy);

            VkImageMemoryBarrier2 toShader = toTransfer;
            toShader.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toShader.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            toShader.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            toShader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkBufferMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            toHost.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = staging.handle();
            toHost.size = VK_WHOLE_SIZE;
            dep.pImageMemoryBarriers = &toShader;
            dep.bufferMemoryBarrierCount = 1;
            dep.pBufferMemoryBarriers = &toHost;
            vkCmdPipelineBarrier2(cmd, &dep);
        });
        CapturedViewportImage result{{ext.width, ext.height}, std::vector<uint8_t>(byteSize)};
        staging.read(0, std::as_writable_bytes(std::span(result.rgba8)));
        return result;
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

    void Detail::RendererReviewAccess::withFrame(Renderer& renderer, const FrameInput& input, const Record& record)
    {
        renderer.impl_->withReviewFrame(input, record);
    }

    std::optional<CapturedViewportImage> Renderer::captureLastSceneViewport()
{
    if (!impl_)
        throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
    return impl_->captureLastSceneViewport();
}

    void Detail::RendererReviewAccess::withSceneOutput(
    Renderer& renderer, const SceneOutput& output,
    const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
{
    renderer.impl_->reviewOutput(output, record);
}

    uint64_t Detail::RendererReviewAccess::completedViewportSampleCount(Renderer& renderer)
{
    if (!renderer.impl_)
        throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
    return renderer.impl_->completedViewportSampleCount();
}

    void Detail::RendererReviewAccess::withShadowOutput(
    Renderer& renderer, const SceneOutput& output,
    const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
{
    renderer.impl_->reviewShadowOutput(output, record);
}

    void Detail::RendererReviewAccess::withHdrSceneTarget(
    Renderer& renderer, const FrameInput& input, const std::array<float, 4>& clearColor,
    const std::function<void(const Vulkan::VulkanDevice&, const Vulkan::VulkanImage&)>& record)
{
    if (!renderer.impl_)
        throw Error::Exception(Error::Code::RENDER, "Renderer has been moved from");
    renderer.impl_->withHdrSceneTarget(input, clearColor, record);
}
}  // namespace Incubator::Render
