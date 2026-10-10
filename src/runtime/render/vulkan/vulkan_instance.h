#pragma once

#include <vulkan/vulkan.h>

#include <vector>

namespace Incubator
{
    namespace Render
    {
        namespace Vulkan
        {
            enum class ValidationMode
            {
                Disabled,
                Optional,
                Required
            };

            class VulkanInstance
            {
            public:
                VulkanInstance(const std::vector<const char*>& requiredExtensions, ValidationMode validationMode);
                ~VulkanInstance();

                VulkanInstance(const VulkanInstance&) = delete;
                VulkanInstance& operator=(const VulkanInstance&) = delete;

                VkInstance handle() const noexcept;
                bool validationEnabled() const noexcept;

            private:
                VkInstance instance_ = VK_NULL_HANDLE;
                VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
