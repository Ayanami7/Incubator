#include "vulkan_review_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Render;
    using namespace Incubator::Render::Vulkan;
    using namespace Incubator::Test;

    class VulkanPostprocessTest : public VulkanReviewTest
    {};


    TEST_F(VulkanPostprocessTest, LinearHdrSceneTargetPreservesSuperBrightValuesAndDepth)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            const std::array<MeshVertex, 4> quadVertices{
                MeshVertex{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{-0.5f, 0.5f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}};
            const std::array<uint32_t, 6> quadIndices{0, 1, 2, 0, 2, 3};
            const auto quad = renderer.createMesh(quadVertices, quadIndices);

            MaterialDesc leftDesc{white, {0.5f, 0.4f, 0.35f}};
            leftDesc.shading = ShadingMode::Lambert;
            const auto leftMat = renderer.createMaterial(leftDesc);

            MaterialDesc rightDesc{white, {1.0f, 0.8f, 0.5f}};
            rightDesc.shading = ShadingMode::Lambert;
            const auto rightMat = renderer.createMaterial(rightDesc);

            MaterialDesc occluderDesc{white, {0.2f, 0.2f, 0.2f}};
            occluderDesc.shading = ShadingMode::Unlit;
            const auto occluderMat = renderer.createMaterial(occluderDesc);

            const Lighting brightLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 4.0f, 0.0f};
            constexpr FrameExtent EXTENT{128, 64};
            const std::array<float, 4> clearColor{0.1f, 0.2f, 0.3f, 0.0f};

            // 1. 两个超亮样本在 VK_FORMAT_R16G16B16A16_SFLOAT 中保留各自大于 1.0 的线性值，背景 alpha 为 0。
            const std::array draws{
                DrawItem{quad, leftMat, glm::translate(glm::mat4(1.0f), {-0.5f, 0.0f, 0.5f}) *
                                            glm::scale(glm::mat4(1.0f), {0.4f, 0.6f, 1.0f})},
                DrawItem{quad, rightMat, glm::translate(glm::mat4(1.0f), {0.5f, 0.0f, 0.5f}) *
                                             glm::scale(glm::mat4(1.0f), {0.4f, 0.6f, 1.0f})}};
            const FrameInput hdrInput{EXTENT, {glm::mat4(1.0f), glm::mat4(1.0f)}, draws, brightLight};
            std::vector<std::array<float, 4>> hdrPixels;
            Detail::RendererReviewAccess::withHdrSceneTarget(
                renderer, hdrInput, clearColor, [&](const auto& device, const auto& image) {
                    EXPECT_EQ(image.format(), VK_FORMAT_R16G16B16A16_SFLOAT);
                    hdrPixels = TextureGpuReview::captureRgba16f(device, image, {EXTENT.width, EXTENT.height});
                });
            ASSERT_EQ(hdrPixels.size(), size_t(EXTENT.width) * EXTENT.height);

            const auto& leftPx = hdrPixels[size_t(32) * EXTENT.width + 32];
            const auto& rightPx = hdrPixels[size_t(32) * EXTENT.width + 96];
            const auto& bgPx = hdrPixels[size_t(32) * EXTENT.width + 64];

            for (int c = 0; c < 3; ++c)
            {
                EXPECT_GT(leftPx[c], 1.0f);
                EXPECT_GT(rightPx[c], leftPx[c] + 0.5f);
            }
            EXPECT_NEAR(leftPx[0], 2.0f, 0.01f);
            EXPECT_NEAR(leftPx[1], 1.6f, 0.01f);
            EXPECT_NEAR(leftPx[2], 1.4f, 0.01f);
            EXPECT_NEAR(leftPx[3], 1.0f, 0.001f);

            EXPECT_NEAR(rightPx[0], 4.0f, 0.01f);
            EXPECT_NEAR(rightPx[1], 3.2f, 0.01f);
            EXPECT_NEAR(rightPx[2], 2.0f, 0.01f);
            EXPECT_NEAR(rightPx[3], 1.0f, 0.001f);

            EXPECT_NEAR(bgPx[0], clearColor[0], 0.005f);
            EXPECT_NEAR(bgPx[1], clearColor[1], 0.005f);
            EXPECT_NEAR(bgPx[2], clearColor[2], 0.005f);
            EXPECT_NEAR(bgPx[3], 0.0f, 0.001f);

            // 2. 验证 HDR 场景目标深度测试生效（更近的 Unlit 遮挡物覆盖后方超亮方块）。
            const std::array depthDraws{
                DrawItem{quad, rightMat, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.8f}) *
                                             glm::scale(glm::mat4(1.0f), {0.6f, 0.6f, 1.0f})},
                DrawItem{quad, occluderMat, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.2f}) *
                                                glm::scale(glm::mat4(1.0f), {0.4f, 0.4f, 1.0f})}};
            Detail::RendererReviewAccess::withHdrSceneTarget(
                renderer, FrameInput{EXTENT, {glm::mat4(1.0f), glm::mat4(1.0f)}, depthDraws, brightLight}, clearColor,
                [&](const auto& device, const auto& image) {
                    hdrPixels = TextureGpuReview::captureRgba16f(device, image, {EXTENT.width, EXTENT.height});
                });
            const auto& occludedPx = hdrPixels[size_t(32) * EXTENT.width + 64];
            EXPECT_NEAR(occludedPx[0], 0.2f, 0.01f);
            EXPECT_NEAR(occludedPx[1], 0.2f, 0.01f);
            EXPECT_NEAR(occludedPx[2], 0.2f, 0.01f);
            EXPECT_NEAR(occludedPx[3], 1.0f, 0.001f);

            // 3. 验证同 Renderer 下现有 drawFrame LDR 路径不受影响（含 Unlit/Lambert/Pbr 三种模式）。
            auto orthoProj = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
            orthoProj[1][1] *= -1.0f;
            const CameraMatrices pbrCamera{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)), orthoProj};
            MaterialDesc pbrDesc{white, {0.5f, 0.5f, 0.5f}};
            pbrDesc.shading = ShadingMode::Pbr;
            pbrDesc.metallic = 0.0f;
            pbrDesc.roughness = 0.5f;
            const auto pbrMat = renderer.createMaterial(pbrDesc);
            const std::array pbrDraw{DrawItem{quad, pbrMat, glm::mat4(1.0f)}};
            Detail::RendererReviewAccess::withFrame(
                renderer,
                FrameInput{{384, 256}, pbrCamera, pbrDraw, {{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.6f, 0.1f}},
                [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                    const auto ldrPx = TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws);
                    expectLinearPixel(ldrPx, glm::vec3(0.434f));
                });
        }
        expectCleanValidation();
    }

    TEST_F(VulkanPostprocessTest, DisplayPassToneMappingExposureBackgroundAndSlotResizeValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            const std::array<MeshVertex, 4> quadVertices{
                MeshVertex{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
                MeshVertex{{-0.5f, 0.5f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}}};
            const std::array<uint32_t, 6> quadIndices{0, 1, 2, 0, 2, 3};
            const auto quad = renderer.createMesh(quadVertices, quadIndices);

            MaterialDesc leftDesc{white, {0.5f, 0.25f, 0.1f}};
            leftDesc.shading = ShadingMode::Lambert;
            const auto leftMat = renderer.createMaterial(leftDesc);

            MaterialDesc rightDesc{white, {1.0f, 0.8f, 0.5f}};
            rightDesc.shading = ShadingMode::Lambert;
            const auto rightMat = renderer.createMaterial(rightDesc);

            const std::array draws{
                DrawItem{quad, leftMat, glm::translate(glm::mat4(1.0f), {-0.5f, 0.0f, 0.5f}) *
                                            glm::scale(glm::mat4(1.0f), {0.4f, 0.6f, 1.0f})},
                DrawItem{quad, rightMat, glm::translate(glm::mat4(1.0f), {0.5f, 0.0f, 0.5f}) *
                                             glm::scale(glm::mat4(1.0f), {0.4f, 0.6f, 1.0f})}};
            const Lighting brightLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 4.0f, 0.0f};
            const std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1.0f};

            // 1. 未准备显示 Shader 时 drawSceneTarget 拒绝执行。
            {
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget({128, 64});
                renderer.imgui().endFrame();
                EXPECT_THROW(renderer.drawSceneTarget(output, {{128, 64}, {glm::mat4(1), glm::mat4(1)}, draws}, clearColor),
                             Error::Exception);
                Detail::RendererReviewAccess::waitForFrames(renderer);
            }

            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            const Resource::ShaderResource emptyShader({}, {});
            EXPECT_THROW(renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment),
                         Error::Exception);
            EXPECT_THROW(renderer.prepareDisplayResources(emptyShader, *assets.displayFragment), Error::Exception);

            // 2. 非法曝光范围校验。
            {
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget({128, 64});
                renderer.imgui().endFrame();
                for (float badEv : {-8.5f, 8.5f, std::numeric_limits<float>::quiet_NaN()})
                {
                    FrameInput badInput{{128, 64}, {glm::mat4(1), glm::mat4(1)}, draws, brightLight};
                    badInput.display = {ToneMappingMode::Reinhard, badEv};
                    EXPECT_THROW(renderer.drawSceneTarget(output, badInput, clearColor), Error::Exception);
                }
                Detail::RendererReviewAccess::waitForFrames(renderer);
            }

            const auto renderAndCapture = [&](FrameExtent extent, std::span<const DrawItem> frameDraws,
                                              DisplaySettings display) {
                const auto uiExtent = renderer.beginUiFrame();
                EXPECT_TRUE(uiExtent);
                const auto output = renderer.prepareSceneTarget(extent);
                ImGui::Begin("Viewport");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(extent.width), float(extent.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* drawData = renderer.imgui().drawData();
                FrameInput input{extent, {glm::mat4(1.0f), glm::mat4(1.0f)}, frameDraws, brightLight};
                input.display = display;
                renderer.drawSceneTarget(output, input, clearColor,
                                         [&](void* cmd) { renderer.imgui().recordDrawData(drawData, cmd); });
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        EXPECT_EQ(image.format(), VK_FORMAT_R8G8B8A8_UNORM);
                        pixels = TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                    });
                return pixels;
            };

            // 3. 空场景直接输出原有 clearColor。
            {
                const auto px = renderAndCapture({128, 64}, {}, {ToneMappingMode::Reinhard, 0.0f});
                const size_t bgOff = (32u * 128u + 64u) * 4u;
                EXPECT_NEAR(px[bgOff + 0], clearColor[0] * 255.0f, 1.5f);
                EXPECT_NEAR(px[bgOff + 1], clearColor[1] * 255.0f, 1.5f);
                EXPECT_NEAR(px[bgOff + 2], clearColor[2] * 255.0f, 1.5f);
                EXPECT_EQ(px[bgOff + 3], 255u);
            }

            // 4. Off 模式：钳制到 [0, 1] 后编码一次 sRGB，EV 不影响输出；未覆盖背景仍为 clearColor。
            {
                const auto offNeg = renderAndCapture({128, 64}, draws, {ToneMappingMode::Off, -4.0f});
                const auto offPos = renderAndCapture({128, 64}, draws, {ToneMappingMode::Off, 4.0f});
                EXPECT_EQ(offNeg, offPos);
                // left = {2.0, 1.0, 0.4} -> clamped = {1.0, 1.0, 0.4}
                expectLinearPixel(offNeg, glm::vec3(1.0f, 1.0f, 0.4f), 32, 32, 128);
                // right = {4.0, 3.2, 2.0} -> clamped = {1.0, 1.0, 1.0}
                expectLinearPixel(offNeg, glm::vec3(1.0f, 1.0f, 1.0f), 96, 32, 128);
                const size_t bgOff = (32u * 128u + 64u) * 4u;
                EXPECT_NEAR(offNeg[bgOff + 0], clearColor[0] * 255.0f, 1.5f);
            }

            // 5. Reinhard 模式：参考像素正确、两个超亮样本可区分，且 +/-1 EV 曝光关系正确。
            {
                const auto reinhard0 = renderAndCapture({128, 64}, draws, {ToneMappingMode::Reinhard, 0.0f});
                const glm::vec3 leftHdr(2.0f, 1.0f, 0.4f);
                const glm::vec3 rightHdr(4.0f, 3.2f, 2.0f);
                expectLinearPixel(reinhard0, leftHdr / (1.0f + leftHdr), 32, 32, 128);
                expectLinearPixel(reinhard0, rightHdr / (1.0f + rightHdr), 96, 32, 128);
                const size_t leftOff = (32u * 128u + 32u) * 4u;
                const size_t rightOff = (32u * 128u + 96u) * 4u;
                EXPECT_GT(reinhard0[rightOff + 0], reinhard0[leftOff + 0] + 10);

                const auto reinhardPlus1 = renderAndCapture({128, 64}, draws, {ToneMappingMode::Reinhard, 1.0f});
                const glm::vec3 leftPlus1 = leftHdr * 2.0f;
                expectLinearPixel(reinhardPlus1, leftPlus1 / (1.0f + leftPlus1), 32, 32, 128);

                const auto reinhardMinus1 = renderAndCapture({128, 64}, draws, {ToneMappingMode::Reinhard, -1.0f});
                const glm::vec3 leftMinus1 = leftHdr * 0.5f;
                expectLinearPixel(reinhardMinus1, leftMinus1 / (1.0f + leftMinus1), 32, 32, 128);
            }

            // 6. 连续帧复用旧帧槽与视口尺寸重建（128x64 -> 192x96 -> 128x64）在 Required Validation 下无错误。
            for (const FrameExtent extent : {FrameExtent{192, 96}, FrameExtent{192, 96}, FrameExtent{128, 64},
                                             FrameExtent{128, 64}})
            {
                const auto px = renderAndCapture(extent, draws, {ToneMappingMode::Reinhard, 0.0f});
                const glm::vec3 rightHdr(4.0f, 3.2f, 2.0f);
                expectLinearPixel(px, rightHdr / (1.0f + rightHdr), (extent.width * 3u) / 4u, extent.height / 2u,
                                  extent.width);
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanPostprocessTest, OffscreenFxaaSmoothingBaselineTogglesAndResizeValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        auto fxaaHold = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
            Resource::ResourceType::SHADER, "shaders/fxaa.frag.spv");
        ASSERT_TRUE(fxaaHold);
        const auto fxaaWords = fxaaHold.get<Resource::ShaderResource>()->words();
        fxaaHold.reset();
        const Resource::ShaderResource fxaaShader({}, fxaaWords);
        const Resource::ShaderResource emptyShader({}, {});
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            // 1. 准备顺序与未准备 FXAA 的错误边界。
            EXPECT_THROW(renderer.prepareFxaaResources(fxaaShader), Error::Exception);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            EXPECT_THROW(renderer.prepareFxaaResources(emptyShader), Error::Exception);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            MaterialDesc matDesc{white, {0.9f, 0.9f, 0.9f}};
            matDesc.shading = ShadingMode::Unlit;
            const auto whiteMat = renderer.createMaterial(matDesc);

            // 固定对照几何：左侧高反差浅斜边三角（含大块纯白内部区域），右侧斜置细线条。
            const std::vector<MeshVertex> contrastVertices{
                // 三角形（索引 0, 1, 2）：斜边自 (-1.45, -0.30) 至 (-0.15, 0.45)，内部纯色覆盖右下角区域
                MeshVertex{{-1.45f, -0.30f, 0.0f}, {0.0f, 0.0f}},
                MeshVertex{{-0.15f, -0.75f, 0.0f}, {1.0f, 0.0f}},
                MeshVertex{{-0.15f, 0.45f, 0.0f}, {1.0f, 1.0f}},
                // 细线条（索引 3..6）：从 (0.20, -0.45) 至 (1.35, 0.45) 的约 1.2px 细斜带
                MeshVertex{{0.20f, -0.458f, 0.0f}, {0.0f, 0.0f}},
                MeshVertex{{0.20f, -0.442f, 0.0f}, {0.0f, 1.0f}},
                MeshVertex{{1.35f, 0.458f, 0.0f}, {1.0f, 1.0f}},
                MeshVertex{{1.35f, 0.442f, 0.0f}, {1.0f, 0.0f}},
            };
            const std::array<uint32_t, 9> contrastIndices{0, 1, 2, 3, 5, 4, 3, 6, 5};
            const auto mesh = renderer.createMesh(contrastVertices, contrastIndices);
            const std::array draws{DrawItem{mesh, whiteMat, glm::mat4(1.0f)}};
            releaseCpuAssets(assets);

            const std::array<float, 4> clearColor{0.04f, 0.05f, 0.08f, 1.0f};
            const auto makeCamera = [](FrameExtent extent) {
                const float aspect = float(extent.width) / float(extent.height);
                auto projection = glm::orthoRH_ZO(-aspect, aspect, -1.0f, 1.0f, 0.1f, 10.0f);
                projection[1][1] *= -1.0f;
                return CameraMatrices{
                    glm::lookAtRH(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                    projection};
            };

            // 未准备 FXAA 时以 fxaa=true 提交 drawSceneTarget 应明确抛错，但不破坏后续准备。
            {
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto invalidFxaaOut = renderer.prepareSceneTarget({320, 180}, true);
                EXPECT_TRUE(invalidFxaaOut.fxaaEnabled());
                renderer.imgui().endFrame();
                FrameInput input{{320, 180}, makeCamera({320, 180}), draws};
                input.display = {ToneMappingMode::Reinhard, 0.0f};
                EXPECT_THROW(renderer.drawSceneTarget(invalidFxaaOut, input, clearColor), Error::Exception);
                Detail::RendererReviewAccess::waitForFrames(renderer);
            }

            renderer.prepareFxaaResources(fxaaShader);
            EXPECT_THROW(renderer.prepareFxaaResources(fxaaShader), Error::Exception);

            const auto renderSceneTarget = [&](FrameExtent extent, bool fxaa) {
                const auto uiExtent = renderer.beginUiFrame();
                EXPECT_TRUE(uiExtent);
                const auto output = renderer.prepareSceneTarget(extent, fxaa);
                EXPECT_EQ(output.fxaaEnabled(), fxaa);
                ImGui::Begin("Viewport");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(extent.width), float(extent.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* drawData = renderer.imgui().drawData();
                FrameInput input{extent, makeCamera(extent), draws};
                input.display = {ToneMappingMode::Reinhard, 0.0f};
                renderer.drawSceneTarget(output, input, clearColor,
                                         [&](void* cmd) { renderer.imgui().recordDrawData(drawData, cmd); });
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        EXPECT_EQ(image.format(), VK_FORMAT_R8G8B8A8_UNORM);
                        pixels = TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                    });
                return pixels;
            };

            const auto lumaOf = [&](const std::vector<uint8_t>& px, uint32_t x, uint32_t y, uint32_t w) {
                const size_t off = (size_t(y) * w + x) * 4u;
                return (int(px[off]) * 299 + int(px[off + 1]) * 587 + int(px[off + 2]) * 114) / 1000;
            };
            const auto pixelEqual = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t x,
                                       uint32_t y, uint32_t w) {
                const size_t off = (size_t(y) * w + x) * 4u;
                return a[off] == b[off] && a[off + 1] == b[off + 1] && a[off + 2] == b[off + 2] &&
                       a[off + 3] == b[off + 3];
            };

            // 2. 固定输入 320x180 下的开关基线对比，并保存对照图到 build/review/。
            constexpr FrameExtent BASE_EXTENT{320, 180};
            const auto offBaseline = renderSceneTarget(BASE_EXTENT, false);
            const auto onBaseline = renderSceneTarget(BASE_EXTENT, true);
            ASSERT_EQ(offBaseline.size(), size_t(BASE_EXTENT.width) * BASE_EXTENT.height * 4u);
            ASSERT_EQ(onBaseline.size(), offBaseline.size());

            // 远离边缘的背景纯色与三角形纯白内部像素保持一致。
            const int bgLuma = lumaOf(offBaseline, 16, 16, BASE_EXTENT.width);
            const int fgLuma = lumaOf(offBaseline, 130, 135, BASE_EXTENT.width);
            EXPECT_LT(bgLuma, 25);
            EXPECT_GT(fgLuma, 170);
            for (uint32_t y = 12; y <= 24; ++y)
                for (uint32_t x = 12; x <= 24; ++x)
                    EXPECT_TRUE(pixelEqual(offBaseline, onBaseline, x, y, BASE_EXTENT.width));
            for (uint32_t y = 128; y <= 142; ++y)
                for (uint32_t x = 124; x <= 136; ++x)
                    EXPECT_TRUE(pixelEqual(offBaseline, onBaseline, x, y, BASE_EXTENT.width));

            // 统计斜边区域内的中间过渡像素数量（关闭时应为 0，开启时应显著出现）。
            auto countEdgeIntermediates = [&](const std::vector<uint8_t>& px, uint32_t w, uint32_t h,
                                              uint32_t xMin, uint32_t xMax, uint32_t yMin, uint32_t yMax) {
                int count = 0;
                for (uint32_t y = yMin; y < yMax; ++y)
                    for (uint32_t x = xMin; x < xMax; ++x)
                    {
                        const size_t off = (size_t(y) * w + x) * 4u;
                        EXPECT_EQ(px[off + 3], 255u);
                        const int lum = lumaOf(px, x, y, w);
                        if (lum > bgLuma + 8 && lum < fgLuma - 8)
                            ++count;
                    }
                return count;
            };
            const int offDiagIntermediates = countEdgeIntermediates(offBaseline, 320, 180, 35, 135, 45, 115);
            const int onDiagIntermediates = countEdgeIntermediates(onBaseline, 320, 180, 35, 135, 45, 115);
            EXPECT_EQ(offDiagIntermediates, 0);
            EXPECT_GT(onDiagIntermediates, 20);

            // 右侧细线区域同样产生中间抗锯齿过渡像素。
            const int offLineIntermediates = countEdgeIntermediates(offBaseline, 320, 180, 175, 290, 45, 140);
            const int onLineIntermediates = countEdgeIntermediates(onBaseline, 320, 180, 175, 290, 45, 140);
            EXPECT_EQ(offLineIntermediates, 0);
            EXPECT_GT(onLineIntermediates, 20);

            // 保存固定机位对照结果到 build/review/fxaa-off.ppm 与 build/review/fxaa-on.ppm。
            const auto reviewDir = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path().parent_path() / "review";
            WindowCapture::save({int(BASE_EXTENT.width), int(BASE_EXTENT.height), offBaseline},
                                reviewDir / "fxaa-off.ppm");
            WindowCapture::save({int(BASE_EXTENT.width), int(BASE_EXTENT.height), onBaseline},
                                reviewDir / "fxaa-on.ppm");

            // 3. 同一帧槽连续帧开关切换：关闭严格保持 LDR 基线，开启保持一致 FXAA 输出。
            for (const bool fxaa : {false, true, true, false, false, true})
            {
                const auto px = renderSceneTarget(BASE_EXTENT, fxaa);
                EXPECT_EQ(px, fxaa ? onBaseline : offBaseline);
            }

            // 4. 尺寸重建（480x270 -> 240x136）下开关及双帧槽输出均正确。
            for (const FrameExtent extent : {FrameExtent{480, 270}, FrameExtent{240, 136}})
            {
                for (int slotPass = 0; slotPass < 2; ++slotPass)
                {
                    const auto offPx = renderSceneTarget(extent, false);
                    const auto onPx = renderSceneTarget(extent, true);
                    EXPECT_TRUE(pixelEqual(offPx, onPx, 8, 8, extent.width));
                    const uint32_t xMin = extent.width / 10u;
                    const uint32_t xMax = (extent.width * 45u) / 100u;
                    const uint32_t yMin = extent.height / 4u;
                    const uint32_t yMax = (extent.height * 65u) / 100u;
                    EXPECT_EQ(countEdgeIntermediates(offPx, extent.width, extent.height, xMin, xMax, yMin, yMax), 0);
                    EXPECT_GT(countEdgeIntermediates(onPx, extent.width, extent.height, xMin, xMax, yMin, yMax), 10);
                }
            }

            // 5. 同 Renderer 下直接呈现路径 drawFrame 继续可用且不录制 FXAA。
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            renderer.imgui().endFrame();
            renderer.drawFrame(FrameInput{*extent, makeCamera(*extent), draws}, clearColor);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanPostprocessTest, OffscreenViewportStatsLifecycleAndComparisonReport)
    {
        auto window = createWindow();
        SceneAssets assets;
        auto fxaaHold = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
            Resource::ResourceType::SHADER, "shaders/fxaa.frag.spv");
        auto shadowHold = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
            Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
        ASSERT_TRUE(fxaaHold);
        ASSERT_TRUE(shadowHold);
        const Resource::ShaderResource fxaaShader({}, fxaaHold.get<Resource::ShaderResource>()->words());
        const Resource::ShaderResource shadowShader({}, shadowHold.get<Resource::ShaderResource>()->words());
        fxaaHold.reset();
        shadowHold.reset();

        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            renderer.prepareFxaaResources(fxaaShader);
            renderer.prepareShadowResources(shadowShader);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            MaterialDesc groundMatDesc{white, {0.72f, 0.74f, 0.78f}};
            groundMatDesc.shading = ShadingMode::Lambert;
            const auto groundMat = renderer.createMaterial(groundMatDesc);
            MaterialDesc cubeMatDesc{white, {0.88f, 0.45f, 0.25f}};
            cubeMatDesc.shading = ShadingMode::Lambert;
            const auto cubeMat = renderer.createMaterial(cubeMatDesc);

            const auto cubeData = Demo::makeCubeMesh();
            const auto cubeMesh = renderer.createMesh(cubeData.vertices, cubeData.indices);
            const glm::mat4 groundModel =
                glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -0.6f, 0.0f)), glm::vec3(5.0f, 0.2f, 5.0f));
            const glm::mat4 casterA =
                glm::rotate(glm::translate(glm::mat4(1.0f), glm::vec3(-0.8f, 0.2f, 0.2f)), glm::radians(28.0f),
                            glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::mat4 casterB =
                glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.9f, 0.0f, -0.5f)), glm::vec3(0.8f, 1.2f, 0.8f));
            const std::array draws{
                DrawItem{cubeMesh, groundMat, groundModel},
                DrawItem{cubeMesh, cubeMat, casterA},
                DrawItem{cubeMesh, cubeMat, casterB},
            };
            releaseCpuAssets(assets);

            // 1. 首帧提交前无完成样本；零尺寸不会产生样本。
            EXPECT_FALSE(renderer.lastViewportStats().hasCompletedSample);
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 0u);
            EXPECT_FALSE(renderer.prepareSceneTarget({0, 0}, false).valid());
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 0u);

            const std::array<float, 4> clearColor{0.08f, 0.10f, 0.14f, 1.0f};
            const auto makeCamera = [](FrameExtent extent) {
                const float aspect = float(extent.width) / float(extent.height);
                auto projection = glm::perspectiveRH_ZO(glm::radians(45.0f), aspect, 0.1f, 50.0f);
                projection[1][1] *= -1.0f;
                return CameraMatrices{
                    glm::lookAtRH(glm::vec3(2.8f, 2.2f, 3.6f), glm::vec3(0.0f, -0.1f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                    projection};
            };

            const auto submitOffscreen = [&](FrameExtent extent, bool shadows, bool fxaa,
                                             std::vector<uint8_t>* outPixels = nullptr) {
                const auto uiExtent = renderer.beginUiFrame();
                EXPECT_TRUE(uiExtent);
                const auto output = renderer.prepareSceneTarget(extent, fxaa);
                ImGui::Begin("Viewport");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(extent.width), float(extent.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* drawData = renderer.imgui().drawData();

                FrameInput input{extent, makeCamera(extent), draws};
                input.display = {ToneMappingMode::Reinhard, 0.0f};
                input.lighting.direction = glm::normalize(glm::vec3(-0.6f, -1.0f, -0.4f));
                input.lighting.color = {1.0f, 0.96f, 0.90f};
                input.lighting.intensity = 2.6f;
                input.lighting.ambient = 0.25f;
                if (shadows)
                {
                    ShadowSettings settings;
                    settings.enabled = true;
                    settings.depthBias = 0.003f;
                    input.shadow = makeShadowFrame(settings, input.lighting.direction);
                    input.shadowDraws = draws;
                }

                renderer.drawSceneTarget(output, input, clearColor,
                                         [&](void* cmd) { renderer.imgui().recordDrawData(drawData, cmd); });
                if (outPixels)
                {
                    Detail::RendererReviewAccess::withSceneOutput(
                        renderer, output, [&](const auto& device, const auto& image) {
                            *outPixels = TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                        });
                }
            };

            // 2. 生命周期：连续帧、开关阴影、drawFrame 不新增样本、尺寸重建。
            submitOffscreen({320, 180}, false, false);
            Detail::RendererReviewAccess::waitForFrames(renderer);
            auto stats = renderer.lastViewportStats();
            EXPECT_TRUE(stats.hasCompletedSample);
            EXPECT_EQ(stats.sceneDrawCount, 3u);
            EXPECT_EQ(stats.shadowDrawCount, 0u);
            if (stats.gpuTimestampAvailable)
            {
                ASSERT_TRUE(stats.gpuTimeMs.has_value());
                EXPECT_GE(*stats.gpuTimeMs, 0.0);
            }
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 1u);

            // 开启阴影并等待该样本完成。
            submitOffscreen({320, 180}, true, false);
            Detail::RendererReviewAccess::waitForFrames(renderer);
            stats = renderer.lastViewportStats();
            EXPECT_TRUE(stats.hasCompletedSample);
            EXPECT_EQ(stats.sceneDrawCount, 3u);
            EXPECT_EQ(stats.shadowDrawCount, 3u);
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 2u);

            // 直接呈现路径 drawFrame 不应新增离屏视口样本。
            {
                const auto uiExtent = renderer.beginUiFrame();
                ASSERT_TRUE(uiExtent);
                renderer.imgui().endFrame();
                renderer.drawFrame(FrameInput{*uiExtent, makeCamera(*uiExtent), draws}, clearColor);
                Detail::RendererReviewAccess::waitForFrames(renderer);
                EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 2u);
            }

            // 尺寸重建后提交仍正常采集样本。
            submitOffscreen({480, 270}, false, true);
            Detail::RendererReviewAccess::waitForFrames(renderer);
            stats = renderer.lastViewportStats();
            EXPECT_EQ(stats.sceneDrawCount, 3u);
            EXPECT_EQ(stats.shadowDrawCount, 0u);
            EXPECT_EQ(Detail::RendererReviewAccess::completedViewportSampleCount(renderer), 3u);

            // 3. 固定条件三组对照（640x360）：全关 / 仅开 Shadows / 仅开 FXAA。
            constexpr FrameExtent COMPARE_EXTENT{640, 360};
            constexpr int WARMUP_FRAMES = 4;
            constexpr int SAMPLE_FRAMES = 11;
            struct GroupSpec
            {
                const char* label;
                const char* ppmName;
                bool shadows;
                bool fxaa;
            };
            const std::array<GroupSpec, 3> groups{{
                {"AllOff", "stats-off.ppm", false, false},
                {"ShadowsOnly", "stats-shadows.ppm", true, false},
                {"FxaaOnly", "stats-fxaa.ppm", false, true},
            }};

            const auto reviewDir = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path().parent_path() / "review";
            std::filesystem::create_directories(reviewDir);
            std::ostringstream report;
            report << "Viewport Stats Comparison (640x360, Debug, Validation=Required)\n";
            report << "Warmup=" << WARMUP_FRAMES << ", Samples=" << SAMPLE_FRAMES << "\n";

            for (const auto& group : groups)
            {
                for (int i = 0; i < WARMUP_FRAMES; ++i)
                    submitOffscreen(COMPARE_EXTENT, group.shadows, group.fxaa);
                Detail::RendererReviewAccess::waitForFrames(renderer);

                std::vector<double> gpuTimes;
                uint32_t recordedSceneDraws = 0;
                uint32_t recordedShadowDraws = 0;
                std::vector<uint8_t> finalPixels;
                for (int i = 0; i < SAMPLE_FRAMES; ++i)
                {
                    submitOffscreen(COMPARE_EXTENT, group.shadows, group.fxaa,
                                    (i + 1 == SAMPLE_FRAMES) ? &finalPixels : nullptr);
                    Detail::RendererReviewAccess::waitForFrames(renderer);
                    const auto current = renderer.lastViewportStats();
                    EXPECT_TRUE(current.hasCompletedSample);
                    EXPECT_EQ(current.sceneDrawCount, 3u);
                    EXPECT_EQ(current.shadowDrawCount, group.shadows ? 3u : 0u);
                    recordedSceneDraws = current.sceneDrawCount;
                    recordedShadowDraws = current.shadowDrawCount;
                    if (current.gpuTimeMs.has_value())
                        gpuTimes.push_back(*current.gpuTimeMs);
                }

                WindowCapture::save({int(COMPARE_EXTENT.width), int(COMPARE_EXTENT.height), finalPixels},
                                    reviewDir / group.ppmName);

                double medianMs = 0.0;
                if (!gpuTimes.empty())
                {
                    std::sort(gpuTimes.begin(), gpuTimes.end());
                    medianMs = gpuTimes[gpuTimes.size() / 2];
                }
                report << group.label << ": medianGpuMs=" << medianMs << ", sceneDraws=" << recordedSceneDraws
                       << ", shadowDraws=" << recordedShadowDraws << ", ppm=" << group.ppmName << "\n";
            }

            std::ofstream reportFile(reviewDir / "viewport-stats-comparison.txt");
            reportFile << report.str();
            std::cout << report.str() << std::endl;
        }
        expectCleanValidation();
    }

    TEST_F(VulkanPostprocessTest, AcesFilmicAndSkyboxWithBlendAndFallbackUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            auto& manager = Resource::ResourceManager::instance();
            const auto skyVert =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/skybox.vert.spv");
            const auto skyFrag =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/skybox.frag.spv");
            ASSERT_TRUE(skyVert && skyFrag);

            const Resource::ShaderResource emptyShader({}, {});
            EXPECT_THROW(renderer.prepareSkyboxResources(emptyShader, *skyFrag.get<Resource::ShaderResource>()),
                         Error::Exception);
            renderer.prepareSkyboxResources(*skyVert.get<Resource::ShaderResource>(),
                                            *skyFrag.get<Resource::ShaderResource>());
            EXPECT_THROW(renderer.prepareSkyboxResources(*skyVert.get<Resource::ShaderResource>(),
                                                         *skyFrag.get<Resource::ShaderResource>()),
                         Error::Exception);

            const auto encodeSrgb8 = [](float linear) {
                const float clamped = std::clamp(linear, 0.0f, 1.0f);
                const float encoded =
                    clamped <= 0.0031308f ? clamped * 12.92f : 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
                return encoded * 255.0f;
            };
            const auto aces = [](float x) {
                return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
            };
            const auto perspectiveCamera = [](glm::vec3 target, FrameExtent ext) {
                const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), target, glm::vec3(0.0f, 1.0f, 0.0f));
                glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), float(ext.width) / ext.height, 0.1f, 50.0f);
                proj[1][1] *= -1.0f;
                return CameraMatrices{view, proj};
            };
            const auto renderLdr = [&](FrameExtent ext, const FrameInput& input, std::array<float, 4> clear) {
                window->pollEvents();
                EXPECT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(ext);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, input, clear);
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        pixels = TextureGpuReview::captureMip(device, image, 0, {ext.width, ext.height});
                    });
                return pixels;
            };

            const FrameExtent ext{64, 64};
            const std::array<float, 4> clearColor{0.18f, 0.34f, 0.52f, 1.0f};
            const CameraMatrices camNegZ = perspectiveCamera({0.0f, 0.0f, -1.0f}, ext);

            // 1. IBL 未就绪时启用 skyboxEnabled 不应崩溃或报错，应退回到 clearColor 背景。
            {
                FrameInput fallbackInput{ext, camNegZ, {}};
                fallbackInput.display.skyboxEnabled = true;
                const auto px = renderLdr(ext, fallbackInput, clearColor);
                const size_t center = (32u * ext.width + 32u) * 4u;
                EXPECT_NEAR(px[center + 0], clearColor[0] * 255.0f, 1.5f);
                EXPECT_NEAR(px[center + 1], clearColor[1] * 255.0f, 1.5f);
                EXPECT_NEAR(px[center + 2], clearColor[2] * 255.0f, 1.5f);

                const auto hdrFallback = captureHdr(renderer, fallbackInput);
                EXPECT_FLOAT_EQ(hdrFallback[32u * ext.width + 32u][3], 0.0f);
            }

            // 2. 构造有明显方向颜色差异的合成立方体贴图 (+X红, -X绿, +Y蓝, -Y黄, +Z青, -Z橙)。
            const auto makeSolidFace = [](std::vector<uint16_t>& dst, uint32_t pixelsPerFace,
                                          std::array<float, 4> rgba) {
                for (uint32_t i = 0; i < pixelsPerFace; ++i)
                {
                    dst.push_back(glm::packHalf1x16(rgba[0]));
                    dst.push_back(glm::packHalf1x16(rgba[1]));
                    dst.push_back(glm::packHalf1x16(rgba[2]));
                    dst.push_back(glm::packHalf1x16(rgba[3]));
                }
            };
            std::vector<uint16_t> prePixels;
            const std::array<std::array<float, 4>, 6> faceColors{{
                {1.2f, 0.06f, 0.06f, 1.0f},  // +X 红
                {0.06f, 1.2f, 0.06f, 1.0f},  // -X 绿
                {0.06f, 0.12f, 1.1f, 1.0f},  // +Y 蓝
                {1.0f, 1.0f, 0.06f, 1.0f},   // -Y 黄
                {0.06f, 1.0f, 1.0f, 1.0f},   // +Z 青
                {1.4f, 0.55f, 0.08f, 1.0f},  // -Z 橙
            }};
            for (const auto& color : faceColors)
                makeSolidFace(prePixels, 4u, color);
            std::vector<uint16_t> irrPixels;
            for (int f = 0; f < 6; ++f)
                makeSolidFace(irrPixels, 1u, {0.1f, 0.1f, 0.1f, 1.0f});
            const std::vector<uint16_t> lutPixels{glm::packHalf1x16(1.0f), glm::packHalf1x16(0.0f),
                                                  glm::packHalf1x16(0.0f), glm::packHalf1x16(1.0f)};

            const auto irrTex = renderer.createTexture(Resource::FloatTextureResource(
                {}, Resource::FloatTextureDimension::CubeMap, {Resource::FloatTextureMip{1, 1, std::move(irrPixels)}}));
            const auto preTex = renderer.createTexture(Resource::FloatTextureResource(
                {}, Resource::FloatTextureDimension::CubeMap, {Resource::FloatTextureMip{2, 2, std::move(prePixels)}}));
            const auto lutTex = renderer.createTexture(Resource::FloatTextureResource(
                {}, Resource::FloatTextureDimension::Texture2D, {Resource::FloatTextureMip{1, 1, lutPixels}}));
            renderer.prepareIblResources(irrTex, preTex, lutTex);

            // 3. 验证相机旋转时天空盒方向颜色随之变化。
            {
                FrameInput skyNegZ{ext, camNegZ, {}};
                skyNegZ.display.skyboxEnabled = true;
                const auto hdrNegZ = captureHdr(renderer, skyNegZ);
                const auto& cNegZ = hdrNegZ[32u * ext.width + 32u];
                EXPECT_NEAR(cNegZ[0], 1.4f, 0.05f);
                EXPECT_NEAR(cNegZ[1], 0.55f, 0.05f);
                EXPECT_NEAR(cNegZ[2], 0.08f, 0.05f);
                EXPECT_FLOAT_EQ(cNegZ[3], 1.0f);

                FrameInput skyPosX{ext, perspectiveCamera({1.0f, 0.0f, 0.0f}, ext), {}};
                skyPosX.display.skyboxEnabled = true;
                const auto hdrPosX = captureHdr(renderer, skyPosX);
                const auto& cPosX = hdrPosX[32u * ext.width + 32u];
                EXPECT_GT(cPosX[0], 1.0f);
                EXPECT_LT(cPosX[1], 0.15f);

                FrameInput skyNegX{ext, perspectiveCamera({-1.0f, 0.0f, 0.0f}, ext), {}};
                skyNegX.display.skyboxEnabled = true;
                const auto hdrNegX = captureHdr(renderer, skyNegX);
                const auto& cNegX = hdrNegX[32u * ext.width + 32u];
                EXPECT_GT(cNegX[1], 1.0f);
                EXPECT_LT(cNegX[0], 0.15f);
            }

            // 4. 验证近处 Opaque 不被天空盒覆盖，半透明 Blend 在天空盒之上混合。
            const auto plane = createLitPlane(renderer);
            const auto white = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            MaterialDesc opaqueBlueDesc{white, {0.0f, 0.0f, 0.0f}};
            opaqueBlueDesc.shading = ShadingMode::Pbr;
            opaqueBlueDesc.emissiveTexture = white;
            opaqueBlueDesc.emissiveFactor = {0.04f, 0.06f, 0.85f};
            const auto opaqueBlueMat = renderer.createMaterial(opaqueBlueDesc);

            MaterialDesc blendCyanDesc{white, {0.0f, 0.0f, 0.0f}};
            blendCyanDesc.shading = ShadingMode::Pbr;
            blendCyanDesc.alphaMode = AlphaMode::Blend;
            blendCyanDesc.doubleSided = true;
            blendCyanDesc.baseAlpha = 0.5f;
            blendCyanDesc.emissiveTexture = white;
            blendCyanDesc.emissiveFactor = {0.0f, 0.2f, 1.0f};
            const auto blendBlueMat = renderer.createMaterial(blendCyanDesc);

            {
                const std::array draws{
                    DrawItem{plane, opaqueBlueMat,
                             glm::translate(glm::mat4(1.0f), {-0.6f, 0.0f, -2.5f}) *
                                 glm::scale(glm::mat4(1.0f), {0.45f, 0.45f, 1.0f})},
                    DrawItem{plane, blendBlueMat,
                             glm::translate(glm::mat4(1.0f), {0.6f, 0.0f, -2.5f}) *
                                 glm::scale(glm::mat4(1.0f), {0.45f, 0.45f, 1.0f})},
                };
                FrameInput sceneInput{ext, camNegZ, draws, Lighting{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.0f}};
                sceneInput.display.skyboxEnabled = true;
                const auto hdr = captureHdr(renderer, sceneInput);
                const auto& leftOpaque = hdr[32u * ext.width + 18u];
                const auto& centerSky = hdr[32u * ext.width + 32u];
                const auto& rightBlend = hdr[32u * ext.width + 46u];
                // 左侧近距不透明蓝块遮挡橙色天空盒
                EXPECT_LT(leftOpaque[0], 0.15f);
                EXPECT_GT(leftOpaque[2], 0.7f);
                // 中间暴露天空盒 (-Z 橙色)
                EXPECT_NEAR(centerSky[0], 1.4f, 0.05f);
                EXPECT_NEAR(centerSky[1], 0.55f, 0.05f);
                // 右侧半透明蓝块在天空盒上方混合：保留天空盒约一半橙色并叠加蓝色
                EXPECT_GT(rightBlend[0], 0.45f);
                EXPECT_LT(rightBlend[0], 1.1f);
                EXPECT_GT(rightBlend[2], 0.45f);
            }

            // 5. 验证 ACES Filmic 色调映射曲线数值、高光区分能力及曝光响应。
            {
                MaterialDesc leftHiDesc{white, {0.06f, 0.03f, 0.01f}};
                leftHiDesc.shading = ShadingMode::Lambert;
                const auto leftMat = renderer.createMaterial(leftHiDesc);

                MaterialDesc rightHiDesc{white, {0.5f, 0.35f, 0.15f}};
                rightHiDesc.shading = ShadingMode::Lambert;
                const auto rightMat = renderer.createMaterial(rightHiDesc);

                const std::array draws{
                    DrawItem{plane, leftMat,
                             glm::translate(glm::mat4(1.0f), {-0.6f, 0.0f, -2.5f}) *
                                 glm::scale(glm::mat4(1.0f), {0.45f, 0.45f, 1.0f})},
                    DrawItem{plane, rightMat,
                             glm::translate(glm::mat4(1.0f), {0.6f, 0.0f, -2.5f}) *
                                 glm::scale(glm::mat4(1.0f), {0.45f, 0.45f, 1.0f})},
                };
                const Lighting acesLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 10.0f, 0.0f};
                FrameInput acesInput{ext, camNegZ, draws, acesLight};
                acesInput.display = {ToneMappingMode::AcesFilmic, 0.0f, false, false};
                const auto aces0 = renderLdr(ext, acesInput, {0, 0, 0, 1});
                const size_t lOff = (32u * ext.width + 18u) * 4u;
                const size_t rOff = (32u * ext.width + 46u) * 4u;
                EXPECT_NEAR(aces0[lOff + 0], encodeSrgb8(aces(0.6f)), 2.5f);
                EXPECT_NEAR(aces0[lOff + 1], encodeSrgb8(aces(0.3f)), 2.5f);
                EXPECT_NEAR(aces0[lOff + 2], encodeSrgb8(aces(0.1f)), 2.5f);
                EXPECT_NEAR(aces0[rOff + 0], encodeSrgb8(aces(5.0f)), 2.5f);
                EXPECT_GT(aces0[rOff + 0], aces0[lOff + 0] + 30);

                acesInput.display.exposureEv = 1.0f;
                const auto acesPlus = renderLdr(ext, acesInput, {0, 0, 0, 1});
                EXPECT_NEAR(acesPlus[lOff + 0], encodeSrgb8(aces(1.2f)), 2.5f);

                acesInput.display.exposureEv = -1.0f;
                const auto acesMinus = renderLdr(ext, acesInput, {0, 0, 0, 1});
                EXPECT_NEAR(acesMinus[lOff + 0], encodeSrgb8(aces(0.3f)), 2.5f);
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanPostprocessTest, HdrBloomHaloThresholdsToggleAndTinyResizeUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            auto& manager = Resource::ResourceManager::instance();
            const auto bloomPre = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                         "shaders/bloom_prefilter.frag.spv");
            const auto bloomDown = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                          "shaders/bloom_downsample.frag.spv");
            const auto bloomUp = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                        "shaders/bloom_upsample.frag.spv");
            const auto fxaaFrag =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/fxaa.frag.spv");
            ASSERT_TRUE(bloomPre && bloomDown && bloomUp && fxaaFrag);

            renderer.prepareFxaaResources(*fxaaFrag.get<Resource::ShaderResource>());
            const Resource::ShaderResource emptyShader({}, {});
            EXPECT_THROW(renderer.prepareBloomResources(emptyShader, *bloomDown.get<Resource::ShaderResource>(),
                                                        *bloomUp.get<Resource::ShaderResource>()),
                         Error::Exception);
            renderer.prepareBloomResources(*bloomPre.get<Resource::ShaderResource>(),
                                           *bloomDown.get<Resource::ShaderResource>(),
                                           *bloomUp.get<Resource::ShaderResource>());
            EXPECT_THROW(renderer.prepareBloomResources(*bloomPre.get<Resource::ShaderResource>(),
                                                        *bloomDown.get<Resource::ShaderResource>(),
                                                        *bloomUp.get<Resource::ShaderResource>()),
                         Error::Exception);

            const auto white = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            const auto plane = createLitPlane(renderer);
            MaterialDesc hotDesc{white, {1.0f, 0.35f, 0.05f}};
            hotDesc.shading = ShadingMode::Lambert;
            const auto hotMat = renderer.createMaterial(hotDesc);
            const std::array hotDraws{
                DrawItem{plane, hotMat, glm::scale(glm::mat4(1.0f), {0.18f, 0.18f, 1.0f})},
            };
            const Lighting hotLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 8.0f, 0.0f};

            const auto orthoCam = [](FrameExtent ext) {
                const float aspect = float(ext.width) / ext.height;
                glm::mat4 proj = glm::orthoRH_ZO(-aspect, aspect, -1.0f, 1.0f, 0.1f, 10.0f);
                proj[1][1] *= -1.0f;
                return CameraMatrices{glm::lookAtRH(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0)), proj};
            };
            const auto renderLdr = [&](FrameExtent ext, const DisplaySettings& display, bool fxaa = false) {
                window->pollEvents();
                EXPECT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(ext, fxaa);
                renderer.imgui().endFrame();
                FrameInput input{ext, orthoCam(ext), hotDraws, hotLight};
                input.display = display;
                renderer.drawSceneTarget(output, input, {0.0f, 0.0f, 0.0f, 1.0f});
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        pixels = TextureGpuReview::captureMip(device, image, 0, {ext.width, ext.height});
                    });
                return pixels;
            };

            const FrameExtent ext{128, 128};
            const auto pixelR = [&](const std::vector<uint8_t>& px, uint32_t x, uint32_t y) {
                return px[(size_t(y) * ext.width + x) * 4u + 0u];
            };
            const auto pixelG = [&](const std::vector<uint8_t>& px, uint32_t x, uint32_t y) {
                return px[(size_t(y) * ext.width + x) * 4u + 1u];
            };

            // 1. 关闭 Bloom 时，亮核四周黑背景应完全为 0，不受 bloomIntensity 影响。
            const auto bloomOffA =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, false, 1.0f, 0.0f});
            const auto bloomOffB =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, false, 1.0f, 2.0f});
            EXPECT_EQ(bloomOffA, bloomOffB);
            EXPECT_EQ(pixelR(bloomOffA, 64, 38), 0u);
            EXPECT_EQ(pixelR(bloomOffA, 38, 64), 0u);

            // 2. 开启 Bloom 且阈值 <= 高亮值时，四周黑背景产生近似对称并随距离衰减的光晕。
            const auto bloomOn =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, true, 1.0f, 1.0f});
            const uint8_t nearTopR = pixelR(bloomOn, 64, 42);
            const uint8_t nearBottomR = pixelR(bloomOn, 64, 86);
            const uint8_t nearLeftR = pixelR(bloomOn, 42, 64);
            const uint8_t nearRightR = pixelR(bloomOn, 86, 64);
            const uint8_t farTopR = pixelR(bloomOn, 64, 24);
            EXPECT_GT(nearTopR, 25u);
            EXPECT_GT(nearTopR, pixelG(bloomOn, 64, 42));
            EXPECT_NEAR(float(nearTopR), float(nearBottomR), 6.0f);
            EXPECT_NEAR(float(nearLeftR), float(nearRightR), 6.0f);
            EXPECT_GT(nearTopR, farTopR + 8u);
            EXPECT_GT(farTopR, 0u);

            // 3. 阈值升高超过高亮辐射度时应抑制光晕；强度放大时同位置光晕单调递增。
            const auto highThreshold =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, true, 20.0f, 1.0f});
            EXPECT_EQ(pixelR(highThreshold, 64, 42), 0u);

            const auto lowIntensity =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, true, 1.0f, 0.4f});
            const auto highIntensity =
                renderLdr(ext, {ToneMappingMode::AcesFilmic, 0.0f, false, true, 1.0f, 2.5f});
            EXPECT_GT(pixelR(highIntensity, 64, 42), pixelR(lowIntensity, 64, 42));

            // 4. FXAA 联用与开关切换 + 极小尺寸缩放 ({4,4}, {2,2}) 在同步验证下应无布局/屏障错误。
            for (const auto& [targetExt, enableBloom, fxaa] :
                 {std::tuple{FrameExtent{128, 128}, true, true}, std::tuple{FrameExtent{128, 128}, false, false},
                  std::tuple{FrameExtent{128, 128}, true, false}, std::tuple{FrameExtent{4, 4}, true, false},
                  std::tuple{FrameExtent{2, 2}, true, true}, std::tuple{FrameExtent{64, 48}, true, false}})
            {
                const auto px = renderLdr(
                    targetExt, {ToneMappingMode::AcesFilmic, 0.0f, false, enableBloom, 1.0f, 0.8f}, fxaa);
                EXPECT_EQ(px.size(), size_t(targetExt.width) * targetExt.height * 4u);
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanPostprocessTest, DebugLinesDepthOcclusionAndDynamicToggleUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            auto& manager = Resource::ResourceManager::instance();
            const auto lineVert =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/debug_line.vert.spv");
            const auto lineFrag =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/debug_line.frag.spv");
            ASSERT_TRUE(lineVert && lineFrag);

            const Resource::ShaderResource emptyShader({}, {});
            EXPECT_THROW(renderer.prepareDebugLineResources(emptyShader, *lineFrag.get<Resource::ShaderResource>()),
                         Error::Exception);
            const auto white = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            const auto plane = createLitPlane(renderer);
            MaterialDesc opaqueRedDesc{white, {0.0f, 0.0f, 0.0f}};
            opaqueRedDesc.shading = ShadingMode::Pbr;
            opaqueRedDesc.emissiveTexture = white;
            opaqueRedDesc.emissiveFactor = {0.8f, 0.05f, 0.05f};
            const auto redMat = renderer.createMaterial(opaqueRedDesc);

            const FrameExtent ext{64, 64};
            glm::mat4 proj = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 10.0f);
            proj[1][1] *= -1.0f;
            const CameraMatrices cam{glm::lookAtRH(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0)), proj};
            const std::array draws{
                DrawItem{plane, redMat,
                         glm::translate(glm::mat4(1.0f), {-1.0f, 0.0f, -2.0f}) *
                             glm::scale(glm::mat4(1.0f), {2.0f, 2.0f, 1.0f})},
            };
            // 未准备 DebugLine 资源时提交非空 debugLines 应抛出 RENDER。
            const std::array singleLine{DebugLine{{-1.8f, 0.0f, -1.0f}, {1.8f, 0.0f, -1.0f}, {0.1f, 0.9f, 0.1f}, 1.0f}};
            FrameInput unreadyInput{ext, cam, draws};
            unreadyInput.debugLines = singleLine;
            EXPECT_THROW((void)captureHdr(renderer, unreadyInput), Error::Exception);

            renderer.prepareDebugLineResources(*lineVert.get<Resource::ShaderResource>(),
                                               *lineFrag.get<Resource::ShaderResource>());
            EXPECT_THROW(renderer.prepareDebugLineResources(*lineVert.get<Resource::ShaderResource>(),
                                                            *lineFrag.get<Resource::ShaderResource>()),
                         Error::Exception);

            // 前景绿线 (z=-1) 在红色方块 (z=-2) 前面绘制；后景蓝线 (z=-3) 左侧被红块遮挡，右侧无遮挡可见。
            constexpr float yTop = 2.0f - 24.5f / 16.0f;
            constexpr float yBot = 2.0f - 40.5f / 16.0f;
            const std::array depthLines{
                DebugLine{{-1.8f, yTop, -1.0f}, {1.8f, yTop, -1.0f}, {0.1f, 0.9f, 0.1f}, 1.0f},
                DebugLine{{-1.8f, yBot, -3.0f}, {1.8f, yBot, -3.0f}, {0.1f, 0.2f, 0.95f}, 1.0f},
            };
            FrameInput lineInput{ext, cam, draws};
            lineInput.debugLines = depthLines;
            const auto hdr = captureHdr(renderer, lineInput);
            const auto samplePixel = [&](uint32_t px, uint32_t py) {
                return hdr[size_t(py) * ext.width + px];
            };
            // 第 24 行（yTop）绿线在红块前面 (x=16 左侧和 x=48 右侧均显绿)
            EXPECT_GT(samplePixel(16u, 24u)[1], 0.7f);
            EXPECT_GT(samplePixel(48u, 24u)[1], 0.7f);
            // 第 40 行（yBot）蓝线左侧被 z=-2 红块遮挡（保持红），右侧穿过背景（显蓝）
            const auto occludedPx = samplePixel(16u, 40u);
            EXPECT_GT(occludedPx[0], 0.6f);
            EXPECT_LT(occludedPx[2], 0.15f);
            EXPECT_GT(samplePixel(48u, 40u)[2], 0.7f);

            // 在 drawSceneTarget 中空线段 <-> 多线段切换与视口尺寸变化及截图回读保持零 Validation 错误。
            for (const auto& [targetExt, lines] :
                 {std::pair{FrameExtent{64, 64}, std::span<const DebugLine>{depthLines}},
                  std::pair{FrameExtent{64, 64}, std::span<const DebugLine>{}},
                  std::pair{FrameExtent{4, 4}, std::span<const DebugLine>{depthLines}},
                  std::pair{FrameExtent{96, 64}, std::span<const DebugLine>{depthLines}}})
            {
                window->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(targetExt);
                renderer.imgui().endFrame();
                FrameInput targetInput{targetExt, cam, draws};
                targetInput.debugLines = lines;
                renderer.drawSceneTarget(output, targetInput, {0, 0, 0, 1});
            }
            const auto captured = renderer.captureLastSceneViewport();
            ASSERT_TRUE(captured);
            EXPECT_EQ(captured->extent, (FrameExtent{96u, 64u}));
            EXPECT_EQ(captured->rgba8.size(), 96u * 64u * 4u);
        }
        expectCleanValidation();
    }
}  // namespace
