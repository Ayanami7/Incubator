#include "runtime/render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/render/vulkan/glfw_surface.h"
#include "runtime/render/vulkan/vulkan_instance.h"

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            namespace
            {
                constexpr std::array<const char*, 1> REQUIRED_DEVICE_EXTENSIONS = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
                constexpr std::array<VkFormat, 2> CANDIDATE_DEPTH_FORMATS = {VK_FORMAT_D32_SFLOAT,
                                                                             VK_FORMAT_D16_UNORM};
                constexpr VkFormat HDR_SCENE_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

                void checkVk(VkResult result, const char* operation)
                {
                    if (result != VK_SUCCESS)
                    {
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("{} failed with VkResult {}", operation,
                                                           static_cast<int>(result)));
                    }
                }

                const char* deviceTypeName(VkPhysicalDeviceType type) noexcept
                {
                    switch (type)
                    {
                        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                            return "Discrete GPU";
                        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                            return "Integrated GPU";
                        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                            return "Virtual GPU";
                        case VK_PHYSICAL_DEVICE_TYPE_CPU:
                            return "CPU";
                        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
                        default:
                            return "Other";
                    }
                }

                int deviceTypePriority(VkPhysicalDeviceType type) noexcept
                {
                    switch (type)
                    {
                        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
                            return 4;
                        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
                            return 3;
                        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
                            return 2;
                        case VK_PHYSICAL_DEVICE_TYPE_CPU:
                            return 1;
                        case VK_PHYSICAL_DEVICE_TYPE_OTHER:
                        default:
                            return 0;
                    }
                }

                std::vector<VkExtensionProperties> enumerateDeviceExtensions(VkPhysicalDevice physicalDevice)
                {
                    uint32_t count = 0;
                    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr),
                            "vkEnumerateDeviceExtensionProperties");
                    std::vector<VkExtensionProperties> extensions(count);
                    if (count != 0)
                    {
                        checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count,
                                                                     extensions.data()),
                                "vkEnumerateDeviceExtensionProperties");
                        extensions.resize(count);
                    }
                    return extensions;
                }

                bool hasDeviceExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
                {
                    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& ext) {
                        return std::string_view(ext.extensionName) == name;
                    });
                }

                bool supportsRequiredExtensions(const std::vector<VkExtensionProperties>& extensions)
                {
                    for (const char* required : REQUIRED_DEVICE_EXTENSIONS)
                    {
                        if (!hasDeviceExtension(extensions, required))
                        {
                            return false;
                        }
                    }
                    return true;
                }

                bool supportsSurface(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface)
                {
                    VkSurfaceCapabilitiesKHR capabilities{};
                    checkVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities),
                            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

                    uint32_t formatCount = 0;
                    checkVk(vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &formatCount, nullptr),
                            "vkGetPhysicalDeviceSurfaceFormatsKHR");
                    if (formatCount == 0)
                    {
                        return false;
                    }

                    uint32_t presentModeCount = 0;
                    checkVk(vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &presentModeCount,
                                                                      nullptr),
                            "vkGetPhysicalDeviceSurfacePresentModesKHR");
                    return presentModeCount != 0;
                }

                bool supportsRequiredFormats(VkPhysicalDevice physicalDevice)
                {
                    const bool hasDepth = std::any_of(
                        CANDIDATE_DEPTH_FORMATS.begin(), CANDIDATE_DEPTH_FORMATS.end(),
                        [physicalDevice](VkFormat format) {
                            VkFormatProperties properties{};
                            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
                            return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) !=
                                   0;
                        });
                    if (!hasDepth)
                    {
                        return false;
                    }

                    VkFormatProperties hdrProperties{};
                    vkGetPhysicalDeviceFormatProperties(physicalDevice, HDR_SCENE_FORMAT, &hdrProperties);
                    constexpr VkFormatFeatureFlags requiredHdrFeatures =
                        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
                    return (hdrProperties.optimalTilingFeatures & requiredHdrFeatures) == requiredHdrFeatures;
                }

                bool findQueueFamilies(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                       QueueFamilyIndices& indices)
                {
                    uint32_t count = 0;
                    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
                    std::vector<VkQueueFamilyProperties> properties(count);
                    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, properties.data());

                    uint32_t firstGraphics = VK_QUEUE_FAMILY_IGNORED;
                    uint32_t firstGraphicsTimestampValidBits = 0;
                    uint32_t firstPresent = VK_QUEUE_FAMILY_IGNORED;
                    for (uint32_t index = 0; index < count; ++index)
                    {
                        if (properties[index].queueCount == 0)
                        {
                            continue;
                        }

                        const bool graphics = (properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
                        VkBool32 present = VK_FALSE;
                        checkVk(vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, index, surface, &present),
                                "vkGetPhysicalDeviceSurfaceSupportKHR");

                        if (graphics && present == VK_TRUE)
                        {
                            indices.graphics = index;
                            indices.present = index;
                            indices.graphicsTimestampValidBits = properties[index].timestampValidBits;
                            return true;
                        }
                        if (graphics && firstGraphics == VK_QUEUE_FAMILY_IGNORED)
                        {
                            firstGraphics = index;
                            firstGraphicsTimestampValidBits = properties[index].timestampValidBits;
                        }
                        if (present == VK_TRUE && firstPresent == VK_QUEUE_FAMILY_IGNORED)
                        {
                            firstPresent = index;
                        }
                    }

                    indices.graphics = firstGraphics;
                    indices.present = firstPresent;
                    indices.graphicsTimestampValidBits = firstGraphicsTimestampValidBits;
                    return firstGraphics != VK_QUEUE_FAMILY_IGNORED && firstPresent != VK_QUEUE_FAMILY_IGNORED;
                }

                struct EvaluatedDevice
                {
                    VkPhysicalDevice device = VK_NULL_HANDLE;
                    QueueFamilyIndices queues;
                    VkPhysicalDeviceProperties properties{};
                    bool swapchainMaintenance1 = false;
                    bool portabilitySubset = false;
                    PhysicalDeviceCandidate summary{};
                };

                bool evaluatePhysicalDevice(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                            bool instanceSurfaceMaintenance1, EvaluatedDevice& evaluated,
                                            std::string& rejectReason)
                {
                    vkGetPhysicalDeviceProperties(physicalDevice, &evaluated.properties);
                    if (evaluated.properties.apiVersion < VK_API_VERSION_1_3)
                    {
                        rejectReason = fmt::format("API {}.{} < 1.3",
                                                   VK_API_VERSION_MAJOR(evaluated.properties.apiVersion),
                                                   VK_API_VERSION_MINOR(evaluated.properties.apiVersion));
                        return false;
                    }

                    const auto extensions = enumerateDeviceExtensions(physicalDevice);
                    if (!supportsRequiredExtensions(extensions))
                    {
                        rejectReason = "missing VK_KHR_swapchain";
                        return false;
                    }
                    if (!findQueueFamilies(physicalDevice, surface, evaluated.queues))
                    {
                        rejectReason = "missing graphics or present queue family";
                        return false;
                    }
                    if (!supportsSurface(physicalDevice, surface))
                    {
                        rejectReason = "no supported surface formats or present modes";
                        return false;
                    }
                    if (!supportsRequiredFormats(physicalDevice))
                    {
                        rejectReason = "missing required depth or R16G16B16A16_SFLOAT HDR format support";
                        return false;
                    }

                    const bool hasMaintenance1Ext =
                        instanceSurfaceMaintenance1 &&
                        hasDeviceExtension(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
                    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance1{};
                    maintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
                    VkPhysicalDeviceVulkan13Features features13{};
                    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
                    features13.pNext = hasMaintenance1Ext ? &maintenance1 : nullptr;
                    VkPhysicalDeviceFeatures2 features{};
                    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    features.pNext = &features13;
                    vkGetPhysicalDeviceFeatures2(physicalDevice, &features);

                    if (features13.dynamicRendering != VK_TRUE || features13.synchronization2 != VK_TRUE)
                    {
                        rejectReason = "missing dynamicRendering or synchronization2 feature";
                        return false;
                    }

                    evaluated.device = physicalDevice;
                    evaluated.swapchainMaintenance1 = hasMaintenance1Ext && maintenance1.swapchainMaintenance1 == VK_TRUE;
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
                    evaluated.portabilitySubset =
                        hasDeviceExtension(extensions, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
                    evaluated.summary = PhysicalDeviceCandidate{
                        evaluated.properties.deviceType, evaluated.properties.apiVersion,
                        evaluated.queues.graphics == evaluated.queues.present, evaluated.swapchainMaintenance1,
                        evaluated.queues.graphicsTimestampValidBits > 0 &&
                            evaluated.properties.limits.timestampPeriod > 0.0f};
                    return true;
                }
            }  // namespace

            std::optional<size_t> selectBestPhysicalDevice(std::span<const PhysicalDeviceCandidate> candidates) noexcept
            {
                if (candidates.empty())
                {
                    return std::nullopt;
                }
                const auto rank = [](const PhysicalDeviceCandidate& candidate) {
                    return std::make_tuple(deviceTypePriority(candidate.deviceType),
                                           candidate.unifiedGraphicsPresentQueue, candidate.swapchainMaintenance1,
                                           candidate.graphicsTimestamps, candidate.apiVersion);
                };
                size_t bestIndex = 0;
                auto bestRank = rank(candidates[0]);
                for (size_t index = 1; index < candidates.size(); ++index)
                {
                    const auto currentRank = rank(candidates[index]);
                    if (currentRank > bestRank)
                    {
                        bestRank = currentRank;
                        bestIndex = index;
                    }
                }
                return bestIndex;
            }

            VulkanDevice::VulkanDevice(const VulkanInstance& instance, const GlfwSurface& surface)
            {
                uint32_t physicalDeviceCount = 0;
                checkVk(vkEnumeratePhysicalDevices(instance.handle(), &physicalDeviceCount, nullptr),
                        "vkEnumeratePhysicalDevices");
                if (physicalDeviceCount == 0)
                {
                    throw Error::Exception(Error::Code::RENDER, "No Vulkan physical devices were found");
                }

                std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
                checkVk(vkEnumeratePhysicalDevices(instance.handle(), &physicalDeviceCount, physicalDevices.data()),
                        "vkEnumeratePhysicalDevices");

                auto logger = Log::Registry::get("render.vulkan");
                std::vector<EvaluatedDevice> suitableDevices;
                std::vector<PhysicalDeviceCandidate> summaries;
                std::vector<std::string> rejections;
                for (VkPhysicalDevice candidate : physicalDevices)
                {
                    EvaluatedDevice evaluated{};
                    std::string rejectReason;
                    if (evaluatePhysicalDevice(candidate, surface.handle(), instance.surfaceMaintenance1Enabled(),
                                               evaluated, rejectReason))
                    {
                        logger.debug("Candidate Vulkan device '{}' ({}, API {}.{}, swapchainMaintenance1: {})",
                                     evaluated.properties.deviceName, deviceTypeName(evaluated.properties.deviceType),
                                     VK_API_VERSION_MAJOR(evaluated.properties.apiVersion),
                                     VK_API_VERSION_MINOR(evaluated.properties.apiVersion),
                                     evaluated.swapchainMaintenance1 ? "supported" : "unsupported");
                        summaries.push_back(evaluated.summary);
                        suitableDevices.push_back(evaluated);
                    }
                    else
                    {
                        const std::string detail =
                            fmt::format("'{}' ({}): {}", evaluated.properties.deviceName,
                                        deviceTypeName(evaluated.properties.deviceType), rejectReason);
                        logger.debug("Rejected Vulkan device {}", detail);
                        rejections.push_back(detail);
                    }
                }

                const auto bestIndex = selectBestPhysicalDevice(summaries);
                if (!bestIndex.has_value())
                {
                    std::string summary;
                    for (size_t index = 0; index < rejections.size(); ++index)
                    {
                        if (index != 0)
                            summary += "; ";
                        summary += rejections[index];
                    }
                    throw Error::Exception(
                        Error::Code::RENDER,
                        fmt::format("No suitable Vulkan 1.3 device (enumerated {}): {}", physicalDevices.size(),
                                    summary));
                }

                const EvaluatedDevice& selected = suitableDevices[*bestIndex];
                physicalDevice_ = selected.device;
                queueFamilies_ = selected.queues;
                properties_ = selected.properties;
                swapchainMaintenance1Enabled_ = selected.swapchainMaintenance1;

                const std::set<uint32_t> uniqueFamilies = {queueFamilies_.graphics, queueFamilies_.present};
                constexpr float QUEUE_PRIORITY = 1.0f;
                std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
                queueCreateInfos.reserve(uniqueFamilies.size());
                for (uint32_t family : uniqueFamilies)
                {
                    VkDeviceQueueCreateInfo queueInfo{};
                    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                    queueInfo.queueFamilyIndex = family;
                    queueInfo.queueCount = 1;
                    queueInfo.pQueuePriorities = &QUEUE_PRIORITY;
                    queueCreateInfos.push_back(queueInfo);
                }

                VkPhysicalDeviceVulkan13Features enabledFeatures{};
                enabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
                enabledFeatures.dynamicRendering = VK_TRUE;
                enabledFeatures.synchronization2 = VK_TRUE;
                VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT enabledMaintenance1{};
                if (swapchainMaintenance1Enabled_)
                {
                    enabledMaintenance1.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
                    enabledMaintenance1.swapchainMaintenance1 = VK_TRUE;
                    enabledFeatures.pNext = &enabledMaintenance1;
                }

                std::vector<const char*> enabledExtensions(REQUIRED_DEVICE_EXTENSIONS.begin(),
                                                           REQUIRED_DEVICE_EXTENSIONS.end());
                if (swapchainMaintenance1Enabled_)
                {
                    enabledExtensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
                }
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
                if (selected.portabilitySubset)
                {
                    enabledExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
                }
#endif

                VkDeviceCreateInfo createInfo{};
                createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
                createInfo.pNext = &enabledFeatures;
                createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
                createInfo.pQueueCreateInfos = queueCreateInfos.data();
                createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
                createInfo.ppEnabledExtensionNames = enabledExtensions.data();
                checkVk(vkCreateDevice(physicalDevice_, &createInfo, nullptr, &device_), "vkCreateDevice");

                vkGetDeviceQueue(device_, queueFamilies_.graphics, 0, &graphicsQueue_);
                vkGetDeviceQueue(device_, queueFamilies_.present, 0, &presentQueue_);
                vkGetPhysicalDeviceMemoryProperties(physicalDevice_, &memoryProperties_);

                logger.info(
                    "Selected Vulkan device '{}' ({}, API {}.{}; Dynamic Rendering and Synchronization2 enabled; "
                    "swapchainMaintenance1: {}; graphics queue family {}, present queue family {})",
                    properties_.deviceName, deviceTypeName(properties_.deviceType),
                    VK_API_VERSION_MAJOR(properties_.apiVersion), VK_API_VERSION_MINOR(properties_.apiVersion),
                    swapchainMaintenance1Enabled_ ? "enabled" : "fallback(queueWaitIdle)", queueFamilies_.graphics,
                    queueFamilies_.present);
                logger.debug(
                    "Device limits: maxImageDimension2D={}, maxUniformBufferRange={}, nonCoherentAtomSize={}",
                    properties_.limits.maxImageDimension2D, properties_.limits.maxUniformBufferRange,
                    properties_.limits.nonCoherentAtomSize);
            }

            VulkanDevice::~VulkanDevice()
            {
                if (device_ != VK_NULL_HANDLE)
                {
                    vkDestroyDevice(device_, nullptr);
                }
            }

            VkPhysicalDevice VulkanDevice::physicalDevice() const noexcept
            {
                return physicalDevice_;
            }

            VkDevice VulkanDevice::handle() const noexcept
            {
                return device_;
            }

            VkQueue VulkanDevice::graphicsQueue() const noexcept
            {
                return graphicsQueue_;
            }

            VkQueue VulkanDevice::presentQueue() const noexcept
            {
                return presentQueue_;
            }

            const QueueFamilyIndices& VulkanDevice::queueFamilies() const noexcept
            {
                return queueFamilies_;
            }

            const VkPhysicalDeviceProperties& VulkanDevice::properties() const noexcept
            {
                return properties_;
            }

            bool VulkanDevice::supportsGraphicsTimestamps() const noexcept
            {
                return queueFamilies_.graphicsTimestampValidBits > 0 && properties_.limits.timestampPeriod > 0.0f;
            }

            bool VulkanDevice::supportsSwapchainMaintenance1() const noexcept
            {
                return swapchainMaintenance1Enabled_;
            }

            uint32_t VulkanDevice::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags required,
                                                   VkMemoryPropertyFlags preferred,
                                                   VkMemoryPropertyFlags* selectedProperties) const
            {
                uint32_t fallback = VK_MAX_MEMORY_TYPES;
                VkMemoryPropertyFlags fallbackProperties = 0;
                for (uint32_t index = 0; index < memoryProperties_.memoryTypeCount; ++index)
                {
                    if ((typeFilter & (1u << index)) == 0)
                        continue;
                    const VkMemoryPropertyFlags properties = memoryProperties_.memoryTypes[index].propertyFlags;
                    if ((properties & required) != required)
                        continue;
                    if ((properties & preferred) == preferred)
                    {
                        if (selectedProperties)
                            *selectedProperties = properties;
                        return index;
                    }
                    if (fallback == VK_MAX_MEMORY_TYPES)
                    {
                        fallback = index;
                        fallbackProperties = properties;
                    }
                }
                if (fallback != VK_MAX_MEMORY_TYPES)
                {
                    if (selectedProperties)
                        *selectedProperties = fallbackProperties;
                    return fallback;
                }
                throw Error::Exception(Error::Code::RENDER,
                                       "No compatible Vulkan memory type satisfies the requested properties");
            }

            VkFormat VulkanDevice::findDepthFormat() const
            {
                constexpr VkFormat CANDIDATE_FORMATS[] = {
                    VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
                for (VkFormat format : CANDIDATE_FORMATS)
                {
                    VkFormatProperties properties{};
                    vkGetPhysicalDeviceFormatProperties(physicalDevice_, format, &properties);
                    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0)
                        return format;
                }
                throw Error::Exception(Error::Code::RENDER,
                                       "The selected Vulkan device has no supported depth attachment format");
            }

            void VulkanDevice::executeImmediate(const std::function<void(VkCommandBuffer)>& recordCommands) const
            {
                VkCommandPool commandPool = VK_NULL_HANDLE;
                VkFence completionFence = VK_NULL_HANDLE;
                bool submitted = false;
                try
                {
                    VkCommandPoolCreateInfo poolInfo{};
                    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
                    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
                    poolInfo.queueFamilyIndex = queueFamilies_.graphics;
                    checkVk(vkCreateCommandPool(device_, &poolInfo, nullptr, &commandPool), "vkCreateCommandPool");

                    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
                    VkCommandBufferAllocateInfo allocateInfo{};
                    allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
                    allocateInfo.commandPool = commandPool;
                    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
                    allocateInfo.commandBufferCount = 1;
                    checkVk(vkAllocateCommandBuffers(device_, &allocateInfo, &commandBuffer),
                            "vkAllocateCommandBuffers");
                    VkCommandBufferBeginInfo beginInfo{};
                    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    checkVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");
                    recordCommands(commandBuffer);
                    checkVk(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");

                    VkFenceCreateInfo fenceInfo{};
                    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                    checkVk(vkCreateFence(device_, &fenceInfo, nullptr, &completionFence), "vkCreateFence");
                    VkSubmitInfo submitInfo{};
                    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    submitInfo.commandBufferCount = 1;
                    submitInfo.pCommandBuffers = &commandBuffer;
                    checkVk(vkQueueSubmit(graphicsQueue_, 1, &submitInfo, completionFence), "vkQueueSubmit(upload)");
                    submitted = true;
                    checkVk(vkWaitForFences(device_, 1, &completionFence, VK_TRUE,
                                            std::numeric_limits<uint64_t>::max()),
                            "vkWaitForFences(upload)");
                }
                catch (...)
                {
                    if (submitted)
                        vkQueueWaitIdle(graphicsQueue_);
                    if (completionFence != VK_NULL_HANDLE)
                        vkDestroyFence(device_, completionFence, nullptr);
                    if (commandPool != VK_NULL_HANDLE)
                        vkDestroyCommandPool(device_, commandPool, nullptr);
                    throw;
                }
                vkDestroyFence(device_, completionFence, nullptr);
                vkDestroyCommandPool(device_, commandPool, nullptr);
            }

        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
