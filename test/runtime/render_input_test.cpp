#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <span>

#include "runtime/core/error/error.h"
#include "runtime/render/vulkan/vulkan_binding_set.h"
#include "runtime/render/vulkan/vulkan_device.h"
#include "runtime/render/vulkan/vulkan_image.h"
#include "runtime/render/vulkan/vulkan_shadow_target.h"
#include "runtime/render/vulkan/vulkan_swapchain.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Render;
    using namespace Incubator::Render::Vulkan;

    TEST(RenderFrameInputTest, FrameUniformPacksOwnedLightingAndUnitPropagationDirection)
    {
        FrameInput input{{800, 600}, {glm::mat4(1), glm::mat4(2)}, {}};
        const auto defaults = makeFrameUniform(input);
        EXPECT_EQ(defaults.lightDirectionIntensity, glm::vec4(0, 0, -1, 1));
        EXPECT_EQ(defaults.lightColorAmbient, glm::vec4(1, 1, 1, 0.1f));
        EXPECT_EQ(defaults.cascadeViewProjections[0], glm::mat4(1));
        EXPECT_EQ(defaults.shadowParams, glm::vec4(0));
        input.shadow = ShadowFrame{glm::mat4(2), 0.002f};
        const auto shadow = makeFrameUniform(input);
        EXPECT_EQ(shadow.cascadeViewProjections[0], glm::mat4(2));
        EXPECT_EQ(shadow.shadowParams, glm::vec4(0.002f, 1, 0, 1));
        EXPECT_EQ(shadow.shadowMeta, glm::vec4(60.0f, 1024.0f, 0.0f, 0.0f));
        input.shadow->viewProjection[0][0] = std::numeric_limits<float>::infinity();
        EXPECT_THROW(makeFrameUniform(input), Error::Exception);
        input.shadow->viewProjection = glm::mat4(1);
        input.shadow->depthBias = -1;
        EXPECT_THROW(makeFrameUniform(input), Error::Exception);
        input.shadow.reset();
        input.lighting = {{3, 0, -4}, {0.25f, 0.5f, 1}, 2, 0.3f};
        const auto packed = makeFrameUniform(input);
        EXPECT_EQ(packed.view, input.camera.view);
        EXPECT_EQ(packed.projection, input.camera.projection);
        EXPECT_NEAR(packed.lightDirectionIntensity.x, 0.6f, 0.00001f);
        EXPECT_EQ(packed.lightDirectionIntensity.y, 0);
        EXPECT_NEAR(packed.lightDirectionIntensity.z, -0.8f, 0.00001f);
        EXPECT_EQ(packed.lightDirectionIntensity.w, 2);
        EXPECT_EQ(packed.lightColorAmbient, glm::vec4(0.25f, 0.5f, 1, 0.3f));
        EXPECT_EQ(input.lighting.direction, glm::vec3(3, 0, -4));
        input.lighting = {};
        EXPECT_EQ(packed.lightDirectionIntensity.w, 2);
        for (const float scale : {0.001f, std::numeric_limits<float>::max()})
        {
            input.lighting.direction = {scale, 0, -scale};
            const auto scaled = makeFrameUniform(input);
            EXPECT_NEAR(glm::length(glm::vec3(scaled.lightDirectionIntensity)), 1, 0.00001f);
            EXPECT_NEAR(scaled.lightDirectionIntensity.x, std::sqrt(0.5f), 0.00001f);
            EXPECT_NEAR(scaled.lightDirectionIntensity.z, -std::sqrt(0.5f), 0.00001f);
        }
        input.lighting.direction = glm::vec3(0);
        EXPECT_THROW(makeFrameUniform(input), Error::Exception);
    }

    TEST(RenderFrameInputTest, FrameUniformDerivesCameraWorldPositionAndFallsBackOnSingularView)
    {
        const glm::vec3 eye(3.0f, -2.0f, 5.0f);
        const glm::mat4 view = glm::lookAtRH(eye, glm::vec3(1.0f, 0.5f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        const auto valid = makeFrameUniform({{800, 600}, {view, glm::mat4(1)}, {}});
        EXPECT_NEAR(valid.cameraPosition.x, eye.x, 1e-4f);
        EXPECT_NEAR(valid.cameraPosition.y, eye.y, 1e-4f);
        EXPECT_NEAR(valid.cameraPosition.z, eye.z, 1e-4f);
        EXPECT_FLOAT_EQ(valid.cameraPosition.w, 0.0f);

        const auto singular = makeFrameUniform({{800, 600}, {glm::mat4(0.0f), glm::mat4(1)}, {}});
        EXPECT_EQ(singular.cameraPosition, glm::vec4(0.0f));
    }

    TEST(RenderFrameInputTest, ShadowFormatRequiresSampledDepthAttachmentAndFixedImageSupport)
    {
        VkFormatProperties format{};
        format.optimalTilingFeatures = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        VkImageFormatProperties image{{1024, 1024, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, 1024 * 1024 * 4};
        EXPECT_NO_THROW(VulkanShadowTarget::validateSupport(VK_FORMAT_D32_SFLOAT, format, image));
        EXPECT_NO_THROW(VulkanShadowTarget::validateSupport(VK_FORMAT_D16_UNORM, format, image));
        for (const auto feature : {VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT})
        {
            auto unsupported = format;
            unsupported.optimalTilingFeatures &= ~feature;
            try
            {
                VulkanShadowTarget::validateSupport(VK_FORMAT_D32_SFLOAT, unsupported, image);
                FAIL() << "Unsupported shadow format was accepted";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::RENDER);
            }
        }
        image.maxExtent.width = 512;
        EXPECT_THROW(VulkanShadowTarget::validateSupport(VK_FORMAT_D32_SFLOAT, format, image), Error::Exception);
        image.maxExtent.width = 1024;
        image.sampleCounts = VK_SAMPLE_COUNT_4_BIT;
        EXPECT_THROW(VulkanShadowTarget::validateSupport(VK_FORMAT_D32_SFLOAT, format, image), Error::Exception);
        image.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        image.maxResourceSize = 1;
        EXPECT_THROW(VulkanShadowTarget::validateSupport(VK_FORMAT_D32_SFLOAT, format, image), Error::Exception);

        // 验证多级联与自定义分辨率支持校验
        VkImageFormatProperties csmImage{{2048, 2048, 1}, 1, 3, VK_SAMPLE_COUNT_1_BIT, 2048ull * 2048 * 3 * 4};
        EXPECT_NO_THROW(VulkanShadowTarget::validateSupport(VK_FORMAT_D32_SFLOAT, format, csmImage, 2048u, 3u));
        csmImage.maxArrayLayers = 2;
        EXPECT_THROW(VulkanShadowTarget::validateSupport(VK_FORMAT_D32_SFLOAT, format, csmImage, 2048u, 3u),
                     Error::Exception);
    }

    TEST(RenderColorTest, SurfaceSelectionHasStablePriorityAndExplicitFallback)
    {
        const VkSurfaceFormatKHR bgra{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        const VkSurfaceFormatKHR rgba{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        const std::array formats = {rgba, bgra};
        EXPECT_EQ(VulkanSwapchain::chooseSurfaceFormat(formats).format, bgra.format);
        EXPECT_EQ(VulkanSwapchain::chooseSurfaceFormat(std::span(&rgba, 1)).format, rgba.format);
        const VkSurfaceFormatKHR any{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        EXPECT_EQ(VulkanSwapchain::chooseSurfaceFormat(std::span(&any, 1)).format, bgra.format);
    }

    TEST(RenderColorTest, SurfaceSelectionRejectsSrgbAttachmentsHdrAndEmptyLists)
    {
        for (const VkSurfaceFormatKHR candidate :
             {VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
              VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
              VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_HDR10_ST2084_EXT}})
        {
            try
            {
                VulkanSwapchain::chooseSurfaceFormat(std::span(&candidate, 1));
                FAIL() << "Unsupported surface was accepted";
            }
            catch (const Error::Exception& error)
            {
                EXPECT_EQ(error.code(), Error::Code::RENDER);
                EXPECT_NE(error.message().find(fmt::format("format {}", static_cast<int>(candidate.format))),
                          std::string::npos);
                EXPECT_NE(error.message().find(fmt::format("colorSpace {}", static_cast<int>(candidate.colorSpace))),
                          std::string::npos);
            }
        }
        EXPECT_THROW(VulkanSwapchain::chooseSurfaceFormat({}), Error::Exception);
    }

    TEST(RenderColorTest, ImageSupportRejectsControlledLimitsBeforeAnyAllocation)
    {
        VkPhysicalDeviceLimits limits{};
        limits.maxImageDimension2D = 4;
        VkFormatProperties format{};
        format.optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        VkImageFormatProperties image{};
        image.maxExtent = {4, 4, 1};
        image.maxMipLevels = image.maxArrayLayers = 1;
        image.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        image.maxResourceSize = 64;
        EXPECT_NO_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, format, image));
        EXPECT_THROW(VulkanImage::validateRgba8Support({5, 1}, limits, format, image), Error::Exception);
        EXPECT_THROW(VulkanImage::validateRgba8Support({1, 5}, limits, format, image), Error::Exception);
        EXPECT_THROW(VulkanImage::validateRgba8Support({0, 1}, limits, format, image), Error::Exception);
        for (auto feature : {VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, VK_FORMAT_FEATURE_TRANSFER_DST_BIT})
        {
            auto unavailable = format;
            unavailable.optimalTilingFeatures &= ~feature;
            EXPECT_THROW(VulkanImage::validateRgba8Support({2, 1}, limits, unavailable, image), Error::Exception);
        }
        for (int kind = 0; kind < 7; ++kind)
        {
            auto unavailable = image;
            if (kind == 0)
                unavailable.maxExtent.width = 1;
            if (kind == 1)
                unavailable.maxExtent.height = 0;
            if (kind == 2)
                unavailable.maxExtent.depth = 0;
            if (kind == 3)
                unavailable.maxMipLevels = 0;
            if (kind == 4)
                unavailable.maxArrayLayers = 0;
            if (kind == 5)
                unavailable.sampleCounts = 0;
            if (kind == 6)
                unavailable.maxResourceSize = 7;
            EXPECT_THROW(VulkanImage::validateRgba8Support({2, 1}, limits, format, unavailable), Error::Exception);
        }
    }

    TEST(RenderColorTest, MipSupportRequiresOnlyUsedFeaturesAndCountsAllLevels)
    {
        VkPhysicalDeviceLimits limits{};
        limits.maxImageDimension2D = 16;
        VkFormatProperties format{};
        format.optimalTilingFeatures = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                       VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                       VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        VkImageFormatProperties image{};
        image.maxExtent = {16, 16, 1};
        image.maxMipLevels = 5;
        image.maxArrayLayers = 1;
        image.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
        image.maxResourceSize = 84;  // 4×4、2×2 和 1×1 的 RGBA8 mip 共 84 字节，而不只是 64 字节的 CPU 载荷。
        EXPECT_NO_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, format, image, 3));
        for (auto feature : {VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_BLIT_SRC_BIT,
                             VK_FORMAT_FEATURE_BLIT_DST_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT})
        {
            auto unavailable = format;
            unavailable.optimalTilingFeatures &= ~feature;
            EXPECT_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, unavailable, image, 3), Error::Exception);
            EXPECT_NO_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, unavailable, image));
            EXPECT_NO_THROW(VulkanImage::validateRgba8Support({1, 1}, limits, unavailable, image, 1));
        }
        auto unavailable = image;
        unavailable.maxMipLevels = 2;
        EXPECT_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, format, unavailable, 3), Error::Exception);
        EXPECT_NO_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, format, unavailable));
        unavailable = image;
        unavailable.maxResourceSize = 83;
        EXPECT_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, format, unavailable, 3), Error::Exception);
        EXPECT_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, format, image, 0), Error::Exception);
        EXPECT_THROW(VulkanImage::validateRgba8Support({4, 4}, limits, format, image, 4), Error::Exception);
    }

    TEST(VulkanDeviceSelectionTest, PrefersDiscreteOverIntegratedVirtualCpuAndFallsBackToSingleIntegratedGpu)
    {
        EXPECT_EQ(selectBestPhysicalDevice({}), std::nullopt);

        const std::array singleIntegrated = {
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_API_VERSION_1_3, true, false, true},
        };
        EXPECT_EQ(selectBestPhysicalDevice(singleIntegrated), 0u);

        const std::array mixed = {
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_CPU, VK_MAKE_API_VERSION(0, 1, 4, 0), true, true, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_MAKE_API_VERSION(0, 1, 4, 0), true, true, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU, VK_MAKE_API_VERSION(0, 1, 4, 0), true, true, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_API_VERSION_1_3, false, false, false},
        };
        EXPECT_EQ(selectBestPhysicalDevice(mixed), 3u);

        const std::array noDiscrete = {
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_OTHER, VK_MAKE_API_VERSION(0, 1, 4, 0), true, true, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_CPU, VK_MAKE_API_VERSION(0, 1, 4, 0), true, true, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU, VK_MAKE_API_VERSION(0, 1, 4, 0), true, true, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_API_VERSION_1_3, false, false, false},
        };
        EXPECT_EQ(selectBestPhysicalDevice(noDiscrete), 3u);
    }

    TEST(VulkanDeviceSelectionTest, BreaksSameTypeTiesByUnifiedQueueMaintenance1TimestampsApiVersionAndOrder)
    {
        const std::array queueTie = {
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_MAKE_API_VERSION(0, 1, 4, 0), false, true, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_API_VERSION_1_3, true, false, false},
        };
        EXPECT_EQ(selectBestPhysicalDevice(queueTie), 1u);

        const std::array maintenanceTie = {
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_MAKE_API_VERSION(0, 1, 4, 0), true, false, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_API_VERSION_1_3, true, true, false},
        };
        EXPECT_EQ(selectBestPhysicalDevice(maintenanceTie), 1u);

        const std::array timestampTie = {
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_MAKE_API_VERSION(0, 1, 4, 0), true, false, false},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_API_VERSION_1_3, true, false, true},
        };
        EXPECT_EQ(selectBestPhysicalDevice(timestampTie), 1u);

        const std::array apiTie = {
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_API_VERSION_1_3, true, false, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_MAKE_API_VERSION(0, 1, 4, 0), true, false, true},
        };
        EXPECT_EQ(selectBestPhysicalDevice(apiTie), 1u);

        const std::array identical = {
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_API_VERSION_1_3, true, false, true},
            PhysicalDeviceCandidate{VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_API_VERSION_1_3, true, false, true},
        };
        EXPECT_EQ(selectBestPhysicalDevice(identical), 0u);
    }

}  // namespace
