#include "runtime/render/vulkan/vulkan_device.h"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>
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

                void checkVk(VkResult result, const char* operation)
                {
                    if (result != VK_SUCCESS)
                    {
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("{} failed with VkResult {}", operation,
                                                           static_cast<int>(result)));
                    }
                }

                bool supportsRequiredExtensions(VkPhysicalDevice physicalDevice)
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

                    for (const char* required : REQUIRED_DEVICE_EXTENSIONS)
                    {
                        const bool found = std::any_of(extensions.begin(), extensions.end(), [required](const auto& ext) {
                            return std::string_view(ext.extensionName) == required;
                        });
                        if (!found)
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

                bool findQueueFamilies(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                       QueueFamilyIndices& indices)
                {
                    uint32_t count = 0;
                    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
                    std::vector<VkQueueFamilyProperties> properties(count);
                    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, properties.data());

                    uint32_t firstGraphics = VK_QUEUE_FAMILY_IGNORED;
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
                            return true;
                        }
                        if (graphics && firstGraphics == VK_QUEUE_FAMILY_IGNORED)
                        {
                            firstGraphics = index;
                        }
                        if (present == VK_TRUE && firstPresent == VK_QUEUE_FAMILY_IGNORED)
                        {
                            firstPresent = index;
                        }
                    }

                    indices.graphics = firstGraphics;
                    indices.present = firstPresent;
                    return firstGraphics != VK_QUEUE_FAMILY_IGNORED && firstPresent != VK_QUEUE_FAMILY_IGNORED;
                }

                bool supportsRequiredFeatures(VkPhysicalDevice physicalDevice)
                {
                    VkPhysicalDeviceVulkan13Features features13{};
                    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
                    VkPhysicalDeviceFeatures2 features{};
                    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                    features.pNext = &features13;
                    vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
                    return features13.dynamicRendering == VK_TRUE && features13.synchronization2 == VK_TRUE;
                }

                bool isSuitable(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, QueueFamilyIndices& indices,
                                VkPhysicalDeviceProperties& properties)
                {
                    vkGetPhysicalDeviceProperties(physicalDevice, &properties);
                    if (properties.apiVersion < VK_API_VERSION_1_3 || !supportsRequiredExtensions(physicalDevice) ||
                        !findQueueFamilies(physicalDevice, surface, indices) || !supportsSurface(physicalDevice, surface))
                    {
                        return false;
                    }
                    return supportsRequiredFeatures(physicalDevice);
                }
            }  // namespace

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
                for (VkPhysicalDevice candidate : physicalDevices)
                {
                    QueueFamilyIndices candidateQueues;
                    VkPhysicalDeviceProperties candidateProperties{};
                    if (isSuitable(candidate, surface.handle(), candidateQueues, candidateProperties))
                    {
                        physicalDevice_ = candidate;
                        queueFamilies_ = candidateQueues;
                        properties_ = candidateProperties;
                        break;
                    }
                }

                if (physicalDevice_ == VK_NULL_HANDLE)
                {
                    throw Error::Exception(
                        Error::Code::RENDER,
                        "No Vulkan 1.3 device supports graphics, presentation, swapchain, dynamic rendering and "
                        "synchronization2");
                }

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

                std::vector<const char*> enabledExtensions(REQUIRED_DEVICE_EXTENSIONS.begin(),
                                                           REQUIRED_DEVICE_EXTENSIONS.end());
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
                uint32_t extensionCount = 0;
                checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount, nullptr),
                        "vkEnumerateDeviceExtensionProperties");
                std::vector<VkExtensionProperties> extensions(extensionCount);
                if (extensionCount != 0)
                {
                    checkVk(vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &extensionCount,
                                                                 extensions.data()),
                            "vkEnumerateDeviceExtensionProperties");
                    const bool hasPortabilitySubset =
                        std::any_of(extensions.begin(), extensions.end(), [](const auto& extension) {
                            return std::string_view(extension.extensionName) == VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME;
                        });
                    if (hasPortabilitySubset)
                    {
                        enabledExtensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
                    }
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

                Log::Registry::get("Render.Vulkan").info(
                    "Selected Vulkan device '{}' (API {}.{}; Dynamic Rendering and Synchronization2 enabled; "
                    "graphics queue family {}, present queue family {})",
                    properties_.deviceName, VK_API_VERSION_MAJOR(properties_.apiVersion),
                    VK_API_VERSION_MINOR(properties_.apiVersion), queueFamilies_.graphics, queueFamilies_.present);
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
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
