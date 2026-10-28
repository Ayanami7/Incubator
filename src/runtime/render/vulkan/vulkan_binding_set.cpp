#include "runtime/render/vulkan/vulkan_binding_set.h"

#include <array>
#include <cmath>
#include <cstdint>

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

            VulkanFrameUniform makeFrameUniform(const FrameInput& input)
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
                if (input.shadow)
                {
                    for (int column = 0; column < 4; ++column)
                        for (int row = 0; row < 4; ++row)
                            if (!std::isfinite(input.shadow->viewProjection[column][row]))
                                throw Error::Exception(Error::Code::RENDER, "Shadow view projection must be finite");
                    if (!std::isfinite(input.shadow->depthBias) || input.shadow->depthBias < 0)
                        throw Error::Exception(Error::Code::RENDER,
                                               "Shadow receiver depth bias must be finite and nonnegative");
                }
                return {input.camera.view,
                        input.camera.projection,
                        glm::vec4(direction, lighting.intensity),
                        glm::vec4(lighting.color, lighting.ambient),
                        input.shadow ? input.shadow->viewProjection : glm::mat4(1),
                        input.shadow ? glm::vec4(input.shadow->depthBias, 1, 0, 0) : glm::vec4(0)};
            }

            VulkanBindingSet::VulkanBindingSet(const VulkanDevice& device,
                                               std::span<const std::shared_ptr<VulkanBuffer>> uniformBuffers)
                : device_(device)
            {
                try
                {
                    constexpr std::array<uint8_t, 4> white{255, 255, 255, 255};
                    shadowFallback_ = VulkanImage::uploadRgba8(device_, {1, 1}, VK_FORMAT_R8G8B8A8_UNORM, white,
                                                               VK_IMAGE_USAGE_SAMPLED_BIT);
                    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
                    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
                    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW =
                        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                    sampler.maxAnisotropy = 1;
                    checkVk(vkCreateSampler(device_.handle(), &sampler, nullptr, &shadowSampler_),
                            "vkCreateSampler(shadow)");
                    constexpr std::array<VkDescriptorSetLayoutBinding, 2> frameBindings{
                        {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                         {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}}};
                    VkDescriptorSetLayoutCreateInfo layoutInfo{};
                    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                    layoutInfo.bindingCount = uint32_t(frameBindings.size());
                    layoutInfo.pBindings = frameBindings.data();
                    checkVk(vkCreateDescriptorSetLayout(device_.handle(), &layoutInfo, nullptr, &layout_),
                            "vkCreateDescriptorSetLayout");

                    constexpr std::array<VkDescriptorSetLayoutBinding, 2> materialBindings = {{
                        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                    }};
                    layoutInfo.bindingCount = static_cast<uint32_t>(materialBindings.size());
                    layoutInfo.pBindings = materialBindings.data();
                    checkVk(vkCreateDescriptorSetLayout(device_.handle(), &layoutInfo, nullptr, &materialLayout_),
                            "vkCreateDescriptorSetLayout(material)");

                    const std::array<VkDescriptorPoolSize, 2> poolSizes{
                        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uint32_t(uniformBuffers.size())},
                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(uniformBuffers.size())}}};
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
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
