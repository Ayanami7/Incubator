#include "vulkan_review_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Render;
    using namespace Incubator::Render::Vulkan;
    using namespace Incubator::Test;

    class VulkanShadowTest : public VulkanReviewTest
    {};

    TEST_F(VulkanShadowTest, AlphaMaskCutsMainAndShadowPixelsAtTheSameThreshold)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto& manager = Resource::ResourceManager::instance();
            const auto vertex = manager.hold<Resource::ShaderResource>(
                Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
            const auto maskVertex = manager.hold<Resource::ShaderResource>(
                Resource::ResourceType::SHADER, "shaders/shadow_mask.vert.spv");
            const auto maskFragment = manager.hold<Resource::ShaderResource>(
                Resource::ResourceType::SHADER, "shaders/shadow_mask.frag.spv");
            ASSERT_TRUE(vertex && maskVertex && maskFragment);
            renderer.prepareShadowResources(*vertex.get<Resource::ShaderResource>(),
                                            *maskVertex.get<Resource::ShaderResource>(),
                                            *maskFragment.get<Resource::ShaderResource>());
            const auto plane = createLitPlane(renderer);
            const Resource::TextureResource alphaTex({}, 2, 1, {255, 0, 0, 127, 255, 0, 0, 255});
            MaterialDesc mask{renderer.createTexture(alphaTex)};
            mask.alphaMode = AlphaMode::Mask;
            mask.alphaCutoff = 0.5f;
            mask.baseAlpha = 0.5f;
            std::array caster{DrawItem{plane, renderer.createMaterial(mask), glm::translate(glm::mat4(1), {0, 0, 0.3f})}};
            constexpr FrameExtent extent{128, 128};
            const auto left = size_t(64) * extent.width + 48;
            const auto right = size_t(64) * extent.width + 80;
            const auto main = captureHdr(renderer, controlledFrame(extent, caster));
            ASSERT_EQ(main.size(), size_t(extent.width) * extent.height);
            EXPECT_EQ(main[left], (std::array<float, 4>{0, 0, 0, 0}));
            EXPECT_EQ(main[right], (std::array<float, 4>{1, 0, 0, 1}));

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            MaterialDesc white{renderer.createTexture(whiteTex)};
            white.shading = ShadingMode::Lambert;
            const std::array receiver{DrawItem{plane, renderer.createMaterial(white), glm::mat4(1)}};
            auto frame = controlledFrame(extent, receiver);
            frame.lighting = {{0, 0, -1}, {1, 1, 1}, 1.0f, 0.0f};
            ShadowSettings settings;
            settings.enabled = true;
            frame.shadow = makeShadowFrame(settings, frame.lighting.direction);
            frame.shadowDraws = caster;
            const auto shadow = captureHdr(renderer, frame);
            ASSERT_EQ(shadow.size(), main.size());
            EXPECT_NEAR(shadow[left][0], 1.0f, 0.005f);
            EXPECT_NEAR(shadow[right][0], 0.0f, 0.005f);
            mask.alphaMode = AlphaMode::Blend;
            caster[0].material = renderer.createMaterial(mask);
            const auto withoutBlendShadow = captureHdr(renderer, frame);
            ASSERT_EQ(withoutBlendShadow.size(), main.size());
            EXPECT_NEAR(withoutBlendShadow[right][0], 1.0f, 0.005f);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadowTest, AdaptiveShadowFitNormalBiasAndResolutionSwitchUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto& manager = Resource::ResourceManager::instance();
            const auto shadowVert =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
            const auto shadowMaskVert = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                               "shaders/shadow_mask.vert.spv");
            const auto shadowMaskFrag = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                               "shaders/shadow_mask.frag.spv");
            ASSERT_TRUE(shadowVert && shadowMaskVert && shadowMaskFrag);
            renderer.prepareShadowResources(*shadowVert.get<Resource::ShaderResource>(),
                                            *shadowMaskVert.get<Resource::ShaderResource>(),
                                            *shadowMaskFrag.get<Resource::ShaderResource>());
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex);
            const auto plane = createLitPlane(renderer);
            MaterialDesc lambertDesc{white};
            lambertDesc.shading = ShadingMode::Lambert;
            lambertDesc.doubleSided = true;
            const auto mat = renderer.createMaterial(lambertDesc);

            // 1. 大范围偏移场景：在 (25, 0, 0) 处放置投射体与接收面（超出固定 {0,0,0}, halfExtent{6,6}）。
            constexpr FrameExtent extent{64, 64};
            constexpr size_t center = 32u * 64u + 32u;
            const auto offCenterProj = [&] {
                auto p = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 30.0f);
                p[1][1] *= -1.0f;
                return CameraMatrices{
                    glm::lookAtRH(glm::vec3(25.0f, 0.0f, 3.0f), glm::vec3(25.0f, 0.0f, 0.0f), glm::vec3(0, 1, 0)), p};
            }();
            const std::array offsetReceivers{
                DrawItem{plane, mat, glm::translate(glm::mat4(1.0f), {25.0f, 0.0f, 0.0f})},
            };
            const std::array offsetCasters{
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {25.0f, 0.0f, 1.0f}) *
                             glm::scale(glm::mat4(1.0f), {0.4f, 0.4f, 1.0f})},
            };
            const Lighting light{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};
            ShadowSettings fixedSettings;
            fixedSettings.enabled = true;
            FrameInput fixedInput{extent, offCenterProj, offsetReceivers, light,
                                  makeShadowFrame(fixedSettings, light.direction), offsetCasters};
            const auto hdrFixed = captureHdr(renderer, fixedInput);
            ASSERT_GT(hdrFixed.size(), center);
            EXPECT_GT(hdrFixed[center][0], 0.9f);  // 固定范围漏失阴影，保持全亮

            ShadowSettings fitSettings = fixedSettings;
            fitSettings.fitToBounds = true;
            fitSettings.resolution = 2048u;
            const Math::Aabb casterBounds{{24.0f, -1.0f, 0.0f}, {26.0f, 1.0f, 1.0f}};
            FrameInput fitInput{extent, offCenterProj, offsetReceivers, light,
                                makeShadowFrameForBounds(fitSettings, light.direction, casterBounds), offsetCasters};
            const auto hdrFitted = captureHdr(renderer, fitInput);
            ASSERT_GT(hdrFitted.size(), center);
            EXPECT_LT(hdrFitted[center][0], 0.1f);  // 自适应拟合精准覆盖并遮挡

            // 2. 斜面自阴影（Shadow Acne）与 normalBias 修复：75 度倾斜正面受 +Z 光源斜照。
            const auto slopeModel = glm::rotate(glm::mat4(1.0f), glm::radians(75.0f), {0.0f, 1.0f, 0.0f}) *
                                    glm::scale(glm::mat4(1.0f), {3.0f, 3.0f, 1.0f});
            const std::array slopeDraw{DrawItem{plane, mat, slopeModel}};
            const Lighting slopeLight{{0.0f, 0.0f, -1.0f}, {1, 1, 1}, 1.0f, 0.0f};
            ShadowSettings acneSettings;
            acneSettings.enabled = true;
            acneSettings.halfExtent = {6.0f, 6.0f};
            acneSettings.nearPlane = 1.0f;
            acneSettings.farPlane = 9.0f;
            acneSettings.depthBias = 0.0f;
            acneSettings.normalBias = 0.0f;
            acneSettings.resolution = 512u;
            FrameInput acneInput{extent,
                                 controlledFrame(extent, slopeDraw).camera,
                                 slopeDraw,
                                 slopeLight,
                                 makeShadowFrame(acneSettings, slopeLight.direction),
                                 slopeDraw};
            const auto hdrAcne = captureHdr(renderer, acneInput);
            float minAcne = 1.0f, maxAcne = 0.0f;
            for (int y = 26; y <= 38; ++y)
                for (int x = 26; x <= 38; ++x)
                {
                    const float r = hdrAcne[size_t(y) * 64u + size_t(x)][0];
                    minAcne = std::min(minAcne, r);
                    maxAcne = std::max(maxAcne, r);
                }
            EXPECT_GT(maxAcne - minAcne, 0.05f);  // 零偏移斜面存在明显自阴影条纹

            acneSettings.normalBias = 0.05f;
            acneInput.shadow = makeShadowFrame(acneSettings, slopeLight.direction);
            const auto hdrClean = captureHdr(renderer, acneInput);
            float minClean = 1.0f, maxClean = 0.0f;
            for (int y = 26; y <= 38; ++y)
                for (int x = 26; x <= 38; ++x)
                {
                    const float r = hdrClean[size_t(y) * 64u + size_t(x)][0];
                    minClean = std::min(minClean, r);
                    maxClean = std::max(maxClean, r);
                }
            const float expectedDiffuse = std::cos(glm::radians(75.0f));
            EXPECT_NEAR(minClean, expectedDiffuse, 0.005f);
            EXPECT_NEAR(maxClean, minClean, 0.005f);  // normalBias 消除自阴影，表面光滑一致受光

            // 3. 在正式 offscreen drawSceneTarget 路径下连续切换阴影分辨率 1024 <-> 2048，验证重建安全与零 Validation 错误。
            for (const uint32_t res : {1024u, 2048u, 1024u})
            {
                fitSettings.resolution = res;
                fitInput.shadow = makeShadowFrameForBounds(fitSettings, light.direction, casterBounds);
                for (int k = 0; k < 2; ++k)
                {
                    window->pollEvents();
                    ASSERT_TRUE(renderer.beginUiFrame());
                    const auto output = renderer.prepareSceneTarget(extent);
                    renderer.imgui().endFrame();
                    renderer.drawSceneTarget(output, fitInput, {0, 0, 0, 1});
                }
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadowTest, CascadedShadowMapsCloseSharpnessFarCoverageAlphaMaskAndResizeUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto& manager = Resource::ResourceManager::instance();
            const auto shadowVert =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
            const auto shadowMaskVert = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                               "shaders/shadow_mask.vert.spv");
            const auto shadowMaskFrag = manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER,
                                                                               "shaders/shadow_mask.frag.spv");
            ASSERT_TRUE(shadowVert && shadowMaskVert && shadowMaskFrag);
            renderer.prepareShadowResources(*shadowVert.get<Resource::ShaderResource>(),
                                            *shadowMaskVert.get<Resource::ShaderResource>(),
                                            *shadowMaskFrag.get<Resource::ShaderResource>());
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex);
            const auto plane = createLitPlane(renderer);
            MaterialDesc lambertDesc{white};
            lambertDesc.shading = ShadingMode::Lambert;
            lambertDesc.doubleSided = true;
            const auto mat = renderer.createMaterial(lambertDesc);

            // Alpha Mask 贴图：左半透明(alpha=0)，右半不透明(alpha=255)。
            const Resource::TextureResource maskTex({}, 2, 1, {255, 255, 255, 0, 255, 255, 255, 255});
            MaterialDesc maskDesc{renderer.createTexture(maskTex)};
            maskDesc.shading = ShadingMode::Lambert;
            maskDesc.alphaMode = AlphaMode::Mask;
            maskDesc.alphaCutoff = 0.5f;
            maskDesc.doubleSided = true;
            const auto maskMat = renderer.createMaterial(maskDesc);

            constexpr FrameExtent extent{128, 128};
            auto proj = glm::perspectiveRH_ZO(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
            proj[1][1] *= -1.0f;
            const CameraMatrices cam{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                proj};
            const Lighting light{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};

            // 窄细近景投射体（宽度 0.08 在 z = -2.5，投影到 z = -3.0 接收面中心）+ 远景物体（在 z = -35.0 投影到 z = -36.0）。
            const std::array nearDraws{
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -3.0f}) *
                             glm::scale(glm::mat4(1.0f), {1.5f, 1.5f, 1.0f})},
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -2.5f}) *
                             glm::scale(glm::mat4(1.0f), {0.04f, 0.5f, 1.0f})},
            };

            const Math::Aabb sceneBounds{{-15.0f, -15.0f, -45.0f}, {15.0f, 15.0f, 0.0f}};
            ShadowSettings singleLarge;
            singleLarge.enabled = true;
            singleLarge.resolution = 512u;
            singleLarge.cascadeCount = 1u;
            singleLarge.maxDistance = 60.0f;
            singleLarge.normalBias = 0.0f;
            // 单级大视锥覆盖 60 米范围时单个纹素大，窄细遮挡体投影会被严重模糊削弱。
            const auto singleShadow =
                makeCascadedShadowFrame(singleLarge, light.direction, cam, 0.1f, 100.0f, sceneBounds);
            ShadowSettings csmSettings = singleLarge;
            csmSettings.cascadeCount = 3u;
            csmSettings.cascadeSplitLambda = 0.75f;
            const auto csmShadow =
                makeCascadedShadowFrame(csmSettings, light.direction, cam, 0.1f, 100.0f, sceneBounds);
            ASSERT_TRUE(singleShadow && csmShadow);
            ASSERT_LT(csmShadow->cascadeSplits[0], 3.0f + 5.0f);  // 确保第 0 级紧密包住近处 z=-3

            // 检查近景阴影对比：在接收面（避开窄投射体正面直视的位置——或者把窄投射体放在光路上，但相机略微偏视角？
            // Wait：此处 light={0,0,-1} 与视线方向相同！如果投射体在 z=-2.5、相机在原点，相机也会直接看到投射体本身！
            // 把光线斜向照射：方向 (-0.5, 0, -1)，投射体置于 (0.5, 0, -2.0) 让阴影斜投射到 (-0.0, 0, -3.0) 接收面中心，
            // 从而相机正对 (-0.0, 0, -3.0) 只看到接收面上的清晰阴影而不被投射体挡住视线！
            const Lighting angledLight{glm::normalize(glm::vec3(-0.5f, 0.0f, -1.0f)), {1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};
            const std::array angledNearDraws{
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -3.0f}) *
                             glm::scale(glm::mat4(1.0f), {2.0f, 2.0f, 1.0f})},
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.5f, 0.0f, -2.0f}) *
                             glm::scale(glm::mat4(1.0f), {0.04f, 0.5f, 1.0f})},
            };
            const auto singleAngled =
                makeCascadedShadowFrame(singleLarge, angledLight.direction, cam, 0.1f, 100.0f, sceneBounds);
            const auto csmAngled =
                makeCascadedShadowFrame(csmSettings, angledLight.direction, cam, 0.1f, 100.0f, sceneBounds);

            constexpr size_t center = 64u * 128u + 64u;
            const auto hdrSingle = captureHdr(
                renderer, FrameInput{extent, cam, angledNearDraws, angledLight, singleAngled, angledNearDraws});
            const auto hdrCsm = captureHdr(
                renderer, FrameInput{extent, cam, angledNearDraws, angledLight, csmAngled, angledNearDraws});
            ASSERT_GT(hdrSingle.size(), center);
            ASSERT_GT(hdrCsm.size(), center);
            // 3 级联第 0 级分辨率密度远高于单级大范围，中心阴影遮挡明显更清晰更暗
            EXPECT_LT(hdrCsm[center][0], 0.1f);
            EXPECT_LT(hdrCsm[center][0] + 0.1f, hdrSingle[center][0]);

            // 验证远景（z = -36 接收面、z = -34 投射体，落在第 2 级联层内）正确采样高层级联投下阴影
            const std::array farDraws{
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, -36.0f}) *
                             glm::scale(glm::mat4(1.0f), {10.0f, 10.0f, 1.0f})},
                DrawItem{plane, mat,
                         glm::translate(glm::mat4(1.0f), {1.0f, 0.0f, -34.0f}) *
                             glm::scale(glm::mat4(1.0f), {1.0f, 1.0f, 1.0f})},
            };
            const auto hdrFarCsm =
                captureHdr(renderer, FrameInput{extent, cam, farDraws, angledLight, csmAngled, farDraws});
            EXPECT_LT(hdrFarCsm[center][0], 0.1f);

            // 验证 Alpha Mask 镂空在 3 级联下同样正确：左半透明（接收面受光），右半不透明（接收面被遮挡）
            const std::array maskCasters{
                DrawItem{plane, maskMat,
                         glm::translate(glm::mat4(1.0f), {0.5f, 0.0f, -2.0f}) *
                             glm::scale(glm::mat4(1.0f), {0.8f, 0.8f, 1.0f})},
            };
            const std::array maskReceivers{angledNearDraws[0]};
            const auto hdrMask = captureHdr(
                renderer, FrameInput{extent, cam, maskReceivers, angledLight, csmAngled, maskCasters});
            const size_t leftLit = 64u * 128u + 48u;
            const size_t rightDark = 64u * 128u + 80u;
            EXPECT_GT(hdrMask[leftLit][0], 0.5f);
            EXPECT_LT(hdrMask[rightDark][0], 0.1f);

            // 验证单级 <-> 3 级联切换及离屏目标尺寸变化在 drawSceneTarget 下零 Validation 错误
            for (const auto& [cascades, targetExt] :
                 {std::pair{1u, FrameExtent{96, 96}}, {3u, FrameExtent{96, 96}}, {3u, FrameExtent{160, 120}},
                  {1u, FrameExtent{160, 120}}})
            {
                csmSettings.cascadeCount = cascades;
                const auto frameShadow =
                    makeCascadedShadowFrame(csmSettings, angledLight.direction, cam, 0.1f, 100.0f, sceneBounds);
                window->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(targetExt);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(
                    output, FrameInput{targetExt, cam, angledNearDraws, angledLight, frameShadow, angledNearDraws},
                    {0, 0, 0, 1});
            }
        }
        expectCleanValidation();
    }
}  // namespace
