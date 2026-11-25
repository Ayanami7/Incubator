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
                /// @brief 判断实例是否已启用 VK_KHR_get_surface_capabilities2 与 VK_EXT_surface_maintenance1。
                bool surfaceMaintenance1Enabled() const noexcept;

            private:
                VkInstance instance_ = VK_NULL_HANDLE;
                VkDebugUtilsMessengerEXT debugMessenger_ = VK_NULL_HANDLE;
                bool surfaceMaintenance1Enabled_ = false;
            };
        }  // namespace Vulkan
    }  // namespace Render
}  // namespace Incubator
