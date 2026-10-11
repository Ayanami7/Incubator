#include "runtime/render/vulkan/vulkan_binding_set.h"

#include <array>

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_buffer.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_sampler.h"

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
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("{} failed with VkResult {}", operation,
                                                           static_cast<int>(result)));
                    }
                }
            }  // namespace

            VulkanBindingSet::VulkanBindingSet(const VulkanDevice& device,
                                               std::span<const std::shared_ptr<VulkanBuffer>> uniformBuffers,
                                               const VulkanImage& texture, const VulkanSampler& sampler)
                : device_(device)
            {
                try
                {
                    constexpr std::array<VkDescriptorSetLayoutBinding, 2> bindings = {{
                        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
                        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                    }};
                    VkDescriptorSetLayoutCreateInfo layoutInfo{};
                    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
                    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
                    layoutInfo.pBindings = bindings.data();
                    checkVk(vkCreateDescriptorSetLayout(device_.handle(), &layoutInfo, nullptr, &layout_),
                            "vkCreateDescriptorSetLayout");

                    const std::array<VkDescriptorPoolSize, 2> poolSizes = {{
                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(uniformBuffers.size())},
                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(uniformBuffers.size())},
                    }};
                    VkDescriptorPoolCreateInfo poolInfo{};
                    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                    poolInfo.maxSets = static_cast<uint32_t>(uniformBuffers.size());
                    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
                    poolInfo.pPoolSizes = poolSizes.data();
                    checkVk(vkCreateDescriptorPool(device_.handle(), &poolInfo, nullptr, &pool_),
                            "vkCreateDescriptorPool");

                    std::vector<VkDescriptorSetLayout> layouts(uniformBuffers.size(), layout_);
                    descriptorSets_.resize(uniformBuffers.size());
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
                        VkDescriptorImageInfo imageInfo{};
                        imageInfo.sampler = sampler.handle();
                        imageInfo.imageView = texture.view();
                        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        const std::array<VkWriteDescriptorSet, 2> writes = {{
                            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[index], 0, 0, 1,
                             VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfo, nullptr},
                            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSets_[index], 1, 0, 1,
                             VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo, nullptr, nullptr},
                        }};
                        vkUpdateDescriptorSets(device_.handle(), static_cast<uint32_t>(writes.size()), writes.data(),
                                               0, nullptr);
                    }
                }
                catch (...)
                {
                    if (pool_ != VK_NULL_HANDLE)
                        vkDestroyDescriptorPool(device_.handle(), pool_, nullptr);
                    if (layout_ != VK_NULL_HANDLE)
                        vkDestroyDescriptorSetLayout(device_.handle(), layout_, nullptr);
                    throw;
                }
            }

            VulkanBindingSet::~VulkanBindingSet()
            {
                if (pool_ != VK_NULL_HANDLE)
                    vkDestroyDescriptorPool(device_.handle(), pool_, nullptr);
                if (layout_ != VK_NULL_HANDLE)
                    vkDestroyDescriptorSetLayout(device_.handle(), layout_, nullptr);
            }

            VkDescriptorSetLayout VulkanBindingSet::layout() const noexcept
            {
                return layout_;
            }

            VkDescriptorSet VulkanBindingSet::descriptorSet(uint32_t frameIndex) const noexcept
            {
                return descriptorSets_[frameIndex];
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
