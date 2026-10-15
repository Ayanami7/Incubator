#include "runtime/render/vulkan/vulkan_instance.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            namespace
            {
                constexpr const char* VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

                void checkVk(VkResult result, std::string_view operation)
                {
                    if (result != VK_SUCCESS)
                    {
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("{} failed with VkResult {}", operation,
                                                           static_cast<int>(result)));
                    }
                }

                template <typename Property, typename EnumerateFunction>
                std::vector<Property> enumerateProperties(EnumerateFunction enumerate, const char* operation)
                {
                    uint32_t count = 0;
                    checkVk(enumerate(&count, nullptr), operation);

                    std::vector<Property> properties(count);
                    if (count != 0)
                    {
                        checkVk(enumerate(&count, properties.data()), operation);
                        properties.resize(count);
                    }
                    return properties;
                }

                bool hasLayer(const std::vector<VkLayerProperties>& layers, const char* name)
                {
                    return std::any_of(layers.begin(), layers.end(), [name](const VkLayerProperties& layer) {
                        return std::string_view(layer.layerName) == name;
                    });
                }

                bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name)
                {
                    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& ext) {
                        return std::string_view(ext.extensionName) == name;
                    });
                }

                VkBool32 VKAPI_CALL debugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* callbackData, void*)
                {
                    const char* message = callbackData->pMessage ? callbackData->pMessage : "(no message)";
                    auto logger = Log::Registry::get("render.vulkan");
                    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
                    {
                        logger.error("{}", message);
                    }
                    else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
                    {
                        logger.warn("{}", message);
                    }
                    else
                    {
                        logger.debug("{}", message);
                    }
                    return VK_FALSE;
                }

                VkDebugUtilsMessengerCreateInfoEXT debugMessengerInfo()
                {
                    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
                    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
                    createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
                    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
                    createInfo.pfnUserCallback = debugMessage;
                    return createInfo;
                }
            }  // namespace

            VulkanInstance::VulkanInstance(const std::vector<const char*>& requiredExtensions,
                                           ValidationMode validationMode)
            {
                uint32_t loaderVersion = VK_API_VERSION_1_0;
                checkVk(vkEnumerateInstanceVersion(&loaderVersion), "vkEnumerateInstanceVersion");
                if (loaderVersion < VK_API_VERSION_1_3)
                {
                    throw Error::Exception(Error::Code::RENDER,
                                           fmt::format("Vulkan 1.3 loader required; found {}.{}.{}",
                                                       VK_API_VERSION_MAJOR(loaderVersion),
                                                       VK_API_VERSION_MINOR(loaderVersion),
                                                       VK_API_VERSION_PATCH(loaderVersion)));
                }

                const auto availableExtensions = enumerateProperties<VkExtensionProperties>(
                    [](uint32_t* count, VkExtensionProperties* properties) {
                        return vkEnumerateInstanceExtensionProperties(nullptr, count, properties);
                    },
                    "vkEnumerateInstanceExtensionProperties");
                const auto availableLayers = enumerateProperties<VkLayerProperties>(
                    [](uint32_t* count, VkLayerProperties* properties) {
                        return vkEnumerateInstanceLayerProperties(count, properties);
                    },
                    "vkEnumerateInstanceLayerProperties");

                for (const char* extension : requiredExtensions)
                {
                    if (!hasExtension(availableExtensions, extension))
                    {
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("Required Vulkan instance extension is unavailable: {}",
                                                           extension));
                    }
                }

                const bool validationRequested = validationMode != ValidationMode::Disabled;
                const bool validationLayerAvailable = hasLayer(availableLayers, VALIDATION_LAYER);
                const bool debugUtilsAvailable = hasExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                const bool validationAvailable = validationLayerAvailable && debugUtilsAvailable;
                if (validationRequested && !validationAvailable)
                {
                    auto logger = Log::Registry::get("render.vulkan");
                    const std::string missing = !validationLayerAvailable ? VALIDATION_LAYER :
                                                                          VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
                    if (validationMode == ValidationMode::Required)
                    {
                        throw Error::Exception(Error::Code::RENDER,
                                               fmt::format("Required Vulkan validation component is unavailable: {}",
                                                           missing));
                    }
                    logger.warn("Vulkan validation requested but {} is unavailable; continuing without validation",
                                missing);
                }

                const bool enableValidation = validationRequested && validationAvailable;
                std::vector<const char*> enabledExtensions = requiredExtensions;
                if (enableValidation)
                {
                    enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                }

                std::vector<const char*> enabledLayers;
                if (enableValidation)
                {
                    enabledLayers.push_back(VALIDATION_LAYER);
                }

                VkApplicationInfo applicationInfo{};
                applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
                applicationInfo.pApplicationName = "Incubator";
                applicationInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
                applicationInfo.pEngineName = "IncubatorRuntime";
                applicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
                applicationInfo.apiVersion = VK_API_VERSION_1_3;

                VkDebugUtilsMessengerCreateInfoEXT debugInfo = debugMessengerInfo();
                VkInstanceCreateInfo createInfo{};
                createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
                createInfo.pApplicationInfo = &applicationInfo;
                createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size());
                createInfo.ppEnabledExtensionNames = enabledExtensions.data();
                createInfo.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
                createInfo.ppEnabledLayerNames = enabledLayers.data();
                createInfo.pNext = enableValidation ? &debugInfo : nullptr;

                checkVk(vkCreateInstance(&createInfo, nullptr, &instance_), "vkCreateInstance");

                if (enableValidation)
                {
                    const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                        vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
                    if (createMessenger == nullptr)
                    {
                        vkDestroyInstance(instance_, nullptr);
                        instance_ = VK_NULL_HANDLE;
                        throw Error::Exception(Error::Code::RENDER,
                                               "vkCreateDebugUtilsMessengerEXT is unavailable");
                    }

                    const VkResult result = createMessenger(instance_, &debugInfo, nullptr, &debugMessenger_);
                    if (result != VK_SUCCESS)
                    {
                        vkDestroyInstance(instance_, nullptr);
                        instance_ = VK_NULL_HANDLE;
                        checkVk(result, "vkCreateDebugUtilsMessengerEXT");
                    }
                }

                Log::Registry::get("render.vulkan").info("Created Vulkan 1.3 instance (validation: {})",
                                                          enableValidation ? "enabled" : "disabled");
                Log::Registry::get("render.vulkan").debug(
                    "Instance setup: loader {}.{}.{}, {} enabled extensions, {} enabled layers",
                    VK_API_VERSION_MAJOR(loaderVersion), VK_API_VERSION_MINOR(loaderVersion),
                    VK_API_VERSION_PATCH(loaderVersion), enabledExtensions.size(), enabledLayers.size());
            }

            VulkanInstance::~VulkanInstance()
            {
                if (debugMessenger_ != VK_NULL_HANDLE)
                {
                    const auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                        vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
                    if (destroyMessenger != nullptr)
                    {
                        destroyMessenger(instance_, debugMessenger_, nullptr);
                    }
                }
                if (instance_ != VK_NULL_HANDLE)
                {
                    vkDestroyInstance(instance_, nullptr);
                }
            }

            VkInstance VulkanInstance::handle() const noexcept
            {
                return instance_;
            }

            bool VulkanInstance::validationEnabled() const noexcept
            {
                return debugMessenger_ != VK_NULL_HANDLE;
            }
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
