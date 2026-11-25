#include "runtime/render/vulkan/vulkan_binding_set.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <glm/matrix.hpp>

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_buffer.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_image.h"

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            namespace
            {
                void checkVk(VkResult result, const char* operation)
                {
                    if (result != VK_SUCCESS)
                    {
                        throw Error::Exception(Error::Code::RENDER, fmt::format("{} failed with VkResult {}", operation,
                                                                                static_cast<int>(result)));
                    }
                }
            }  // namespace

            VulkanFrameUniform makeFrameUniform(const FrameInput& input, float maxPrefilteredLod)
            {
                const auto& lighting = input.lighting;
                if (!lighting.valid())
                    throw Error::Exception(Error::Code::RENDER,
                                           "Frame lighting requires a finite nonzero direction, "
                                           "RGB and ambient in [0, 1], and finite nonnegative intensity");
                const double length =
                    std::hypot(static_cast<double>(lighting.direction.x), static_cast<double>(lighting.direction.y),
                               static_cast<double>(lighting.direction.z));
                const glm::vec3 direction(glm::dvec3(lighting.direction) / length);
                std::array<glm::mat4, MAX_SHADOW_CASCADES> cascadeMatrices{
                    glm::mat4(1), glm::mat4(1), glm::mat4(1), glm::mat4(1)};
                glm::vec4 cascadeSplits(0.0f);
                glm::vec4 shadowParams(0.0f);
                glm::vec4 shadowMeta(0.0f);
                if (input.shadow)
                {
                    const auto& shadow = *input.shadow;
                    if (shadow.cascadeCount == 0u || shadow.cascadeCount > MAX_SHADOW_CASCADES)
                        throw Error::Exception(Error::Code::RENDER,
                                               "Shadow cascadeCount must be in [1, MAX_SHADOW_CASCADES]");
                    if (!std::has_single_bit(shadow.resolution) || shadow.resolution < 512u ||
                        shadow.resolution > 4096u)
                        throw Error::Exception(Error::Code::RENDER,
                                               "Shadow resolution must be a power of two in [512, 4096]");
                    if (!std::isfinite(shadow.depthBias) || shadow.depthBias < 0)
                        throw Error::Exception(Error::Code::RENDER,
                                               "Shadow receiver depth bias must be finite and nonnegative");
                    if (!std::isfinite(shadow.normalBias) || shadow.normalBias < 0)
                        throw Error::Exception(Error::Code::RENDER,
                                               "Shadow receiver normal bias must be finite and nonnegative");
                    if (!std::isfinite(shadow.maxDistance) || shadow.maxDistance <= 0)
                        throw Error::Exception(Error::Code::RENDER, "Shadow maxDistance must be finite and positive");
                    const auto checkFiniteMat = [](const glm::mat4& m) {
                        for (int column = 0; column < 4; ++column)
                            for (int row = 0; row < 4; ++row)
                                if (!std::isfinite(m[column][row]))
                                    throw Error::Exception(Error::Code::RENDER,
                                                           "Shadow view projection must be finite");
                    };
                    if (shadow.cascadeCount == 1u)
                    {
                        checkFiniteMat(shadow.viewProjection);
                        cascadeMatrices[0] = shadow.viewProjection;
                        cascadeSplits[0] = shadow.cascadeSplits[0] > 0.0f && std::isfinite(shadow.cascadeSplits[0])
                                               ? shadow.cascadeSplits[0]
                                               : shadow.maxDistance;
                    }
                    else
                    {
                        float prevSplit = 0.0f;
                        for (uint32_t i = 0; i < shadow.cascadeCount; ++i)
                        {
                            checkFiniteMat(shadow.cascadeViewProjections[i]);
                            cascadeMatrices[i] = shadow.cascadeViewProjections[i];
                            const float split = shadow.cascadeSplits[i];
                            if (!std::isfinite(split) || split <= prevSplit)
                                throw Error::Exception(Error::Code::RENDER,
                                                       "Shadow cascade splits must be finite and strictly increasing");
                            cascadeSplits[i] = split;
                            prevSplit = split;
                        }
                    }
                    shadowParams =
                        glm::vec4(shadow.depthBias, 1.0f, shadow.normalBias, static_cast<float>(shadow.cascadeCount));
                    shadowMeta = glm::vec4(shadow.maxDistance, static_cast<float>(shadow.resolution), 0.0f, 0.0f);
                }
                glm::vec4 cameraPosition(0.0f);
                const glm::dmat3 linear(input.camera.view);
                const glm::dvec3 translation(input.camera.view[3]);
                bool finiteView = std::isfinite(translation.x) && std::isfinite(translation.y) &&
                                  std::isfinite(translation.z);
                for (int column = 0; column < 3 && finiteView; ++column)
                    for (int row = 0; row < 3 && finiteView; ++row)
                        if (!std::isfinite(linear[column][row]))
                            finiteView = false;
                if (finiteView)
                {
                    const double determinant = glm::determinant(linear);
                    if (std::isfinite(determinant) && determinant != 0.0)
                    {
                        const glm::dvec3 position = -glm::inverse(linear) * translation;
                        if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z))
                            cameraPosition = glm::vec4(glm::vec3(position), 0.0f);
                    }
                }
                std::array<GpuLocalLight, MAX_LOCAL_LIGHTS> packedLocalLights{};
                uint32_t activeLocalCount = 0;
                for (const auto& local : lighting.localLights)
                {
                    if (!local.enabled || local.intensity <= 0.0f)
                        continue;
                    const double dirLen =
                        std::hypot(static_cast<double>(local.direction.x), static_cast<double>(local.direction.y),
                                   static_cast<double>(local.direction.z));
                    const glm::vec3 localDir(glm::dvec3(local.direction) / dirLen);
                    packedLocalLights[activeLocalCount++] = {
                        glm::vec4(local.position, local.range),
                        glm::vec4(local.color, local.intensity),
                        glm::vec4(localDir, static_cast<float>(local.type)),
                        glm::vec4(std::cos(local.innerConeAngle), std::cos(local.outerConeAngle), 0.0f, 0.0f)};
                }
                return {input.camera.view,
                        input.camera.projection,
                        glm::vec4(direction, lighting.intensity),
                        glm::vec4(lighting.color, lighting.ambient),
                        cascadeMatrices,
                        cascadeSplits,
                        shadowParams,
                        shadowMeta,
                        cameraPosition,
                        input.iblEnabled ? glm::vec4(1.0f, maxPrefilteredLod, 0.0f, 0.0f) : glm::vec4(0.0f),
                        glm::uvec4(activeLocalCount, 0u, 0u, 0u),
                        packedLocalLights};
            }

            VulkanBindingSet::VulkanBindingSet(const VulkanDevice& device,
                                               std::span<const std::shared_ptr<VulkanBuffer>> uniformBuffers)
                : device_(device)
            {
                try
                {
                    constexpr std::array<uint8_t, 4> white{255, 255, 255, 255};
                    constexpr std::array<uint8_t, 4> flatNormal{128, 128, 255, 255};
                    shadowFallback_ = VulkanImage::uploadRgba8(device_, {1, 1}, VK_FORMAT_R8G8B8A8_UNORM, white,
                                                               VK_IMAGE_USAGE_SAMPLED_BIT, TextureMipMode::BaseLevel,
                                                               VK_IMAGE_VIEW_TYPE_2D_ARRAY);
                    normalFallback_ = VulkanImage::uploadRgba8(device_, {1, 1}, VK_FORMAT_R8G8B8A8_UNORM, flatNormal,
                                                               VK_IMAGE_USAGE_SAMPLED_BIT);
                    metallicRoughnessFallback_ = VulkanImage::uploadRgba8(
                        device_, {1, 1}, VK_FORMAT_R8G8B8A8_UNORM, white, VK_IMAGE_USAGE_SAMPLED_BIT);
                    const std::array<uint16_t, 4> blackLut{0, 0, 0, 0x3C00u};
                    std::array<uint16_t, 6 * 4> blackCube{};
                    for (uint32_t f = 0; f < 6u; ++f)
                        blackCube[f * 4u + 3u] = 0x3C00u;
                    const std::array<std::span<const uint16_t>, 1> lutMips{blackLut};
                    const std::array<std::span<const uint16_t>, 1> cubeMips{blackCube};
                    iblCubeFallback_ =
                        VulkanImage::uploadRgba16f(device_, {1, 1}, 6u, cubeMips, VK_IMAGE_USAGE_SAMPLED_BIT);
                    iblLutFallback_ =
                        VulkanImage::uploadRgba16f(device_, {1, 1}, 1u, lutMips, VK_IMAGE_USAGE_SAMPLED_BIT);

                    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
                    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampler.maxAnisotropy = 1;
                    checkVk(vkCreateSampler(device_.handle(), &sampler, nullptr, &shadowSampler_),
                            "vkCreateSampler(shadow)");

                    sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
                    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                    sampler.minLod = 0.0f;
                    sampler.maxLod = VK_LOD_CLAMP_NONE;
                    checkVk(vkCreateSampler(device_.handle(), &sampler, nullptr, &iblCubeSampler_),
                            "vkCreateSampler(iblCube)");
                    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                    sampler.maxLod = 0.0f;
                    checkVk(vkCreateSampler(device_.handle(), &sampler, nullptr, &iblLutSampler_),
                            "vkCreateSampler(iblLut)");

                    constexpr std::array<VkDescriptorSetLayoutBinding, 5> frameBindings{
                        {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                         {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                         {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                         {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
                    VkDescriptorSetLayoutCreateInfo layoutInfo{};
                    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                    layoutInfo.bindingCount = uint32_t(frameBindings.size());
                    layoutInfo.pBindings = frameBindings.data();
                    checkVk(vkCreateDescriptorSetLayout(device_.handle(), &layoutInfo, nullptr, &layout_),
                            "vkCreateDescriptorSetLayout");

                    constexpr std::array<VkDescriptorSetLayoutBinding, 5> materialBindings = {{
                        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                    }};
                    layoutInfo.bindingCount = static_cast<uint32_t>(materialBindings.size());
                    layoutInfo.pBindings = materialBindings.data();
                    checkVk(vkCreateDescriptorSetLayout(device_.handle(), &layoutInfo, nullptr, &materialLayout_),
                            "vkCreateDescriptorSetLayout(material)");

                    const std::array<VkDescriptorPoolSize, 2> poolSizes{
                        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uint32_t(uniformBuffers.size())},
                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4u * uint32_t(uniformBuffers.size())}}};
                    VkDescriptorPoolCreateInfo poolInfo{};
                    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                    poolInfo.maxSets = static_cast<uint32_t>(uniformBuffers.size());
                    poolInfo.poolSizeCount = uint32_t(poolSizes.size());
                    poolInfo.pPoolSizes = poolSizes.data();
                    checkVk(vkCreateDescriptorPool(device_.handle(), &poolInfo, nullptr, &pool_),
                            "vkCreateDescriptorPool");

                    std::vector<VkDescriptorSetLayout> layouts(uniformBuffers.size(), layout_);
                    descriptorSets_.resize(uniformBuffers.size());
                    shadowViews_.resize(uniformBuffers.size(), VK_NULL_HANDLE);
                    VkDescriptorSetAllocateInfo allocateInfo{};
                    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                    allocateInfo.descriptorPool = pool_;
                    allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
                    allocateInfo.pSetLayouts = layouts.data();
                    checkVk(vkAllocateDescriptorSets(device_.handle(), &allocateInfo, descriptorSets_.data()),
                            "vkAllocateDescriptorSets");

                    for (size_t index = 0; index < uniformBuffers.size(); ++index)
                    {
                        VkDescriptorBufferInfo bufferInfo{};
                        bufferInfo.buffer = uniformBuffers[index]->handle();
                        bufferInfo.range = uniformBuffers[index]->size();
                        const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                         nullptr,
                                                         descriptorSets_[index],
                                                         0,
                                                         0,
                                                         1,
                                                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                         nullptr,
                                                         &bufferInfo,
                                                         nullptr};
                        vkUpdateDescriptorSets(device_.handle(), 1, &write, 0, nullptr);
                        setShadowImage(uint32_t(index));
                    }
                    setIblImages();
                }
                catch (...)
                {
                    if (pool_ != VK_NULL_HANDLE)
                        vkDestroyDescriptorPool(device_.handle(), pool_, nullptr);
                    if (layout_ != VK_NULL_HANDLE)
                        vkDestroyDescriptorSetLayout(device_.handle(), layout_, nullptr);
                    if (materialLayout_ != VK_NULL_HANDLE)
                        vkDestroyDescriptorSetLayout(device_.handle(), materialLayout_, nullptr);
                    if (shadowSampler_ != VK_NULL_HANDLE)
                        vkDestroySampler(device_.handle(), shadowSampler_, nullptr);
                    if (iblCubeSampler_ != VK_NULL_HANDLE)
                        vkDestroySampler(device_.handle(), iblCubeSampler_, nullptr);
                    if (iblLutSampler_ != VK_NULL_HANDLE)
                        vkDestroySampler(device_.handle(), iblLutSampler_, nullptr);
                    throw;
                }
            }

            VulkanBindingSet::~VulkanBindingSet()
            {
                if (pool_ != VK_NULL_HANDLE)
                    vkDestroyDescriptorPool(device_.handle(), pool_, nullptr);
                if (layout_ != VK_NULL_HANDLE)
                    vkDestroyDescriptorSetLayout(device_.handle(), layout_, nullptr);
                if (materialLayout_ != VK_NULL_HANDLE)
                    vkDestroyDescriptorSetLayout(device_.handle(), materialLayout_, nullptr);
                if (shadowSampler_ != VK_NULL_HANDLE)
                    vkDestroySampler(device_.handle(), shadowSampler_, nullptr);
                if (iblCubeSampler_ != VK_NULL_HANDLE)
                    vkDestroySampler(device_.handle(), iblCubeSampler_, nullptr);
                if (iblLutSampler_ != VK_NULL_HANDLE)
                    vkDestroySampler(device_.handle(), iblLutSampler_, nullptr);
            }

            void VulkanBindingSet::setIblImages(const VulkanImage* irradiance, const VulkanImage* prefiltered,
                                                const VulkanImage* brdfLut)
            {
                const VkDescriptorImageInfo irrInfo{
                    iblCubeSampler_, irradiance ? irradiance->view() : iblCubeFallback_->view(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const VkDescriptorImageInfo preInfo{
                    iblCubeSampler_, prefiltered ? prefiltered->view() : iblCubeFallback_->view(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const VkDescriptorImageInfo lutInfo{
                    iblLutSampler_, brdfLut ? brdfLut->view() : iblLutFallback_->view(),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                for (VkDescriptorSet set : descriptorSets_)
                {
                    const std::array<VkWriteDescriptorSet, 3> writes{
                        {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 2, 0, 1,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &irrInfo, nullptr, nullptr},
                         {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 3, 0, 1,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &preInfo, nullptr, nullptr},
                         {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 4, 0, 1,
                          VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &lutInfo, nullptr, nullptr}}};
                    vkUpdateDescriptorSets(device_.handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0,
                                           nullptr);
                }
            }

            void VulkanBindingSet::setShadowImage(uint32_t frameIndex, const VulkanImage* image)
            {
                const auto view = image ? image->view() : shadowFallback_->view();
                if (shadowViews_[frameIndex] == view)
                    return;
                const VkDescriptorImageInfo info{shadowSampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                 nullptr,
                                                 descriptorSets_[frameIndex],
                                                 1,
                                                 0,
                                                 1,
                                                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                 &info,
                                                 nullptr,
                                                 nullptr};
                vkUpdateDescriptorSets(device_.handle(), 1, &write, 0, nullptr);
                shadowViews_[frameIndex] = view;
            }

            VkDescriptorSetLayout VulkanBindingSet::layout() const noexcept
            {
                return layout_;
            }

            VkDescriptorSetLayout VulkanBindingSet::materialLayout() const noexcept
            {
                return materialLayout_;
            }

            VkDescriptorSet VulkanBindingSet::descriptorSet(uint32_t frameIndex) const noexcept
            {
                return descriptorSets_[frameIndex];
            }

            const VulkanImage& VulkanBindingSet::normalFallback() const noexcept
            {
                return *normalFallback_;
            }

            const VulkanImage& VulkanBindingSet::metallicRoughnessFallback() const noexcept
            {
                return *metallicRoughnessFallback_;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
