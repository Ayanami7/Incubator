#include "runtime/render/vulkan/vulkan_material_binding.h"

#include <span>

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
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("{} failed with VkResult {}", operation, int(result)));
                }
            }  // namespace

            VulkanMaterialBinding::VulkanMaterialBinding(const VulkanDevice& device, VkDescriptorSetLayout layout,
                                                         const VulkanImage& colorImage,
                                                         const VulkanSampler& colorSampler,
                                                         const VulkanImage& normalImage,
                                                         const VulkanSampler& normalSampler,
                                                         const VulkanImage& metallicRoughnessImage,
                                                         const VulkanSampler& metallicRoughnessSampler,
                                                         const std::array<float, 3>& color, ShadingMode shading,
                                                         float normalScale, float roughness, float metallic,
                                                         MaterialParameterMode roughnessMode,
                                                         MaterialParameterMode metallicMode)
                : device_(device)
            {
                struct MaterialUniformData
                {
                    std::array<float, 4> colorShading;
                    std::array<float, 4> surfaceParams;
                };
                static_assert(sizeof(MaterialUniformData) == 32);
                // std140：colorShading.w 编码 ShadingMode，surfaceParams 依次为 normalScale、roughness、metallic、参数模式位。
                const uint32_t modeBits =
                    (roughnessMode == MaterialParameterMode::FixedValue ? 1u : 0u) |
                    (metallicMode == MaterialParameterMode::FixedValue ? 2u : 0u);
                const MaterialUniformData parameters{
                    {color[0], color[1], color[2], static_cast<float>(shading)},
                    {normalScale, roughness, metallic, static_cast<float>(modeBits)},
                };
                uniform_ = std::make_unique<VulkanBuffer>(
                    device_, sizeof(parameters), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                uniform_->write(0, std::as_bytes(std::span(&parameters, 1)));
                try
                {
                    constexpr std::array<VkDescriptorPoolSize, 2> poolSizes = {{
                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                    }};
                    VkDescriptorPoolCreateInfo poolInfo{};
                    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
                    poolInfo.maxSets = 1;
                    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
                    poolInfo.pPoolSizes = poolSizes.data();
                    checkVk(vkCreateDescriptorPool(device_.handle(), &poolInfo, nullptr, &pool_),
                            "vkCreateDescriptorPool(material)");
                    VkDescriptorSetAllocateInfo allocateInfo{};
                    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                    allocateInfo.descriptorPool = pool_;
                    allocateInfo.descriptorSetCount = 1;
                    allocateInfo.pSetLayouts = &layout;
                    checkVk(vkAllocateDescriptorSets(device_.handle(), &allocateInfo, &descriptorSet_),
                            "vkAllocateDescriptorSets(material)");
                    const VkDescriptorImageInfo colorInfo{colorSampler.handle(), colorImage.view(),
                                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    const VkDescriptorImageInfo normalInfo{normalSampler.handle(), normalImage.view(),
                                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    const VkDescriptorImageInfo mrInfo{metallicRoughnessSampler.handle(),
                                                       metallicRoughnessImage.view(),
                                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    const VkDescriptorBufferInfo bufferInfo{uniform_->handle(), 0, sizeof(parameters)};
                    const std::array<VkWriteDescriptorSet, 4> writes = {{
                        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet_, 0, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &colorInfo, nullptr, nullptr},
                        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet_, 1, 0, 1,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &bufferInfo, nullptr},
                        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet_, 2, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalInfo, nullptr, nullptr},
                        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet_, 3, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mrInfo, nullptr, nullptr},
                    }};
                    vkUpdateDescriptorSets(device_.handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0,
                                           nullptr);
                }
                catch (...)
                {
                    if (pool_ != VK_NULL_HANDLE)
                        vkDestroyDescriptorPool(device_.handle(), pool_, nullptr);
                    throw;
                }
            }

            VulkanMaterialBinding::~VulkanMaterialBinding()
            {
                if (pool_ != VK_NULL_HANDLE)
                    vkDestroyDescriptorPool(device_.handle(), pool_, nullptr);
            }

            VkDescriptorSet VulkanMaterialBinding::descriptorSet() const noexcept
            {
                return descriptorSet_;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
