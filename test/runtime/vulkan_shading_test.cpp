#include "vulkan_review_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Render;
    using namespace Incubator::Render::Vulkan;
    using namespace Incubator::Test;

    class VulkanShadingTest : public VulkanReviewTest
    {};


    TEST_F(VulkanShadingTest, DoubleSidedLightsBackFacesAndSingleSidedCullsThem)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto plane = createLitPlane(renderer);
            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex);
            constexpr FrameExtent extent{64, 64};
            constexpr size_t center = (extent.height / 2) * extent.width + extent.width / 2;
            for (const auto shading : {ShadingMode::Lambert, ShadingMode::Pbr})
            {
                SCOPED_TRACE(static_cast<int>(shading));
                MaterialDesc desc{white, {0.6f, 0.6f, 0.6f}};
                desc.shading = shading;
                desc.doubleSided = true;
                std::array draws{DrawItem{plane, renderer.createMaterial(desc), glm::mat4(1)}};
                auto frame = controlledFrame(extent, draws);
                frame.lighting = {{0, 0, -1}, {1, 1, 1}, 1.0f, 0.0f};
                const auto front = captureHdr(renderer, frame);
                frame.camera.view = glm::lookAtRH(glm::vec3(0, 0, -3), glm::vec3(0), glm::vec3(0, 1, 0));
                frame.lighting.direction = {0, 0, 1};
                const auto back = captureHdr(renderer, frame);
                ASSERT_GT(front.size(), center);
                ASSERT_GT(back.size(), center);
                EXPECT_GT(front[center][0], 0.2f);
                EXPECT_NEAR(back[center][0], front[center][0], 0.005f);
                EXPECT_EQ(back[center][3], 1.0f);
                desc.doubleSided = false;
                draws[0].material = renderer.createMaterial(desc);
                const auto culled = captureHdr(renderer, frame);
                ASSERT_GT(culled.size(), center);
                EXPECT_EQ(culled[center], (std::array<float, 4>{0, 0, 0, 0}));
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, EmissiveTextureContributesInDarknessAndPassesThroughToneMapping)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const Resource::TextureResource glowTex({}, 1, 1, {255, 128, 64, 255});
            const auto white = renderer.createTexture(whiteTex);
            const auto glow = renderer.createTexture(glowTex);
            const auto plane = createLitPlane(renderer);
            constexpr FrameExtent extent{64, 64};
            constexpr size_t center = (extent.height / 2) * extent.width + extent.width / 2;
            const auto expected = linearFromSrgb(glm::vec3(255, 128, 64) / 255.0f) * glm::vec3(0.5f, 1.0f, 0.75f);
            for (const auto shading : {ShadingMode::Lambert, ShadingMode::Pbr})
            {
                SCOPED_TRACE(static_cast<int>(shading));
                MaterialDesc desc{white};
                desc.shading = shading;
                desc.emissiveTexture = glow;
                desc.emissiveFactor = {0.5f, 1.0f, 0.75f};
                const std::array draws{DrawItem{plane, renderer.createMaterial(desc), glm::mat4(1)}};
                auto frame = controlledFrame(extent, draws);
                frame.lighting = {{0, 0, -1}, {1, 1, 1}, 0.0f, 0.0f};
                frame.display = {ToneMappingMode::Reinhard, 1.0f};
                const auto hdr = captureHdr(renderer, frame);
                ASSERT_GT(hdr.size(), center);
                for (int channel = 0; channel < 3; ++channel)
                    EXPECT_NEAR(hdr[center][channel], expected[channel], 0.003f);
                EXPECT_EQ(hdr[center][3], 1.0f);
                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(extent);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame, {0, 0, 0, 1});
                Detail::RendererReviewAccess::withSceneOutput(renderer, output, [&](const auto& device, const auto& image) {
                    const auto ldr = TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                    const auto exposed = expected * 2.0f;
                    expectLinearPixel(ldr, exposed / (glm::vec3(1) + exposed), 32, 32, extent.width);
                });
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, AlphaBlendCompositesOverBackgroundAndOpaqueWithoutDepthWrite)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto white = renderer.createTexture(whiteTex);
            const auto plane = createLitPlane(renderer);

            auto makeUnlit = [&](std::array<float, 3> color, AlphaMode mode, float alpha = 1.0f) {
                MaterialDesc desc{white, color};
                desc.shading = ShadingMode::Unlit;
                desc.alphaMode = mode;
                desc.baseAlpha = alpha;
                desc.doubleSided = true;
                return renderer.createMaterial(desc);
            };

            const auto blendRed = makeUnlit({1.0f, 0.0f, 0.0f}, AlphaMode::Blend, 0.5f);
            const auto blendBlue = makeUnlit({0.0f, 0.0f, 1.0f}, AlphaMode::Blend, 0.5f);
            const auto opaqueGreen = makeUnlit({0.0f, 1.0f, 0.0f}, AlphaMode::Opaque);

            // 1. 纯背景上的半透明混合：验证离屏 HDR 路径（避免双重乘 Alpha/压暗）与直接呈现路径一致。
            {
                const std::array draws{DrawItem{plane, blendRed, glm::mat4(1.0f)}};
                const auto direct = TextureGpuReview::captureFrame(renderer, controlledFrame({384, 256}, draws));
                const size_t directCenter = (128u * 384u + 192u) * 4u;
                ASSERT_GT(direct.size(), directCenter + 3u);
                EXPECT_NEAR(direct[directCenter + 0], 192, 2);
                EXPECT_NEAR(direct[directCenter + 1], 64, 2);
                EXPECT_NEAR(direct[directCenter + 2], 64, 2);
                EXPECT_EQ(direct[directCenter + 3], 255);

                constexpr FrameExtent offscreenExtent{64, 64};
                auto offscreen = controlledFrame(offscreenExtent, draws);
                offscreen.display = {ToneMappingMode::Off, 0.0f};
                const auto hdr = captureHdr(renderer, offscreen);
                constexpr size_t hdrCenter = 32u * 64u + 32u;
                ASSERT_GT(hdr.size(), hdrCenter);
                EXPECT_NEAR(hdr[hdrCenter][0], 0.5f, 0.005f);
                EXPECT_NEAR(hdr[hdrCenter][1], 0.0f, 0.005f);
                EXPECT_NEAR(hdr[hdrCenter][2], 0.0f, 0.005f);
                EXPECT_NEAR(hdr[hdrCenter][3], 0.5f, 0.005f);

                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(offscreenExtent);
                renderer.imgui().endFrame();
                const float midGray = 128.0f / 255.0f;
                renderer.drawSceneTarget(output, offscreen, {midGray, midGray, midGray, 1.0f});
                Detail::RendererReviewAccess::withSceneOutput(renderer, output, [&](const auto& device, const auto& image) {
                    const auto ldr = TextureGpuReview::captureMip(device, image, 0, {64, 64});
                    const size_t offCenter = hdrCenter * 4u;
                    ASSERT_GT(ldr.size(), offCenter + 3u);
                    EXPECT_NEAR(ldr[offCenter + 0], 192, 2);
                    EXPECT_NEAR(ldr[offCenter + 1], 64, 2);
                    EXPECT_NEAR(ldr[offCenter + 2], 64, 2);
                    EXPECT_EQ(ldr[offCenter + 3], 255);
                });
            }

            // 2. 不透明背景之上的半透明混合（HDR 颜色与 LDR sRGB 输出）。
            {
                constexpr FrameExtent extent{64, 64};
                const std::array draws{
                    DrawItem{plane, opaqueGreen, glm::mat4(1.0f)},
                    DrawItem{plane, blendBlue, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.2f})},
                };
                auto frame = controlledFrame(extent, draws);
                frame.display = {ToneMappingMode::Off, 0.0f};
                const auto hdr = captureHdr(renderer, frame);
                constexpr size_t center = 32u * 64u + 32u;
                ASSERT_GT(hdr.size(), center);
                EXPECT_NEAR(hdr[center][0], 0.0f, 0.005f);
                EXPECT_NEAR(hdr[center][1], 0.5f, 0.005f);
                EXPECT_NEAR(hdr[center][2], 0.5f, 0.005f);
                EXPECT_EQ(hdr[center][3], 1.0f);

                ASSERT_TRUE(renderer.beginUiFrame());
                const auto output = renderer.prepareSceneTarget(extent);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame, {0.0f, 0.0f, 0.0f, 1.0f});
                Detail::RendererReviewAccess::withSceneOutput(renderer, output, [&](const auto& device, const auto& image) {
                    const auto ldr = TextureGpuReview::captureMip(device, image, 0, {64, 64});
                    expectLinearPixel(ldr, glm::vec3(0.0f, 0.5f, 0.5f), 32, 32, extent.width);
                });
            }

            // 3. 验证半透明不写入深度：先画近处 Blend，再画较远 Opaque 仍可写入；先画近处 Opaque 则遮挡较远 Blend。
            {
                constexpr FrameExtent extent{64, 64};
                constexpr size_t center = 32u * 64u + 32u;
                const std::array blendFirst{
                    DrawItem{plane, blendRed, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.5f})},
                    DrawItem{plane, opaqueGreen, glm::mat4(1.0f)},
                };
                const auto hdrAfterOpaque = captureHdr(renderer, controlledFrame(extent, blendFirst));
                ASSERT_GT(hdrAfterOpaque.size(), center);
                EXPECT_NEAR(hdrAfterOpaque[center][0], 0.0f, 0.005f);
                EXPECT_NEAR(hdrAfterOpaque[center][1], 1.0f, 0.005f);
                EXPECT_NEAR(hdrAfterOpaque[center][2], 0.0f, 0.005f);
                EXPECT_EQ(hdrAfterOpaque[center][3], 1.0f);

                const std::array opaqueFront{
                    DrawItem{plane, opaqueGreen, glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.5f})},
                    DrawItem{plane, blendRed, glm::mat4(1.0f)},
                };
                const auto hdrOccluded = captureHdr(renderer, controlledFrame(extent, opaqueFront));
                ASSERT_GT(hdrOccluded.size(), center);
                EXPECT_NEAR(hdrOccluded[center][0], 0.0f, 0.005f);
                EXPECT_NEAR(hdrOccluded[center][1], 1.0f, 0.005f);
                EXPECT_NEAR(hdrOccluded[center][2], 0.0f, 0.005f);
                EXPECT_EQ(hdrOccluded[center][3], 1.0f);
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, UInt32IndexDrawUsesVertexAboveUint16Range)
    {
        auto window = createWindow(true, 640, 480);
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            std::vector<MeshVertex> vertices(65537, MeshVertex{{4, 4, 0}, {0, 0}});
            vertices[0] = {{-0.65f, -0.55f, 0}, {0, 0}};
            vertices[1] = {{0.65f, -0.55f, 0}, {1, 0}};
            vertices[65536] = {{0, 0.65f, 0}, {0.5f, 1}};
            const auto mesh = renderer.createMesh(vertices, std::array<uint32_t, 3>{0, 1, 65536});
            releaseCpuAssets(assets);
            const std::array draws{DrawItem{mesh, material, glm::mat4(1)}};
            for (int f = 0; f < 6; ++f, glfwWaitEventsTimeout(0.025))
                submitControlledScene(*window, renderer, draws);
            const auto bounds = WindowCapture::bounds(WindowCapture::capture(*window));
            EXPECT_GT(bounds[2] - bounds[0], 100);
            EXPECT_GT(bounds[3] - bounds[1], 100);
        }
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, SamplerAndMipMaterialsSelectFiltersAndLevelsAfterCpuRelease)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(false, 640, 480);
        SceneAssets assets(referenceSceneAssetPaths());
        std::array<Texture, 2> texObs;
        std::array<Material, 8> matObs{};
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            texObs[0] = renderer.createTexture(Resource::TextureResource(
                {}, 2, 2, {0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255}));
            std::vector<uint8_t> mipPixels(4 * 4 * 4);
            for (size_t i = 0; i < 16; ++i)
                for (int c = 0; c < 4; ++c)
                    mipPixels[i * 4 + c] = (i % 4 >= 2) ? 255 : 0;
            const Resource::TextureResource mipSrc({}, 4, 4, std::move(mipPixels));
            EXPECT_THROW(renderer.createTexture(mipSrc, static_cast<TextureMipMode>(99)), Error::Exception);
            texObs[1] = renderer.createTexture(mipSrc, TextureMipMode::FullChain);

            MaterialDesc desc{texObs[0]};
            EXPECT_EQ(desc.sampler, SamplerDesc{});
            auto invalid = desc;
            invalid.sampler.minFilter = static_cast<TextureFilter>(99);
            EXPECT_THROW(renderer.createMaterial(invalid), Error::Exception);
            invalid = desc;
            invalid.sampler.mipFilter = static_cast<MipFilter>(99);
            EXPECT_THROW(renderer.createMaterial(invalid), Error::Exception);

            const std::array<SamplerDesc, 5> samplerModes{
                SamplerDesc{}, {TextureFilter::Nearest, TextureFilter::Linear, MipFilter::BaseLevel},
                {TextureFilter::Linear, TextureFilter::Nearest, MipFilter::BaseLevel},
                {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Nearest},
                {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear}};
            for (size_t i = 0; i < samplerModes.size(); ++i)
                matObs[i] = renderer.createMaterial({texObs[0], {1, 1, 1}, samplerModes[i]});
            const std::array mipModes{MipFilter::BaseLevel, MipFilter::Nearest, MipFilter::Linear};
            for (size_t i = 0; i < mipModes.size(); ++i)
                matObs[5 + i] = renderer.createMaterial(
                    {texObs[1], {1, 1, 1}, {TextureFilter::Nearest, TextureFilter::Nearest, mipModes[i]}});
            releaseCpuAssets(assets);

            const std::array<uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
            auto quadDraws = [&](float u0, float u1, const Material& base, const Material& alt) {
                const std::array<MeshVertex, 4> v{{{{-1, -1, 0.5f}, {u0, 0.5f}},
                                                   {{0, -1, 0.5f}, {u1, 0.5f}},
                                                   {{0, 1, 0.5f}, {u1, 0.5f}},
                                                   {{-1, 1, 0.5f}, {u0, 0.5f}}}};
                const auto mesh = renderer.createMesh(v, indices);
                return std::array{DrawItem{mesh, base, glm::mat4(1)},
                                  DrawItem{mesh, alt, glm::translate(glm::mat4(1), glm::vec3(1, 0, 0))}};
            };
            auto captureQuad = [&](const std::array<DrawItem, 2>& draws) {
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withFrame(
                    renderer, FrameInput{{256, 256}, {glm::mat4(1), glm::mat4(1)}, draws},
                    [&](const auto& d, const auto& p, auto fs, auto nd) {
                        pixels = TextureGpuReview::captureDraws(d, p, fs, nd, {0, 0, 256, 256, 0, 1});
                    });
                return pixels;
            };

            for (bool minify : {false, true})
                for (size_t i = 1; i < 5; ++i)
                {
                    const auto px = captureQuad(quadDraws(minify ? -128.0f : 0.5f, minify ? 0.0f : 0.5f,
                                                          matObs[0], matObs[i]));
                    const bool linear = i >= 3 || (minify ? i == 2 : i == 1);
                    for (int c = 0; c < 3; ++c)
                    {
                        EXPECT_NEAR(px[(128 * 384 + 64) * 4 + c], 255, 1);
                        EXPECT_NEAR(px[(128 * 384 + 192) * 4 + c], linear ? 188 : 255, 1);
                    }
                    EXPECT_EQ(px[(128 * 384 + 192) * 4 + 3], 255);
                }

            for (float lod : {0.0f, 1.25f, 1.75f, 10.0f})
            {
                const float step = std::exp2(lod) / 4.0f;
                const float u0 = 0.625f - 64.5f * step, u1 = u0 + 128.0f * step;
                for (size_t i = 1; i < 3; ++i)
                {
                    const auto draws = quadDraws(u0, u1, matObs[5], matObs[5 + i]);
                    const auto px = captureQuad(draws);
                    const float linear = i == 1 ? (lod < 1.5f ? 1.0f : 0.5f) : 1.0f - 0.5f * std::clamp(lod - 1.0f, 0.0f, 1.0f);
                    const float encoded = 255.0f * (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);
                    for (int c = 0; c < 3; ++c)
                    {
                        EXPECT_NEAR(px[(128 * 384 + 64) * 4 + c], 255, 1);
                        EXPECT_NEAR(px[(128 * 384 + 192) * 4 + c], encoded, 2);
                    }
                    if (lod == 1.75f && i == 1)
                        glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 800, 600);
                    window->pollEvents();
                    const auto extent = renderer.beginUiFrame();
                    ASSERT_TRUE(extent);
                    renderer.imgui().endFrame();
                    renderer.drawFrame({*extent, {glm::mat4(1), glm::mat4(1)}, draws}, {0.2f, 0.24f, 0.28f, 1});
                }
            }
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 4u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created material:"), 8u);
        }
        for (const auto& t : texObs)
            EXPECT_FALSE(t.valid());
        for (const auto& m : matObs)
            EXPECT_FALSE(m.valid());
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, GeneratedMipExtentsAndLinearSrgbAlphaMatchReadback)
    {
        auto window = createWindow();
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            for (const auto& chain : std::vector<std::vector<VkExtent2D>>{
                     {{1, 1}}, {{1, 9}, {1, 4}, {1, 2}, {1, 1}}, {{9, 1}, {4, 1}, {2, 1}, {1, 1}},
                     {{3, 5}, {1, 2}, {1, 1}}, {{4, 4}, {2, 2}, {1, 1}}})
            {
                std::vector<uint8_t> pixels(chain[0].width * chain[0].height * 4);
                for (size_t i = 0; i < pixels.size(); ++i)
                    pixels[i] = std::array<uint8_t, 4>{64, 128, 192, 85}[i % 4];
                const auto image = VulkanImage::uploadRgba8(
                    device, chain[0], VK_FORMAT_R8G8B8A8_SRGB, pixels,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, TextureMipMode::FullChain);
                ASSERT_EQ(image->mipLevels(), chain.size());
                for (uint32_t level = 0; level < chain.size(); ++level)
                {
                    const auto px = TextureGpuReview::captureMip(device, *image, level, chain[level]);
                    ASSERT_EQ(px.size(), chain[level].width * chain[level].height * 4);
                    for (size_t i = 0; i < px.size(); ++i)
                        EXPECT_NEAR(px[i], (std::array<uint8_t, 4>{64, 128, 192, 85}[i % 4]), 1);
                }
            }
            const std::array<uint8_t, 16> bw{0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255};
            const auto image = VulkanImage::uploadRgba8(device, {2, 2}, VK_FORMAT_R8G8B8A8_SRGB, bw,
                                                        VK_IMAGE_USAGE_SAMPLED_BIT, TextureMipMode::FullChain);
            ASSERT_EQ(image->mipLevels(), 2u);
            const auto mip = TextureGpuReview::captureMip(device, *image, 1, {1, 1});
            for (int c = 0; c < 3; ++c)
                EXPECT_NEAR(mip[c], 188, 1);
            EXPECT_NEAR(mip[3], 128, 1);
            EXPECT_EQ(VulkanImage::uploadRgba8(device, {2, 2}, VK_FORMAT_R8G8B8A8_SRGB, bw, VK_IMAGE_USAGE_SAMPLED_BIT)
                          ->mipLevels(),
                      1u);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, MaterialsCopyParametersShareTexturesAndPerItemDraws)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(true, 640, 480);
        FileAudit referenceAudit{referenceSceneAssetPaths()}, directionAudit{referenceSceneAssetPaths()};
        directionAudit.paths.texture = "textures/direction_rgba.png";
        Texture textureObserver;
        std::array<Material, 3> observers;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            Material white, half, direction;
            {
                SceneAssets assets(referenceAudit.paths, std::make_unique<AuditedFileIO>(referenceAudit));
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                textureObserver = renderer.createTexture(*assets.texture);
                MaterialDesc desc{textureObserver};
                EXPECT_EQ(desc.color, (std::array{1.0f, 1.0f, 1.0f}));
                white = renderer.createMaterial(desc);
                desc.color = {0.25f, 0.5f, 1.0f};
                half = renderer.createMaterial(desc);
                EXPECT_FALSE(Material{}.valid());
                for (int c = 0; c < 3; ++c)
                    for (const float v : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(),
                                          std::numeric_limits<float>::quiet_NaN()})
                    {
                        auto invalid = desc;
                        invalid.color[c] = v;
                        EXPECT_THROW(renderer.createMaterial(invalid), Error::Exception);
                    }
                desc = {};
                releaseCpuAssets(assets);
            }
            {
                SceneAssets assets(directionAudit.paths, std::make_unique<AuditedFileIO>(directionAudit));
                direction = renderer.createMaterial({renderer.createTexture(*assets.texture)});
                releaseCpuAssets(assets);
            }
            observers = {white, half, direction};
            const auto pd = Demo::makePlaneMesh();
            auto plane = renderer.createMesh(pd.vertices, pd.indices);
            auto present = [&](const Material& selected) {
                for (int f = 0; f < 6; ++f, glfwWaitEventsTimeout(0.03))
                    submitControlledScene(*window, renderer, std::array{DrawItem{plane, selected, glm::mat4(1)}});
                return WindowCapture::capture(*window);
            };
            (void)present(white);
            const auto original = present(white);
            white = half = direction = {};
            expectInteriorEqual(original, present(observers[0]));
            const auto image = present(observers[1]);
            const auto b = WindowCapture::bounds(image);
            ASSERT_GT(b[2] - b[0], 100);
            const auto offset = (size_t(b[1] + (b[3] - b[1]) / 2) * image.width + b[0] + 7 * (b[2] - b[0]) / 8) * 4;
            EXPECT_GT(int(image.rgba[offset + 1]), int(image.rgba[offset]) + 20);
            EXPECT_GT(int(image.rgba[offset + 2]), int(image.rgba[offset + 1]) + 20);
            for (int f = 0; f < 4; ++f)
            {
                const bool sw = f % 2 != 0;
                std::vector<DrawItem> draws = {
                    {plane, observers[sw ? 1 : 0], glm::translate(glm::mat4(1), {-0.65f, 0.45f, 0}) * glm::scale(glm::mat4(1), glm::vec3(0.65f))},
                    {plane, observers[sw ? 0 : 1], glm::translate(glm::mat4(1), {0.65f, 0.45f, 0}) * glm::scale(glm::mat4(1), glm::vec3(0.65f))},
                    {plane, observers[2], glm::translate(glm::mat4(1), {0.65f, -0.45f, 0}) * glm::scale(glm::mat4(1), glm::vec3(0.65f))}};
                if (sw)
                    std::reverse(draws.begin(), draws.end());
                submitControlledScene(*window, renderer, draws, true);
                draws.clear();
                glfwWaitEventsTimeout(0.06);
                const auto img = WindowCapture::capture(*window);
                const auto px = [&](float x, float y, int c) {
                    return img.rgba[(size_t(img.height / 2 - int(y * img.height / 2)) * img.width + img.width / 2 +
                                     int(x * img.height / 2)) * 4 + c];
                };
                EXPECT_NEAR(px(-0.65f + 0.24375f, 0.45f, 0), sw ? 137 : 255, 14);
                EXPECT_NEAR(px(0.65f + 0.24375f, 0.45f, 0), sw ? 255 : 137, 14);
                EXPECT_GT(px(0.65f + 0.21667f, -0.45f + 0.08125f, 0), 220);
                EXPECT_LT(px(0.65f + 0.21667f, -0.45f + 0.08125f, 1), 30);
            }
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 4u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created material:"), 3u);
            EXPECT_TRUE(referenceAudit.cpuReleasedBeforeDestroy && directionAudit.cpuReleasedBeforeDestroy);
        }
        EXPECT_FALSE(textureObserver.valid());
        for (const auto& material : observers)
            EXPECT_FALSE(material.valid());
        expectReleased(referenceAudit.paths);
        expectReleased(directionAudit.paths);
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, MaterialTintAndFormatRebuildMatchLinearRgbOnGpu)
    {
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(false, 384, 256);
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            VulkanSwapchain swapchain(device, surface, 384, 256);
            const auto check = [&](VkFormat format, VkFormat rebuildFrom) {
                SceneAssets assets(referenceSceneAssetPaths());
                const auto pixels =
                    TextureGpuReview::capture(instance.handle(), device, *window, assets, format, {0.5f, 0.5f, 0.5f},
                                              rebuildFrom);
                const size_t white = (112 * 384 + 240) * 4, gray = (112 * 384 + 176) * 4;
                for (int c = 0; c < 3; ++c)
                {
                    EXPECT_NEAR(pixels[white + c], 188, 1);
                    EXPECT_NEAR(pixels[gray + c], 92, 1);
                    EXPECT_NEAR(pixels[(64 * 384 + 320) * 4 + c], 128, 1);
                    EXPECT_NEAR(pixels[(160 * 384 + 320) * 4 + c], 128, 1);
                }
                EXPECT_EQ(pixels[white + 3], 255);
                EXPECT_EQ(pixels[((16 + 2 * 64 + 32) * 384 + 16 + 2 * 64 + 32) * 4 + 3], 255);
                expectReleased(referenceSceneAssetPaths());
            };
            check(swapchain.format(), VK_FORMAT_UNDEFINED);
            const auto alt =
                swapchain.format() == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
            check(alt, swapchain.format());
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 3u);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, TextureSamplesAndUploadValidationMatchReferenceDirectionAndUi)
    {
        auto window = createWindow(false, 384, 256);
        {
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            const std::array<uint8_t, 4> pixel{128, 128, 128, 255};
            EXPECT_THROW(VulkanImage::uploadRgba8(device, {device.properties().limits.maxImageDimension2D + 1, 1},
                                                  VK_FORMAT_R8G8B8A8_SRGB, pixel, VK_IMAGE_USAGE_SAMPLED_BIT),
                         Error::Exception);
            EXPECT_THROW(
                VulkanImage::uploadRgba8(device, {2, 1}, VK_FORMAT_R8G8B8A8_SRGB, pixel, VK_IMAGE_USAGE_SAMPLED_BIT),
                Error::Exception);
            auto image = VulkanImage::uploadRgba8(device, {1, 1}, VK_FORMAT_R8G8B8A8_SRGB, pixel, VK_IMAGE_USAGE_SAMPLED_BIT);
            EXPECT_EQ(image->format(), VK_FORMAT_R8G8B8A8_SRGB);
            EXPECT_NE(image->view(), VK_NULL_HANDLE);
            VulkanSwapchain swapchain(device, surface, 384, 256);
            EXPECT_EQ(swapchain.colorSpace(), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

            const std::array<std::array<int, 3>, 12> ref{{{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0},
                                                          {0, 255, 255}, {255, 0, 255}, {128, 128, 128}, {255, 255, 255},
                                                          {255, 128, 0}, {128, 0, 255}, {17, 34, 51}, {0, 0, 0}}};
            const std::vector<uint8_t> dir{255, 255, 255, 255, 255, 128, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255,
                                           128, 128, 128, 255, 255, 0, 0, 255, 0, 255, 0, 255, 255, 0, 255, 128,
                                           0, 255, 255, 255, 17, 34, 51, 0, 255, 255, 0, 255, 128, 0, 255, 255};
            for (const bool direction : {false, true})
            {
                auto paths = referenceSceneAssetPaths();
                if (direction)
                    paths.texture = "textures/direction_rgba.png";
                SceneAssets assets(paths);
                const int cols = direction ? 3 : 4, rows = direction ? 4 : 3;
                ASSERT_EQ(assets.texture->width(), cols);
                ASSERT_EQ(assets.texture->height(), rows);
                if (direction)
                    EXPECT_EQ(assets.texture->pixels(), dir);
                const auto px = TextureGpuReview::capture(instance.handle(), device, *window, assets, swapchain.format());
                for (int r = 0; r < rows; ++r)
                    for (int c = 0; c < cols; ++c)
                    {
                        const size_t src = r * cols + c;
                        const size_t off = ((16 + (2 * r + 1) * 192 / (2 * rows)) * 384 + 16 + (2 * c + 1) * 256 / (2 * cols)) * 4;
                        for (int ch = 0; ch < 3; ++ch)
                            EXPECT_NEAR(px[off + ch], direction ? dir[src * 4 + ch] : ref[src][ch], 1);
                        EXPECT_EQ(px[off + 3], 255);
                    }
                for (const auto p : {std::array{320, 64}, std::array{320, 160}})
                    for (int ch = 0; ch < 3; ++ch)
                        EXPECT_NEAR(px[(p[1] * 384 + p[0]) * 4 + ch], 128, 1);
                EXPECT_EQ(px[(34 * 384 + 290) * 4], 255);
                EXPECT_EQ(px[(34 * 384 + 290) * 4 + 1], 0);
                EXPECT_EQ(px[(34 * 384 + 290) * 4 + 2], 255);
                expectReleased(paths);
            }
        }
        expectCleanValidation();
    }
    TEST_F(VulkanShadingTest, LambertShaderMathAndAllNormalBranchesOnGpu)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto tintTex = renderer.createTexture(Resource::TextureResource({}, 1, 1, {128, 64, 192, 0}));
            const auto whiteTex = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            auto plane = Demo::makePlaneMesh();
            const auto zeroMesh = renderer.createMesh(plane.vertices, plane.indices);
            for (auto& vertex : plane.vertices)
                vertex.normal[2] = 1;
            const auto normMesh = renderer.createMesh(plane.vertices, plane.indices);

            const std::array<float, 3> tint{0.5f, 0.75f, 0.25f};
            MaterialDesc desc{tintTex, tint};
            EXPECT_EQ(desc.shading, ShadingMode::Unlit);
            const auto unlit = renderer.createMaterial(desc);
            desc.shading = static_cast<ShadingMode>(99);
            EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            desc.shading = ShadingMode::Lambert;
            const auto lit = renderer.createMaterial(desc);
            const auto whiteLit = renderer.createMaterial({whiteTex, {1, 1, 1}, {}, ShadingMode::Lambert});
            desc = {};
            releaseCpuAssets(assets);

            // 分支 1 & 2：线性 ambient+directional 叠加钳位，以及与 Unlit 共存。
            const auto base =
                linearFromSrgb({128 / 255.0f, 64 / 255.0f, 192 / 255.0f}) * glm::vec3(tint[0], tint[1], tint[2]);
            std::array draws{DrawItem{normMesh, lit, glm::mat4(1)}};
            auto frame = controlledFrame({384, 256}, draws);
            frame.lighting = {{0, 0, -1}, {0.3f, 0.7f, 0.5f}, 1.2f, 0.15f};
            for (const float cosine : {1.0f, 0.5f, 0.0f, -1.0f})
            {
                frame.lighting.direction = {-std::sqrt(1 - cosine * cosine), 0, -cosine};
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame),
                                  base * (glm::vec3(0.15f) + frame.lighting.color * 1.2f * std::max(cosine, 0.0f)));
                draws[0].material = unlit;
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), base);
                draws[0].material = lit;
            }
            frame.lighting.intensity = 0;
            frame.lighting.ambient = 0.25f;
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), base * 0.25f);
            frame.lighting.ambient = 0;
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), glm::vec3(0));
            frame.lighting = {{0, 0, -1}, {1, 1, 1}, 1000, 0};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), glm::vec3(1));

            // 分支 3：零/奇异矩阵与三角内部抵消法线均退回仅输出 ambient。
            const auto clip = [](float x, float y) { return std::array<float, 3>{x / 192 - 1, y / 128 - 1, 0.5f}; };
            const auto a = clip(64.5f, 64.5f), b = clip(320.5f, 64.5f), c = clip(64.5f, 192.5f);
            const auto cancelling = renderer.createMesh(
                std::array{MeshVertex{{a[0], a[1], a[2]}, {0, 0}, {0, 0, 1}},
                           MeshVertex{{b[0], b[1], b[2]}, {0, 0}, {0, 0, -1}},
                           MeshVertex{{c[0], c[1], c[2]}, {0, 0}, {0, 0, -1}}},
                std::array<uint32_t, 3>{0, 1, 2});
            auto projective = glm::mat4(1);
            projective[0][3] = 0.25f;
            frame.lighting = {{0, 0, -1}, {1, 1, 1}, 1, 0.2f};
            draws[0] = {zeroMesh, lit, glm::mat4(1)};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), base * 0.2f);
            for (const auto& singular : {glm::scale(glm::mat4(1), glm::vec3(1, 1, 0)), projective})
            {
                draws[0] = {normMesh, lit, singular};
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, frame), base * 0.2f);
            }
            draws[0] = {cancelling, lit, glm::mat4(1)};
            FrameInput cancelFrame{{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws, frame.lighting};
            for (const float sign : {-1.0f, 1.0f})
            {
                cancelFrame.lighting.direction.z = sign;
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, cancelFrame), base * 0.2f, 128, 96);
            }

            // 分支 4：非单位法线量级与非均匀缩放下先归一化再光照。
            for (const float magnitude : {1.0f, 1e20f, 1e38f})
            {
                for (auto& vertex : plane.vertices)
                    vertex.normal[2] = magnitude;
                const auto bigNorm = renderer.createMesh(plane.vertices, plane.indices);
                const std::array d{DrawItem{bigNorm, whiteLit, glm::scale(glm::mat4(1), glm::vec3(1, 1, 0.001f))}};
                auto f = controlledFrame({384, 256}, d);
                f.lighting = {{0, 0, -1}, {1, 1, 1}, 1, 0};
                expectLinearPixel(TextureGpuReview::captureFrame(renderer, f), glm::vec3(1));
            }

            // 分支 5：从镜像后的正面观察，负 Z 缩放使 -Z 侧成为正面并使法线指向 -Z。
            const std::array negDraws{DrawItem{normMesh, whiteLit, glm::scale(glm::mat4(1), glm::vec3(1, 1, -1))}};
            auto negFrame = controlledFrame({384, 256}, negDraws);
            negFrame.camera.view = glm::lookAtRH(glm::vec3(0, 0, -3), glm::vec3(0), glm::vec3(0, 1, 0));
            negFrame.lighting = {{0, 0, -1}, {1, 1, 1}, 0.7f, 0.2f};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, negFrame), glm::vec3(0.2f));
            negFrame.lighting.direction = {0, 0, 1};
            expectLinearPixel(TextureGpuReview::captureFrame(renderer, negFrame), glm::vec3(0.9f));
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline:"), 4u);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, LambertFrameSlotsShareLightingAndRejectInvalidValuesCleanly)
    {
        Log::Registry::get("render").setLevel(Log::Level::DEBUG);
        auto native = createWindow(true, 768, 512);
        DeferredFramebufferWindow window(*native);
        window.drawable = true;
        SceneAssets assets;
        Mesh meshObserver;
        Material materialObserver;
        {
            Renderer renderer(window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            auto data = Demo::makePlaneMesh();
            for (auto& vertex : data.vertices)
                vertex.normal[2] = 1;
            meshObserver = renderer.createMesh(data.vertices, data.indices);
            materialObserver = renderer.createMaterial(
                {renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 0})),
                 {0.25f, 0.5f, 0.75f},
                 {},
                 ShadingMode::Lambert});
            releaseCpuAssets(assets);
            Renderer moved(std::move(renderer));
            for (int f = 0; f < 4; ++f, glfwWaitEventsTimeout(0.02))
                submitControlledScene(window, moved, {}, true);
            std::array draws{
                DrawItem{meshObserver, materialObserver, glm::translate(glm::mat4(1), glm::vec3(-0.6f, 0, 0))},
                DrawItem{meshObserver, materialObserver, glm::translate(glm::mat4(1), glm::vec3(0.6f, 0, 0))}};
            for (int index = 0; index < 6; ++index)
            {
                window.pollEvents();
                const auto extent = moved.beginUiFrame();
                ASSERT_TRUE(extent);
                moved.imgui().endFrame();
                auto frame = controlledFrame(*extent, draws);
                frame.lighting = index % 2 ? Lighting{{0, 0, 1}, {1, 0.2f, 0.5f}, 1, 0.3f}
                                           : Lighting{{0, 0, -2}, {0.4f, 0.6f, 1}, 0.8f, 0.1f};
                const auto expected =
                    glm::vec3(0.25f, 0.5f, 0.75f) *
                    (glm::vec3(frame.lighting.ambient) + (index % 2 ? glm::vec3(0) : frame.lighting.color * 0.8f));
                moved.drawFrame(frame, {0, 0, 0, 1});
                frame.lighting = {{1, 0, 0}, {0, 0, 0}, 0, 0};
                glfwWaitEventsTimeout(0.06);
                const auto image = WindowCapture::capture(window);
                const float aspect = float(extent->width) / extent->height;
                for (const float x : {-0.6f, 0.6f})
                    expectLinearPixel(image.rgba, expected, static_cast<uint32_t>((x / aspect + 1) * image.width * 0.5f),
                                      image.height / 2, image.width);
            }
            window.throwOnSize = true;
            auto badFrame = Demo::makeSceneFrame({640, 360}, draws);
            for (const auto& invalid : {Lighting{{0, 0, 0}}, Lighting{{std::numeric_limits<float>::quiet_NaN(), 0, -1}},
                                        Lighting{{0, 0, -1}, {0, std::numeric_limits<float>::infinity(), 0}},
                                        Lighting{{0, 0, -1}, {1, 1, 1.1f}}, Lighting{{0, 0, -1}, {1, 1, 1}, -1},
                                        Lighting{{0, 0, -1}, {1, 1, 1}, 1, -0.1f},
                                        Lighting{{0, 0, -1}, {1, 1, 1}, std::numeric_limits<float>::infinity()}})
            {
                badFrame.lighting = invalid;
                bool recorded = false;
                EXPECT_THROW(moved.drawFrame(badFrame, {0, 0, 0, 1}, [&](void*) { recorded = true; }), Error::Exception);
                EXPECT_THROW(Detail::RendererReviewAccess::withFrame(
                                 moved, badFrame, [&](const auto&, const auto&, auto, auto) { recorded = true; }),
                             Error::Exception);
                EXPECT_FALSE(recorded);
            }
            window.throwOnSize = false;
            EXPECT_GT(drawUiFrame(window, moved, meshObserver, materialObserver), 0);
        }
        EXPECT_FALSE(meshObserver.valid() || materialObserver.valid());
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, FinalModelNormalMatricesAndDegenerateUnlitDrawsRemainValid)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material =
                renderer.createMaterial({renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}))});
            releaseCpuAssets(assets);
            auto data = Demo::makePlaneMesh();
            for (int i = 0; i < 3; ++i)
                data.vertices[i].normal[i] = float(i + 1);
            const auto mesh = renderer.createMesh(data.vertices, data.indices);
            const auto rot = glm::rotate(glm::mat4(1), glm::radians(37.0f), glm::normalize(glm::vec3(1, 2, 3)));
            const auto nonuniform = rot * glm::scale(glm::mat4(1), glm::vec3(2, 3, 0.5f));
            auto projective = glm::mat4(1);
            projective[0][3] = 0.25f;
            const std::array models = {glm::mat4(1),
                                       rot,
                                       nonuniform,
                                       rot * glm::scale(glm::mat4(1), glm::vec3(-2, 3, 0.5f)),
                                       rot * glm::scale(glm::mat4(1), glm::vec3(0.001f)),
                                       glm::translate(glm::mat4(1), glm::vec3(7, -2, 4)) * nonuniform,
                                       glm::scale(glm::mat4(1), glm::vec3(1, 1, 0)),
                                       projective};
            std::vector<DrawItem> draws;
            for (const auto& model : models)
                draws.push_back({mesh, material, model});
            Detail::RendererReviewAccess::withFrame(
                renderer, FrameInput{{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws},
                [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                    ASSERT_EQ(nativeDraws.size(), models.size());
                    for (size_t i = 0; i < models.size(); ++i)
                    {
                        const auto& object = nativeDraws[i].object;
                        const auto ref = i < 6 ? glm::transpose(glm::inverse(glm::dmat3(models[i]))) : glm::dmat3(0);
                        for (int c = 0; c < 4; ++c)
                            EXPECT_EQ(object.model[c], models[i][c]);
                        for (int c = 0; c < 3; ++c)
                        {
                            EXPECT_EQ(object.normalColumns[c].w, 0);
                            for (int r = 0; r < 3; ++r)
                            {
                                EXPECT_TRUE(std::isfinite(object.normalColumns[c][r]));
                                EXPECT_NEAR(object.normalColumns[c][r], ref[c][r],
                                            0.00001 * std::max(1.0, std::abs(ref[c][r])));
                            }
                        }
                    }
                    for (int c = 0; c < 3; ++c)
                        EXPECT_EQ(nativeDraws[2].object.normalColumns[c], nativeDraws[5].object.normalColumns[c]);
                    for (size_t index : {6u, 7u})
                    {
                        const auto px =
                            TextureGpuReview::captureDraws(device, pipeline, frameSet, nativeDraws.subspan(index, 1));
                        for (int c = 0; c < 4; ++c)
                            EXPECT_EQ(px[(128 * 384 + 192) * 4 + c], 255);
                    }
                });
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, MeshTangentValidationAndNormalization)
    {
        auto window = createWindow();
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            SceneAssets assets;
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto tex = renderer.createTexture(*assets.texture);
            const auto mat = renderer.createMaterial({tex});

            const std::array<uint32_t, 3> indices{0, 1, 2};
            for (size_t component = 0; component < 4; ++component)
            {
                for (const float bad : {std::numeric_limits<float>::quiet_NaN(),
                                        std::numeric_limits<float>::infinity(),
                                        -std::numeric_limits<float>::infinity()})
                {
                    std::array<MeshVertex, 3> badVerts{{{{0, 0, 0}, {0, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                                                        {{1, 0, 0}, {1, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                                                        {{0, 1, 0}, {0, 1}, {0, 0, 1}, {1, 0, 0, 1}}}};
                    badVerts[1].tangent[component] = bad;
                    EXPECT_THROW(renderer.createMesh(badVerts, indices), Error::Exception);
                }
            }

            // 覆盖超大向量双精度归一化、手性符号规范化、零向量/零手性归零。
            const std::array<MeshVertex, 4> validVerts{{
                {{-1, -1, 0}, {0, 0}, {0, 0, 1}, {3e20f, 4e20f, 0.0f, -2.5f}},
                {{1, -1, 0}, {1, 0}, {0, 0, 1}, {0.0f, 5.0f, 0.0f, 0.25f}},
                {{1, 1, 0}, {1, 1}, {0, 0, 1}, {0.0f, 0.0f, 0.0f, 1.0f}},
                {{-1, 1, 0}, {0, 1}, {0, 0, 1}, {1.0f, 0.0f, 0.0f, 0.0f}},
            }};
            const std::array<uint32_t, 6> quadIndices{0, 1, 2, 2, 3, 0};
            const auto mesh = renderer.createMesh(validVerts, quadIndices);
            std::vector<uint8_t> pixels;
            Detail::RendererReviewAccess::withFrame(
                renderer,
                FrameInput{{256, 256}, {glm::mat4(1), glm::mat4(1)}, std::array{DrawItem{mesh, mat, glm::mat4(1)}}},
                [&](const auto& d, const auto& p, auto fs, auto nd) {
                    pixels = TextureGpuReview::captureDraws(d, p, fs, nd, {0, 0, 256, 256, 0, 1});
                });
            EXPECT_FALSE(pixels.empty());
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, TextureColorSpaceAndPbrMaterialValidation)
    {
        auto window = createWindow();
        auto otherWindow = createWindow();
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            SceneAssets assets;
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            const Resource::TextureResource srgb1x1({}, 1, 1, {255, 255, 255, 255});
            const Resource::TextureResource linear4x4({}, 4, 4, std::vector<uint8_t>(4 * 4 * 4, 128));
            EXPECT_THROW(renderer.createTexture(srgb1x1, TextureMipMode::BaseLevel,
                                                static_cast<TextureColorSpace>(99)),
                         Error::Exception);

            const auto baseColor = renderer.createTexture(srgb1x1, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);
            const auto linearBase =
                renderer.createTexture(srgb1x1, TextureMipMode::BaseLevel, TextureColorSpace::Linear);
            const auto normalFullMip =
                renderer.createTexture(linear4x4, TextureMipMode::FullChain, TextureColorSpace::Linear);
            const auto mrLinear =
                renderer.createTexture(linear4x4, TextureMipMode::FullChain, TextureColorSpace::Linear);

            // 基色纹理必须为 Srgb；法线与金属粗糙度纹理必须为 Linear。
            EXPECT_THROW(renderer.createMaterial({linearBase}), Error::Exception);
            {
                MaterialDesc desc{baseColor};
                desc.normalTexture = baseColor;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            {
                MaterialDesc desc{baseColor};
                desc.metallicRoughnessTexture = baseColor;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            // 属于其他 Renderer 或已过期的非空句柄必须被拒绝。
            {
                struct RestoreContext
                {
                    ImGuiContext* previous;
                    ~RestoreContext() { ImGui::SetCurrentContext(previous); }
                } restore{ImGui::GetCurrentContext()};
                ImGui::SetCurrentContext(nullptr);
                Renderer otherRenderer(*otherWindow, RendererValidationMode::Required);
                const auto otherLinear =
                    otherRenderer.createTexture(srgb1x1, TextureMipMode::BaseLevel, TextureColorSpace::Linear);
                MaterialDesc desc{baseColor};
                desc.normalTexture = otherLinear;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            {
                Texture expiredLinear;
                {
                    auto tempGroup = renderer.createResources();
                    expiredLinear = renderer.createTexture(tempGroup, srgb1x1, TextureMipMode::BaseLevel,
                                                           TextureColorSpace::Linear);
                }
                ASSERT_FALSE(expiredLinear.valid());
                MaterialDesc desc{baseColor};
                desc.metallicRoughnessTexture = expiredLinear;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }

            // 越界或非有限 PBR 参数与非法着色模式必须抛出 RENDER。
            for (const float badScale : {-0.1f, std::numeric_limits<float>::quiet_NaN(),
                                         std::numeric_limits<float>::infinity()})
            {
                MaterialDesc desc{baseColor};
                desc.shading = ShadingMode::Pbr;
                desc.normalScale = badScale;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            for (const float badFactor : {-0.01f, 1.01f, std::numeric_limits<float>::quiet_NaN(),
                                          std::numeric_limits<float>::infinity()})
            {
                MaterialDesc desc{baseColor};
                desc.shading = ShadingMode::Pbr;
                desc.metallic = badFactor;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
                desc.metallic = 0.5f;
                desc.roughness = badFactor;
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }
            {
                MaterialDesc desc{baseColor};
                desc.shading = static_cast<ShadingMode>(99);
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
            }

            // 资源组持有期：材质强持有 normalTexture 与 metallicRoughnessTexture，释放纹理组后仍存活。
            {
                auto texGroup = renderer.createResources();
                auto matGroup = renderer.createResources();
                const auto scopedNormal =
                    renderer.createTexture(texGroup, linear4x4, TextureMipMode::FullChain, TextureColorSpace::Linear);
                const auto scopedMr =
                    renderer.createTexture(texGroup, linear4x4, TextureMipMode::BaseLevel, TextureColorSpace::Linear);
                MaterialDesc desc{baseColor};
                desc.shading = ShadingMode::Pbr;
                desc.normalTexture = scopedNormal;
                desc.metallicRoughnessTexture = scopedMr;
                desc.normalScale = 1.0f;
                desc.metallic = 0.2f;
                desc.roughness = 0.6f;
                const auto scopedMat = renderer.createMaterial(matGroup, desc);
                texGroup.reset();
                EXPECT_TRUE(scopedNormal.valid());
                EXPECT_TRUE(scopedMr.valid());
                EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).textures, 6u);
                matGroup.reset();
                EXPECT_FALSE(scopedMat.valid());
                EXPECT_FALSE(scopedNormal.valid());
                EXPECT_FALSE(scopedMr.valid());
                EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).textures, 4u);
            }

            // Required Validation 下分别使用共享回退贴图与实际 FullChain 数据贴图完成绘制（含 1x1 基色 + FullChain 法线）。
            MaterialDesc fallbackPbr{baseColor};
            fallbackPbr.shading = ShadingMode::Pbr;
            fallbackPbr.sampler = {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear};
            const auto matFallback = renderer.createMaterial(fallbackPbr);

            MaterialDesc fullPbr{baseColor};
            fullPbr.shading = ShadingMode::Pbr;
            fullPbr.sampler = {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear};
            fullPbr.normalTexture = normalFullMip;
            fullPbr.metallicRoughnessTexture = mrLinear;
            fullPbr.normalScale = 1.0f;
            fullPbr.metallic = 0.8f;
            fullPbr.roughness = 0.3f;
            const auto matFull = renderer.createMaterial(fullPbr);

            const auto quad = Demo::makePlaneMesh();
            const auto mesh = renderer.createMesh(quad.vertices, quad.indices);
            const std::array draws{
                DrawItem{mesh, matFallback, glm::translate(glm::mat4(1), glm::vec3(-0.5f, 0, 0))},
                DrawItem{mesh, matFull, glm::translate(glm::mat4(1), glm::vec3(0.5f, 0, 0))},
            };
            std::vector<uint8_t> pixels;
            Detail::RendererReviewAccess::withFrame(
                renderer, FrameInput{{256, 256}, {glm::mat4(1), glm::mat4(1)}, draws},
                [&](const auto& d, const auto& p, auto fs, auto nd) {
                    pixels = TextureGpuReview::captureDraws(d, p, fs, nd, {0, 0, 256, 256, 0, 1});
                });
            EXPECT_FALSE(pixels.empty());
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, PbrShadingNormalPerturbationGgxAndMirroredTbn)
    {
        auto window = createWindow();
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            SceneAssets assets;
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            const Resource::TextureResource whiteTex({}, 1, 1, {255, 255, 255, 255});
            const auto baseWhite = renderer.createTexture(whiteTex, TextureMipMode::BaseLevel, TextureColorSpace::Srgb);

            // +Y 切线空间法线扰动：nTs ≈ (0, 0.8, 0.6) -> RGBA (128, 230, 204, 255)。
            const Resource::TextureResource plusYNormalTex({}, 1, 1, {128, 230, 204, 255});
            const auto plusYNormal =
                renderer.createTexture(plusYNormalTex, TextureMipMode::BaseLevel, TextureColorSpace::Linear);

            auto projection = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
            projection[1][1] *= -1.0f;
            const CameraMatrices camera{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                projection};

            const std::array<uint32_t, 6> indices{0, 1, 2, 2, 3, 0};
            const std::array<MeshVertex, 4> tangentQuad{{
                {{-1, -1, 0}, {0, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                {{1, -1, 0}, {1, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                {{1, 1, 0}, {1, 1}, {0, 0, 1}, {1, 0, 0, 1}},
                {{-1, 1, 0}, {0, 1}, {0, 0, 1}, {1, 0, 0, 1}},
            }};
            const std::array<MeshVertex, 4> zeroTangentQuad{{
                {{-1, -1, 0}, {0, 0}, {0, 0, 1}, {0, 0, 0, 0}},
                {{1, -1, 0}, {1, 0}, {0, 0, 1}, {0, 0, 0, 0}},
                {{1, 1, 0}, {1, 1}, {0, 0, 1}, {0, 0, 0, 0}},
                {{-1, 1, 0}, {0, 1}, {0, 0, 1}, {0, 0, 0, 0}},
            }};
            const auto mesh = renderer.createMesh(tangentQuad, indices);
            const auto zeroTanMesh = renderer.createMesh(zeroTangentQuad, indices);

            const auto captureCenter = [&](const Mesh& m, const Material& mat, const glm::mat4& model,
                                           const Lighting& lighting) {
                std::vector<uint8_t> pixels;
                Detail::RendererReviewAccess::withFrame(
                    renderer,
                    FrameInput{{384, 256}, camera, std::array{DrawItem{m, mat, model}}, lighting},
                    [&](const auto& d, const auto& p, auto fs, auto nd) {
                        pixels = TextureGpuReview::captureDraws(d, p, fs, nd, {0, 0, 384, 256, 0, 1});
                    });
                return pixels;
            };

            // 1. 固定光照参数验证 Cook-Torrance GGX 与能量守恒漫反射参考值（0.05 + 0.384 = 0.434）。
            {
                MaterialDesc desc{baseWhite, {0.5f, 0.5f, 0.5f}};
                desc.shading = ShadingMode::Pbr;
                desc.metallic = 0.0f;
                desc.roughness = 0.5f;
                const auto mat = renderer.createMaterial(desc);
                const auto pixels = captureCenter(mesh, mat, glm::mat4(1.0f),
                                                  {{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.6f, 0.1f});
                expectLinearPixel(pixels, glm::vec3(0.434f));
            }

            // 2. +Y 切线空间法线贴图：正缩放受光更亮、单轴负缩放镜像模型反转副切线而变暗；
            //    normalScale = 0 或零切线安全退化为几何法线。
            {
                const Lighting angledLight{{0.0f, -0.8f, -0.6f}, {1.0f, 1.0f, 1.0f}, 0.8f, 0.1f};
                MaterialDesc flatDesc{baseWhite, {0.8f, 0.8f, 0.8f}};
                flatDesc.shading = ShadingMode::Pbr;
                flatDesc.metallic = 0.0f;
                flatDesc.roughness = 0.6f;
                const auto flatMat = renderer.createMaterial(flatDesc);

                MaterialDesc perturbedDesc = flatDesc;
                perturbedDesc.normalTexture = plusYNormal;
                perturbedDesc.normalScale = 1.0f;
                const auto perturbedMat = renderer.createMaterial(perturbedDesc);

                MaterialDesc zeroScaleDesc = perturbedDesc;
                zeroScaleDesc.normalScale = 0.0f;
                const auto zeroScaleMat = renderer.createMaterial(zeroScaleDesc);

                const auto flatPx = captureCenter(mesh, flatMat, glm::mat4(1.0f), angledLight);
                const auto perturbedPx = captureCenter(mesh, perturbedMat, glm::mat4(1.0f), angledLight);
                const auto mirroredPx = captureCenter(
                    mesh, perturbedMat, glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f)), angledLight);
                const auto zeroScalePx = captureCenter(mesh, zeroScaleMat, glm::mat4(1.0f), angledLight);
                const auto zeroTanPx = captureCenter(zeroTanMesh, perturbedMat, glm::mat4(1.0f), angledLight);

                const size_t centerOff = (128u * 384u + 192u) * 4u;
                EXPECT_GT(perturbedPx[centerOff], flatPx[centerOff] + 20);
                EXPECT_LT(mirroredPx[centerOff] + 40, flatPx[centerOff]);
                EXPECT_NEAR(zeroScalePx[centerOff], flatPx[centerOff], 1);
                EXPECT_NEAR(zeroTanPx[centerOff], flatPx[centerOff], 1);
            }

            // 3. 低粗糙度 vs 高粗糙度 GGX 高光峰值差异，以及纯金属环境光/漫反射衰减。
            {
                const Lighting frontLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.5f, 0.05f};
                MaterialDesc smoothDesc{baseWhite, {0.5f, 0.5f, 0.5f}};
                smoothDesc.shading = ShadingMode::Pbr;
                smoothDesc.metallic = 0.0f;
                smoothDesc.roughness = 0.1f;
                MaterialDesc roughDesc = smoothDesc;
                roughDesc.roughness = 0.9f;
                const auto smoothPx = captureCenter(mesh, renderer.createMaterial(smoothDesc), glm::mat4(1.0f), frontLight);
                const auto roughPx = captureCenter(mesh, renderer.createMaterial(roughDesc), glm::mat4(1.0f), frontLight);
                const size_t centerOff = (128u * 384u + 192u) * 4u;
                EXPECT_GT(smoothPx[centerOff], roughPx[centerOff] + 50);

                MaterialDesc metalDesc{baseWhite, {0.8f, 0.8f, 0.8f}};
                metalDesc.shading = ShadingMode::Pbr;
                metalDesc.metallic = 1.0f;
                metalDesc.roughness = 0.5f;
                const Lighting backLight{{0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, 0.3f};
                const auto metalBackPx = captureCenter(mesh, renderer.createMaterial(metalDesc), glm::mat4(1.0f), backLight);
                expectLinearPixel(metalBackPx, glm::vec3(0.0f));
            }
        }
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, IblFloatCubemapUploadAndPbrMaterialResponsesUnderValidation)
    {
        const auto buildDir = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path().parent_path();
        const auto buildAssetsDir = buildDir / "assets";
        const auto reviewDir = buildDir / "review";
        std::filesystem::create_directories(reviewDir);

        auto window = createWindow();
        SceneAssets assets;
        auto shadowHold = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
            Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
        ASSERT_TRUE(shadowHold);
        const Resource::ShaderResource shadowShader({}, shadowHold.get<Resource::ShaderResource>()->words());
        shadowHold.reset();

        const Resource::ShaderResource vertexShader({}, assets.vertex->words());
        const Resource::ShaderResource fragmentShader({}, assets.fragment->words());
        const Resource::ShaderResource displayVertexShader({}, assets.displayVertex->words());
        const Resource::ShaderResource displayFragmentShader({}, assets.displayFragment->words());
        const Resource::TextureResource refTex({}, assets.texture->width(), assets.texture->height(),
                                               assets.texture->pixels());
        releaseCpuAssets(assets);

        // 从构建部署根目录读取预生成的棚拍 IBL 资源与 Task 01 展示场景。
        auto io = FileIO::createPlatformFileIO();
        Resource::AssetSource buildSource(*io, buildAssetsDir);
        Resource::ResourceManager::instance().bindAssetSource(&buildSource);
        Resource::ResourceManager::instance().registerLoader(
            Resource::ResourceType::TEXTURE, std::make_unique<Resource::TextureLoader>(buildSource));
        Resource::ResourceManager::instance().registerLoader(
            Resource::ResourceType::MODEL, std::make_unique<Resource::ModelLoader>(buildSource));

        {
            Renderer renderer(*window, RendererValidationMode::Required);
            Texture irrTex, preTex, lutTex;
            {
                const auto irrHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_irradiance.ktx");
                const auto preHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_prefiltered.ktx");
                const auto lutHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_brdf_lut.ktx");
                ASSERT_TRUE(irrHold);
                ASSERT_TRUE(preHold);
                ASSERT_TRUE(lutHold);
                const auto* irrData = irrHold.get<Resource::FloatTextureResource>();
                const auto* preData = preHold.get<Resource::FloatTextureResource>();
                const auto* lutData = lutHold.get<Resource::FloatTextureResource>();
                ASSERT_NE(irrData, nullptr);
                ASSERT_NE(preData, nullptr);
                ASSERT_NE(lutData, nullptr);

                // 1. 准备顺序与非法 IBL 调用校验。
                {
                    auto scoped = renderer.createResources();
                    const auto irrScoped = renderer.createTexture(scoped, *irrData);
                    const auto preScoped = renderer.createTexture(scoped, *preData);
                    const auto lutScoped = renderer.createTexture(scoped, *lutData);
                    EXPECT_TRUE(irrScoped.valid() && preScoped.valid() && lutScoped.valid());
                    EXPECT_THROW(renderer.prepareIblResources(irrScoped, preScoped, lutScoped), Error::Exception);
                    scoped.reset();
                    EXPECT_FALSE(irrScoped.valid() || preScoped.valid() || lutScoped.valid());
                }

                renderer.prepareSceneResources(vertexShader, fragmentShader);
                renderer.prepareDisplayResources(displayVertexShader, displayFragmentShader);
                renderer.prepareShadowResources(shadowShader);

                irrTex = renderer.createTexture(*irrData);
                preTex = renderer.createTexture(*preData);
                lutTex = renderer.createTexture(*lutData);
            }

            const auto white = renderer.createTexture(
                Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}), TextureMipMode::BaseLevel,
                TextureColorSpace::Srgb);

            // 材质不得绑定浮点或立方体贴图；IBL 未准备前开启 iblEnabled 须抛出 RENDER；类型错配须拒绝。
            EXPECT_THROW(renderer.createMaterial({irrTex}), Error::Exception);
            EXPECT_THROW(renderer.createMaterial({lutTex}), Error::Exception);
            EXPECT_THROW(renderer.prepareIblResources(white, preTex, lutTex), Error::Exception);
            EXPECT_THROW(renderer.prepareIblResources(lutTex, preTex, lutTex), Error::Exception);
            EXPECT_THROW(renderer.prepareIblResources(irrTex, preTex, irrTex), Error::Exception);

            // 2. 构造球体网格用于光滑金属、粗糙金属与非金属响应检验。
            std::vector<MeshVertex> sphereVertices;
            std::vector<uint32_t> sphereIndices;
            constexpr uint32_t RINGS = 24;
            constexpr uint32_t SECTORS = 48;
            constexpr float PI = 3.14159265359f;
            for (uint32_t r = 0; r <= RINGS; ++r)
            {
                const float v = float(r) / RINGS;
                const float phi = v * PI;
                for (uint32_t s = 0; s <= SECTORS; ++s)
                {
                    const float u = float(s) / SECTORS;
                    const float theta = u * 2.0f * PI;
                    const glm::vec3 n{std::sin(phi) * std::sin(theta), std::cos(phi), std::sin(phi) * std::cos(theta)};
                    sphereVertices.push_back({{n.x * 0.5f, n.y * 0.5f, n.z * 0.5f},
                                              {u, v},
                                              {n.x, n.y, n.z},
                                              {std::cos(theta), 0.0f, -std::sin(theta), 1.0f}});
                }
            }
            for (uint32_t r = 0; r < RINGS; ++r)
            {
                for (uint32_t s = 0; s < SECTORS; ++s)
                {
                    const uint32_t a = r * (SECTORS + 1) + s;
                    const uint32_t b = a + SECTORS + 1;
                    sphereIndices.push_back(a);
                    sphereIndices.push_back(b);
                    sphereIndices.push_back(a + 1);
                    sphereIndices.push_back(a + 1);
                    sphereIndices.push_back(b);
                    sphereIndices.push_back(b + 1);
                }
            }
            const auto sphereMesh = renderer.createMesh(sphereVertices, sphereIndices);
            const auto quadData = Demo::makePlaneMesh();
            const auto quadMesh = renderer.createMesh(quadData.vertices, quadData.indices);

            MaterialDesc smoothMetalDesc{white, {0.95f, 0.95f, 0.95f}};
            smoothMetalDesc.shading = ShadingMode::Pbr;
            smoothMetalDesc.metallic = 1.0f;
            smoothMetalDesc.roughness = 0.05f;
            const auto smoothMetalMat = renderer.createMaterial(smoothMetalDesc);

            MaterialDesc roughMetalDesc{white, {0.95f, 0.95f, 0.95f}};
            roughMetalDesc.shading = ShadingMode::Pbr;
            roughMetalDesc.metallic = 1.0f;
            roughMetalDesc.roughness = 0.75f;
            const auto roughMetalMat = renderer.createMaterial(roughMetalDesc);

            MaterialDesc dielectricDesc{white, {0.82f, 0.28f, 0.18f}};
            dielectricDesc.shading = ShadingMode::Pbr;
            dielectricDesc.metallic = 0.0f;
            dielectricDesc.roughness = 0.85f;
            const auto dielectricMat = renderer.createMaterial(dielectricDesc);

            // IBL 关闭像素基线：安装 IBL 前后 iblEnabled=false 须输出完全一致的像素。
            const std::array<float, 4> blackClear{0.0f, 0.0f, 0.0f, 1.0f};
            auto proj = glm::perspectiveRH_ZO(glm::radians(38.0f), 900.0f / 320.0f, 0.1f, 20.0f);
            proj[1][1] *= -1.0f;
            const CameraMatrices sampleCamera{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 4.1f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)), proj};
            const std::array sampleDraws{
                DrawItem{sphereMesh, smoothMetalMat, glm::translate(glm::mat4(1.0f), glm::vec3(-1.35f, 0.0f, 0.0f))},
                DrawItem{sphereMesh, roughMetalMat, glm::mat4(1.0f)},
                DrawItem{sphereMesh, dielectricMat, glm::translate(glm::mat4(1.0f), glm::vec3(1.35f, 0.0f, 0.0f))},
            };

            const auto captureSampleHdr = [&](const Lighting& lighting, bool iblEnabled) {
                std::vector<std::array<float, 4>> pixels;
                FrameInput input{{900, 320}, sampleCamera, sampleDraws, lighting};
                input.iblEnabled = iblEnabled;
                Detail::RendererReviewAccess::withHdrSceneTarget(
                    renderer, input, blackClear, [&](const auto& d, const auto& img) {
                        pixels = TextureGpuReview::captureRgba16f(d, img, {900, 320});
                    });
                return pixels;
            };
            const auto captureOffscreenLdr = [&](const FrameInput& input, const std::array<float, 4>& clear) {
                std::vector<uint8_t> pixels;
                const auto uiExtent = renderer.beginUiFrame();
                EXPECT_TRUE(uiExtent);
                const auto output = renderer.prepareSceneTarget(input.extent, false);
                ImGui::Begin("IBL");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(input.extent.width), float(input.extent.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* drawData = renderer.imgui().drawData();
                renderer.drawSceneTarget(output, input, clear,
                                         [&](void* cmd) { renderer.imgui().recordDrawData(drawData, cmd); });
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& d, const auto& img) {
                        pixels = TextureGpuReview::captureMip(d, img, 0, {input.extent.width, input.extent.height});
                    });
                return pixels;
            };

            // 尚未准备 IBL 时开启 iblEnabled 必须抛 RENDER。
            {
                FrameInput badInput{{320, 180}, sampleCamera, sampleDraws};
                badInput.iblEnabled = true;
                EXPECT_THROW(renderer.drawFrame(badInput, blackClear), Error::Exception);
            }

            const Lighting directLighting{{0.0f, -0.5f, -1.0f}, {1.0f, 0.95f, 0.9f}, 1.5f, 0.12f};
            const auto beforeIblPrepareOff = captureSampleHdr(directLighting, false);

            // 安装共享 IBL 资源，验证不可重复安装且原关闭基线严格不变。
            renderer.prepareIblResources(irrTex, preTex, lutTex);
            EXPECT_THROW(renderer.prepareIblResources(irrTex, preTex, lutTex), Error::Exception);
            const auto afterIblPrepareOff = captureSampleHdr(directLighting, false);
            EXPECT_EQ(beforeIblPrepareOff, afterIblPrepareOff);

            // 3. 方向光强度为零且常量 ambient=0：检验光滑金属、粗糙金属与非金属响应差异。
            const Lighting zeroLighting{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.0f};
            FrameInput zeroOffInput{{900, 320}, sampleCamera, sampleDraws, zeroLighting};
            zeroOffInput.display = {ToneMappingMode::Reinhard, 0.0f};
            zeroOffInput.iblEnabled = false;
            const auto zeroOffPx = captureOffscreenLdr(zeroOffInput, {0.06f, 0.07f, 0.09f, 1.0f});

            FrameInput zeroOnInput = zeroOffInput;
            zeroOnInput.iblEnabled = true;
            const auto zeroOnPx = captureOffscreenLdr(zeroOnInput, {0.06f, 0.07f, 0.09f, 1.0f});
            EXPECT_NE(zeroOnPx, zeroOffPx);
            WindowCapture::save({900, 320, zeroOnPx}, reviewDir / "ibl-samples-zero-light.ppm");

            struct RegionMetrics
            {
                double meanLuma = 0.0;
                double meanR = 0.0;
                double meanG = 0.0;
                double meanB = 0.0;
                double meanGrad = 0.0;
            };
            const auto measureSphereRegion = [&](std::span<const uint8_t> rgba, uint32_t xStart, uint32_t xEnd) {
                RegionMetrics m{};
                size_t count = 0;
                size_t gradCount = 0;
                const auto pxAt = [&](uint32_t x, uint32_t y, int c) -> double {
                    return rgba[(size_t(y) * 900u + x) * 4u + c] / 255.0;
                };
                for (uint32_t y = 120; y < 200; ++y)
                {
                    for (uint32_t x = xStart; x < xEnd; ++x)
                    {
                        const double r = pxAt(x, y, 0);
                        const double g = pxAt(x, y, 1);
                        const double b = pxAt(x, y, 2);
                        m.meanR += r;
                        m.meanG += g;
                        m.meanB += b;
                        m.meanLuma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
                        ++count;
                        if (x + 1 < xEnd && y + 1 < 200)
                        {
                            const double l0 = 0.2126 * r + 0.7152 * g + 0.0722 * b;
                            const double lx = 0.2126 * pxAt(x + 1, y, 0) + 0.7152 * pxAt(x + 1, y, 1) +
                                              0.0722 * pxAt(x + 1, y, 2);
                            const double ly = 0.2126 * pxAt(x, y + 1, 0) + 0.7152 * pxAt(x, y + 1, 1) +
                                              0.0722 * pxAt(x, y + 1, 2);
                            m.meanGrad += std::abs(lx - l0) + std::abs(ly - l0);
                            ++gradCount;
                        }
                    }
                }
                m.meanR /= count;
                m.meanG /= count;
                m.meanB /= count;
                m.meanLuma /= count;
                m.meanGrad /= gradCount;
                return m;
            };
            const auto smoothM = measureSphereRegion(zeroOnPx, 257, 337);
            const auto roughM = measureSphereRegion(zeroOnPx, 410, 490);
            const auto dielM = measureSphereRegion(zeroOnPx, 563, 643);
            EXPECT_GT(smoothM.meanLuma, 0.20);
            EXPECT_GT(roughM.meanLuma, 0.20);
            EXPECT_LT(roughM.meanGrad, smoothM.meanGrad * 0.65);
            EXPECT_GT(dielM.meanR, 0.50);
            EXPECT_GT(dielM.meanR, dielM.meanG * 1.25);
            EXPECT_GT(dielM.meanG, dielM.meanB * 1.05);

            // 4. 开启方向光与阴影时，IBL 之上仍保留直接光与 PCF 阴影衰减。
            const auto orthoProj = [] {
                auto p = glm::orthoRH_ZO(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 10.0f);
                p[1][1] *= -1.0f;
                return p;
            }();
            const CameraMatrices shadowCam{
                glm::lookAtRH(glm::vec3(0.0f, 1.5f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)),
                orthoProj};
            const std::array<MeshVertex, 4> receiverVerts{{
                {{-2.0f, 0.0f, -2.0f}, {0, 0}, {0, 1, 0}, {1, 0, 0, 1}},
                {{2.0f, 0.0f, -2.0f}, {1, 0}, {0, 1, 0}, {1, 0, 0, 1}},
                {{2.0f, 0.0f, 2.0f}, {1, 1}, {0, 1, 0}, {1, 0, 0, 1}},
                {{-2.0f, 0.0f, 2.0f}, {0, 1}, {0, 1, 0}, {1, 0, 0, 1}},
            }};
            const std::array<uint32_t, 6> receiverIndices{0, 2, 1, 0, 3, 2};
            const auto xzQuadMesh = renderer.createMesh(receiverVerts, receiverIndices);
            MaterialDesc receiverDesc{white, {0.8f, 0.8f, 0.8f}};
            receiverDesc.shading = ShadingMode::Pbr;
            receiverDesc.metallic = 0.0f;
            receiverDesc.roughness = 0.8f;
            const auto matteReceiver = renderer.createMaterial(receiverDesc);
            const std::array shadowDraws{
                DrawItem{xzQuadMesh, matteReceiver, glm::mat4(1.0f)},
                DrawItem{xzQuadMesh, matteReceiver,
                         glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.8f, 0.0f)) *
                             glm::scale(glm::mat4(1.0f), glm::vec3(0.225f, 1.0f, 0.225f))},
            };
            ShadowSettings shadowSettings;
            shadowSettings.enabled = true;
            shadowSettings.depthBias = 0.002f;
            const Lighting topLight{{0.0f, -1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 2.0f, 0.0f};
            FrameInput iblShadowInput{{384, 256}, shadowCam, shadowDraws, topLight};
            iblShadowInput.shadow = makeShadowFrame(shadowSettings, topLight.direction);
            iblShadowInput.shadowDraws = shadowDraws;
            iblShadowInput.display = {ToneMappingMode::Reinhard, 0.0f};
            iblShadowInput.iblEnabled = true;
            const auto shadowIblPx = captureOffscreenLdr(iblShadowInput, blackClear);
            const auto centerShadowed = shadowIblPx[(128u * 384u + 192u) * 4u];
            const auto sideLit = shadowIblPx[(128u * 384u + 80u) * 4u];
            EXPECT_GT(sideLit, centerShadowed + 20u);

            // 5. 在 Task 01 场景 pbr-showcase.json 上执行同机位 IBL OFF / ON 对照并记录 GPU 耗时。
            const auto showcaseDesc = Scene::readScene(*io, Utils::Utf8::toUtf8String(buildAssetsDir / "scenes/pbr-showcase.json"));
            Model::ModelManager modelManager(renderer);
            const auto rebuilt = Scene::rebuildScene(
                showcaseDesc, [&](const Model::ModelReference& ref) { return modelManager.resolve(ref); });
            constexpr FrameExtent SHOWCASE_EXTENT{800, 600};
            ShadowSettings showcaseShadow;
            showcaseShadow.enabled = true;
            showcaseShadow.depthBias = 0.003f;
            const auto extractedOff =
                Scene::extractSceneFrame(*rebuilt.scene, SHOWCASE_EXTENT, true, showcaseShadow, false);
            const auto extractedOn =
                Scene::extractSceneFrame(*rebuilt.scene, SHOWCASE_EXTENT, true, showcaseShadow, true);
            ASSERT_TRUE(extractedOff.has_value());
            ASSERT_TRUE(extractedOn.has_value());

            constexpr int WARMUP_FRAMES = 4;
            constexpr int SAMPLE_FRAMES = 11;
            std::ostringstream report;
            report << "Fixed Studio IBL Comparison (pbr-showcase.json, 800x600, Debug, Validation=Required)\n";
            report << "ExposureEv=" << showcaseDesc.display.exposureEv << ", ToneMapping=Reinhard, Shadows=ON(bias="
                   << showcaseShadow.depthBias << "), FXAA=OFF\n";
            report << "Warmup=" << WARMUP_FRAMES << ", Samples=" << SAMPLE_FRAMES << "\n";

            std::vector<uint8_t> showcaseOffPx, showcaseOnPx;
            for (const bool enableIbl : {false, true})
            {
                const auto& frameSrc = enableIbl ? *extractedOn : *extractedOff;
                for (int i = 0; i < WARMUP_FRAMES; ++i)
                    (void)captureOffscreenLdr(frameSrc.input(), showcaseDesc.clearColor);
                Detail::RendererReviewAccess::waitForFrames(renderer);

                std::vector<double> gpuTimes;
                uint32_t sceneDraws = 0;
                uint32_t shadowDrawsCount = 0;
                std::vector<uint8_t> lastPx;
                for (int i = 0; i < SAMPLE_FRAMES; ++i)
                {
                    lastPx = captureOffscreenLdr(frameSrc.input(), showcaseDesc.clearColor);
                    Detail::RendererReviewAccess::waitForFrames(renderer);
                    const auto stats = renderer.lastViewportStats();
                    EXPECT_TRUE(stats.hasCompletedSample);
                    sceneDraws = stats.sceneDrawCount;
                    shadowDrawsCount = stats.shadowDrawCount;
                    if (stats.gpuTimeMs.has_value())
                        gpuTimes.push_back(*stats.gpuTimeMs);
                }
                double medianMs = 0.0;
                if (!gpuTimes.empty())
                {
                    std::sort(gpuTimes.begin(), gpuTimes.end());
                    medianMs = gpuTimes[gpuTimes.size() / 2];
                }
                const char* ppmName = enableIbl ? "ibl-showcase-on.ppm" : "ibl-showcase-off.ppm";
                WindowCapture::save({int(SHOWCASE_EXTENT.width), int(SHOWCASE_EXTENT.height), lastPx},
                                    reviewDir / ppmName);
                if (enableIbl)
                    showcaseOnPx = std::move(lastPx);
                else
                    showcaseOffPx = std::move(lastPx);
                report << (enableIbl ? "IblOn" : "IblOff") << ": medianGpuMs=" << medianMs
                       << ", sceneDraws=" << sceneDraws << ", shadowDraws=" << shadowDrawsCount
                       << ", ppm=" << ppmName << "\n";
            }
            EXPECT_NE(showcaseOnPx, showcaseOffPx);
            std::ofstream reportOut(reviewDir / "ibl-comparison-report.txt");
            reportOut << report.str();
            std::cout << report.str() << std::endl;
        }
        Resource::ResourceManager::instance().reset();
        Resource::ResourceManager::instance().bindAssetSource(nullptr);
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, MaterialParameterModesFixedOverrideSharedModelIsolationAndZeroTextureReuploadUnderValidation)
    {
        const auto buildDir = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR).parent_path().parent_path();
        const auto buildAssetsDir = buildDir / "assets";
        const auto reviewDir = buildDir / "review";
        std::filesystem::create_directories(reviewDir);

        auto window = createWindow();
        SceneAssets assets;
        const Resource::ShaderResource vertexShader({}, assets.vertex->words());
        const Resource::ShaderResource fragmentShader({}, assets.fragment->words());
        const Resource::ShaderResource displayVertexShader({}, assets.displayVertex->words());
        const Resource::ShaderResource displayFragmentShader({}, assets.displayFragment->words());
        releaseCpuAssets(assets);

        auto io = FileIO::createPlatformFileIO();
        Resource::AssetSource buildSource(*io, buildAssetsDir);
        Resource::ResourceManager::instance().bindAssetSource(&buildSource);
        Resource::ResourceManager::instance().registerLoader(
            Resource::ResourceType::TEXTURE, std::make_unique<Resource::TextureLoader>(buildSource));
        Resource::ResourceManager::instance().registerLoader(
            Resource::ResourceType::MODEL, std::make_unique<Resource::ModelLoader>(buildSource));

        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(vertexShader, fragmentShader);
            renderer.prepareDisplayResources(displayVertexShader, displayFragmentShader);
            {
                const auto irrHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_irradiance.ktx");
                const auto preHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_prefiltered.ktx");
                const auto lutHold = Resource::ResourceManager::instance().hold<Resource::FloatTextureResource>(
                    Resource::ResourceType::TEXTURE, "ibl/studio_brdf_lut.ktx");
                ASSERT_TRUE(irrHold && preHold && lutHold);
                renderer.prepareIblResources(renderer.createTexture(*irrHold.get<Resource::FloatTextureResource>()),
                                             renderer.createTexture(*preHold.get<Resource::FloatTextureResource>()),
                                             renderer.createTexture(*lutHold.get<Resource::FloatTextureResource>()));
            }

            // 1. 验证 MaterialParameterMode 非法枚举拒绝，以及固定粗糙度/金属度替换贴图通道。
            const auto white = renderer.createTexture(
                Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}), TextureMipMode::BaseLevel,
                TextureColorSpace::Srgb);
            // 贴图 G=230 (~0.902 粗糙), B=0 (0.0 金属)。
            const auto roughNonMetalTex = renderer.createTexture(
                Resource::TextureResource({}, 1, 1, {0, 230, 0, 255}), TextureMipMode::BaseLevel,
                TextureColorSpace::Linear);
            {
                MaterialDesc invalidDesc{white};
                invalidDesc.shading = ShadingMode::Pbr;
                invalidDesc.roughnessMode = static_cast<MaterialParameterMode>(7);
                EXPECT_THROW(renderer.createMaterial(invalidDesc), Error::Exception);
                invalidDesc.roughnessMode = MaterialParameterMode::TextureFactor;
                invalidDesc.metallicMode = static_cast<MaterialParameterMode>(7);
                EXPECT_THROW(renderer.createMaterial(invalidDesc), Error::Exception);
            }

            MaterialDesc texFactorDesc{white};
            texFactorDesc.shading = ShadingMode::Pbr;
            texFactorDesc.metallicRoughnessTexture = roughNonMetalTex;
            texFactorDesc.roughness = 1.0f;
            texFactorDesc.metallic = 1.0f;
            const auto matTexFactor = renderer.createMaterial(texFactorDesc);

            MaterialDesc fixedOverrideDesc = texFactorDesc;
            fixedOverrideDesc.roughnessMode = MaterialParameterMode::FixedValue;
            fixedOverrideDesc.roughness = 0.05f;
            fixedOverrideDesc.metallicMode = MaterialParameterMode::FixedValue;
            fixedOverrideDesc.metallic = 1.0f;
            const auto matFixedMetal = renderer.createMaterial(fixedOverrideDesc);

            const auto quadData = Demo::makePlaneMesh();
            const std::array<MeshVertex, 4> tangentQuad{{
                {{-1, -1, 0}, {0, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                {{1, -1, 0}, {1, 0}, {0, 0, 1}, {1, 0, 0, 1}},
                {{1, 1, 0}, {1, 1}, {0, 0, 1}, {1, 0, 0, 1}},
                {{-1, 1, 0}, {0, 1}, {0, 0, 1}, {1, 0, 0, 1}},
            }};
            const std::array<uint32_t, 6> quadIndices{0, 1, 2, 0, 2, 3};
            const auto tangentMesh = renderer.createMesh(tangentQuad, quadIndices);

            auto ortho = glm::orthoRH_ZO(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
            ortho[1][1] *= -1.0f;
            const CameraMatrices cam{
                glm::lookAtRH(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)), ortho};
            const auto captureHdrCenter = [&](const Material& mat, const Lighting& l, bool ibl) {
                std::vector<std::array<float, 4>> hdr;
                FrameInput input{{64, 64}, cam, std::array{DrawItem{tangentMesh, mat, glm::mat4(1.0f)}}, l};
                input.iblEnabled = ibl;
                Detail::RendererReviewAccess::withHdrSceneTarget(
                    renderer, input, {0, 0, 0, 1}, [&](const auto& d, const auto& img) {
                        hdr = TextureGpuReview::captureRgba16f(d, img, {64, 64});
                    });
                return hdr[(32u * 64u + 32u)];
            };
            // 正向光源与视线对准半向量 H=(0,0,1)：高粗糙度(0.9)高光极弱，固定粗糙度(0.05) + 固定金属度(1.0)产生强高光。
            const Lighting frontLight{{0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, 1.0f, 0.0f};
            const auto factorPx = captureHdrCenter(matTexFactor, frontLight, false);
            const auto fixedPx = captureHdrCenter(matFixedMetal, frontLight, false);
            EXPECT_GT(fixedPx[0], factorPx[0] * 10.0f);

            // 2. 加载共享 Avocado 模型并建立双实体，验证 PreparedModel 槽位映射、隔离、零贴图重传及 Fence 回收。
            Model::PrepareOptions pbrOpts;
            pbrOpts.mipMode = TextureMipMode::FullChain;
            pbrOpts.sampler = {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear};
            pbrOpts.shading = ShadingMode::Pbr;
            const auto avocado = Model::prepareModel(renderer, "models/avocado/gltf/avocado.gltf", pbrOpts);
            ASSERT_EQ(avocado->materialSlots().size(), 1u);
            EXPECT_EQ(avocado->materialSlots()[0].sourceMaterialIndex, 0u);
            ASSERT_EQ(avocado->drawMaterialSlots().size(), avocado->draws().size());
            EXPECT_EQ(avocado->drawMaterialSlots()[0], 0u);

            const auto countsBeforeOverride = Detail::RendererReviewAccess::resourceCounts(renderer);
            const std::string logsBefore = messages_.str();
            const size_t uploadsBefore = TextureGpuReview::countSceneTextureUploads(logsBefore);

            Scene::Scene scene;
            scene.setCamera(Scene::Camera({0.0f, 0.0f, 2.6f}));
            scene.setLighting({{0.5f, -0.5f, -0.7f}, {1.0f, 0.96f, 0.9f}, 1.2f, 0.12f});
            const auto leftId =
                scene.createEntity(avocado, Scene::Transform({-0.65f, -0.75f, 0.0f}, {1, 0, 0, 0}, glm::vec3(24.0f)),
                                   true, "Left");
            const auto rightId =
                scene.createEntity(avocado, Scene::Transform({0.65f, -0.75f, 0.0f}, {1, 0, 0, 0}, glm::vec3(24.0f)),
                                   true, "Right");

            const auto renderSceneLdr = [&](FrameExtent ext) {
                std::vector<uint8_t> pixels;
                const auto extracted = Scene::extractSceneFrame(scene, ext, true, {}, true);
                EXPECT_TRUE(extracted.has_value());
                const auto uiExt = renderer.beginUiFrame();
                EXPECT_TRUE(uiExt);
                const auto output = renderer.prepareSceneTarget(ext, false);
                ImGui::Begin("OverrideReview");
                ImGui::Image(static_cast<ImTextureID>(renderer.sceneImage(output)),
                             ImVec2(float(ext.width), float(ext.height)));
                ImGui::End();
                renderer.imgui().endFrame();
                auto* dd = renderer.imgui().drawData();
                renderer.drawSceneTarget(output, extracted->input(), {0.08f, 0.095f, 0.125f, 1.0f},
                                         [&](void* cmd) { renderer.imgui().recordDrawData(dd, cmd); });
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& d, const auto& img) {
                        pixels = TextureGpuReview::captureMip(d, img, 0, {ext.width, ext.height});
                    });
                return pixels;
            };

            const auto basePx = renderSceneLdr({640, 480});

            // 仅对左侧实体应用覆盖（红色染色 + 固定光滑金属），右侧实体保持原样。
            Scene::MaterialOverride ov{
                0u,
                {1.0f, 0.25f, 0.2f},
                MaterialParameterMode::FixedValue,
                0.12f,
                MaterialParameterMode::FixedValue,
                1.0f,
            };
            auto compiled = Model::prepareMaterialOverride(renderer, *avocado, ov.materialSlot, ov.color,
                                                           ov.roughnessMode, ov.roughness, ov.metallicMode, ov.metallic);
            ASSERT_TRUE(scene.setMaterialOverride(leftId, ov, compiled));
            compiled.reset();

            // 验证零纹理/网格重传，只新增 1 份独立材质绑定。
            const auto countsAfterOverride = Detail::RendererReviewAccess::resourceCounts(renderer);
            EXPECT_EQ(countsAfterOverride.meshes, countsBeforeOverride.meshes);
            EXPECT_EQ(countsAfterOverride.textures, countsBeforeOverride.textures);
            EXPECT_EQ(countsAfterOverride.materials, countsBeforeOverride.materials + 1u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), uploadsBefore);

            const auto overriddenPx = renderSceneLdr({640, 480});
            WindowCapture::save({640, 480, overriddenPx}, reviewDir / "material-override-isolation.ppm");

            // 验证左半屏变化且右半屏（共享同模型的右侧实体）逐像素严格不变。
            bool leftChanged = false;
            for (uint32_t y = 0; y < 480; ++y)
            {
                for (uint32_t x = 0; x < 310; ++x)
                {
                    const size_t off = (size_t(y) * 640u + x) * 4u;
                    if (overriddenPx[off] != basePx[off] || overriddenPx[off + 1] != basePx[off + 1] ||
                        overriddenPx[off + 2] != basePx[off + 2])
                    {
                        leftChanged = true;
                        break;
                    }
                }
                if (leftChanged)
                    break;
            }
            EXPECT_TRUE(leftChanged);
            for (uint32_t y = 0; y < 480; ++y)
            {
                for (uint32_t x = 330; x < 640; ++x)
                {
                    const size_t off = (size_t(y) * 640u + x) * 4u;
                    ASSERT_EQ(overriddenPx[off + 0], basePx[off + 0]);
                    ASSERT_EQ(overriddenPx[off + 1], basePx[off + 1]);
                    ASSERT_EQ(overriddenPx[off + 2], basePx[off + 2]);
                }
            }

            // 连续替换覆盖并重置，验证在途帧 Fence 完成前安全且最终回收独立覆盖材质资源。
            for (int k = 1; k <= 3; ++k)
            {
                ov.roughness = 0.1f * float(k);
                auto nextCompiled =
                    Model::prepareMaterialOverride(renderer, *avocado, ov.materialSlot, ov.color, ov.roughnessMode,
                                                   ov.roughness, ov.metallicMode, ov.metallic);
                ASSERT_TRUE(scene.setMaterialOverride(leftId, ov, std::move(nextCompiled)));
                (void)renderSceneLdr({640, 480});
            }
            ASSERT_TRUE(scene.clearMaterialOverride(leftId, 0u));
            const auto restoredPx = renderSceneLdr({640, 480});
            EXPECT_EQ(restoredPx, basePx);
            Detail::RendererReviewAccess::waitForFrames(renderer);
            EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).materials, countsBeforeOverride.materials);

            // 验证 Scene 1.2 捕获、序列化与 rebuildScene GPU 重建恢复。
            ASSERT_TRUE(scene.setMaterialOverride(
                leftId, ov,
                Model::prepareMaterialOverride(renderer, *avocado, ov.materialSlot, ov.color, ov.roughnessMode,
                                               ov.roughness, ov.metallicMode, ov.metallic)));
            const auto desc12 = Scene::captureScene(scene, {0.08f, 0.095f, 0.125f, 1.0f});
            const auto decoded12 = Scene::decodeScene(Scene::encodeScene(desc12));
            const auto rebuilt12 = Scene::rebuildScene(
                decoded12, [&](const Model::ModelReference&) { return avocado; },
                [&](const Model::PreparedModel& model, const Scene::MaterialOverride& item) {
                    return Model::prepareMaterialOverride(renderer, model, item.materialSlot, item.color,
                                                          item.roughnessMode, item.roughness, item.metallicMode,
                                                          item.metallic);
                });
            ASSERT_EQ(rebuilt12.scene->entities().size(), 2u);
            EXPECT_EQ(rebuilt12.scene->entities()[0].materialOverrides.size(), 1u);
            EXPECT_TRUE(rebuilt12.scene->entities()[1].materialOverrides.empty());
            ASSERT_TRUE(rebuilt12.scene->entities()[0].materialOverrides[0].compiled != nullptr);
            EXPECT_TRUE(rebuilt12.scene->entities()[0].materialOverrides[0].compiled->material.valid());
            EXPECT_TRUE(rebuilt12.scene->find(rebuilt12.scene->entities()[1].id)->materialOverrides.empty());
            EXPECT_EQ(scene.find(rightId)->materialOverrides.size(), 0u);
        }
        Resource::ResourceManager::instance().reset();
        Resource::ResourceManager::instance().bindAssetSource(nullptr);
        expectCleanValidation();
    }

    TEST_F(VulkanShadingTest, LocalPointAndSpotLightsPbrAndLambertAttenuationUnderValidation)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);

            const auto white = renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255}));
            const auto plane = createLitPlane(renderer);
            const auto largePlane = DrawItem{plane, Material{}, glm::scale(glm::mat4(1.0f), {8.0f, 6.0f, 1.0f})};

            MaterialDesc lambertDesc{white, {1.0f, 1.0f, 1.0f}};
            lambertDesc.shading = ShadingMode::Lambert;
            const auto lambertMat = renderer.createMaterial(lambertDesc);

            MaterialDesc pbrDesc{white, {0.8f, 0.8f, 0.8f}};
            pbrDesc.shading = ShadingMode::Pbr;
            pbrDesc.roughnessMode = MaterialParameterMode::FixedValue;
            pbrDesc.roughness = 0.55f;
            pbrDesc.metallicMode = MaterialParameterMode::FixedValue;
            pbrDesc.metallic = 0.1f;
            const auto pbrMat = renderer.createMaterial(pbrDesc);

            MaterialDesc unlitDesc{white, {0.2f, 0.3f, 0.4f}};
            unlitDesc.shading = ShadingMode::Unlit;
            const auto unlitMat = renderer.createMaterial(unlitDesc);

            const FrameExtent ext{160, 120};
            // 世界范围 [-4, 4] x [-3, 3] 精确一一映射至 160x120（每单位 20 像素，中心 x=80, y=60）。
            const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
            glm::mat4 proj = glm::orthoRH_ZO(-4.0f, 4.0f, -3.0f, 3.0f, 0.1f, 20.0f);
            proj[1][1] *= -1.0f;
            const CameraMatrices cam{view, proj};

            const auto sampleWorld = [&](const std::vector<std::array<float, 4>>& hdr, float wx, float wy) {
                const uint32_t px = std::min(ext.width - 1u, static_cast<uint32_t>(std::lround((wx + 4.0f) * 20.0f)));
                const uint32_t py = std::min(ext.height - 1u, static_cast<uint32_t>(std::lround((3.0f - wy) * 20.0f)));
                return hdr[size_t(py) * ext.width + px];
            };
            const auto renderMatHdr = [&](const Material& mat, const Lighting& lighting) {
                const std::array draws{DrawItem{largePlane.mesh, mat, largePlane.model}};
                return captureHdr(renderer, FrameInput{ext, cam, draws, lighting});
            };

            Lighting baseLight;
            baseLight.intensity = 0.0f;
            baseLight.ambient = 0.0f;

            // 1. 基线：关闭方向光、环境光与局部光源时，Lambert 与 PBR 均为纯黑。
            EXPECT_FLOAT_EQ(sampleWorld(renderMatHdr(lambertMat, baseLight), 0.0f, 0.0f)[0], 0.0f);
            EXPECT_FLOAT_EQ(sampleWorld(renderMatHdr(pbrMat, baseLight), 0.0f, 0.0f)[0], 0.0f);

            // 2. 点光源距离衰减：中心 (dist=1.5) 符合 KHR_lights_punctual 解析解，近亮远暗，超 range(3.0) 为零。
            LocalLight point;
            point.type = LocalLightType::Point;
            point.position = {0.0f, 0.0f, 1.5f};
            point.color = {1.0f, 0.5f, 0.25f};
            point.intensity = 8.0f;
            point.range = 3.0f;
            Lighting pointLighting = baseLight;
            pointLighting.localLights = {point};

            const float ratioCenter = 1.5f / 3.0f;
            const float winCenter = 1.0f - std::pow(ratioCenter, 4.0f);
            const float expectedCenterAtten = (winCenter * winCenter) / (1.5f * 1.5f);
            const float expectedCenterRed = point.color.r * point.intensity * expectedCenterAtten;

            for (const auto& [mat, isLambert] : {std::pair{lambertMat, true}, std::pair{pbrMat, false}})
            {
                const auto hdr = renderMatHdr(mat, pointLighting);
                const auto cCenter = sampleWorld(hdr, 0.0f, 0.0f);
                const auto cMid = sampleWorld(hdr, 1.5f, 0.0f);
                const auto cOut = sampleWorld(hdr, 3.2f, 0.0f);
                EXPECT_GT(cCenter[0], cMid[0] * 3.0f);
                EXPECT_GT(cMid[0], 0.01f);
                EXPECT_FLOAT_EQ(cOut[0], 0.0f);
                EXPECT_FLOAT_EQ(cOut[1], 0.0f);
                EXPECT_FLOAT_EQ(cOut[2], 0.0f);
                EXPECT_NEAR(cCenter[1] / cCenter[0], 0.5f, 0.02f);
                if (isLambert)
                    EXPECT_NEAR(cCenter[0], expectedCenterRed, 0.05f);
            }

            // 3. 聚光灯内外锥角平滑过渡：内锥满照度，过渡带平滑衰减，外锥之外严格归零。
            LocalLight spot;
            spot.type = LocalLightType::Spot;
            spot.position = {0.0f, 0.0f, 2.0f};
            spot.direction = {0.0f, 0.0f, -1.0f};
            spot.color = {0.3f, 0.9f, 1.0f};
            spot.intensity = 10.0f;
            spot.range = 10.0f;
            spot.innerConeAngle = 0.25f;  // r ~= 0.51
            spot.outerConeAngle = 0.65f;  // r ~= 1.52
            Lighting spotLighting = baseLight;
            spotLighting.localLights = {spot};

            // 对比同位置同参数的 Point 光源，验证内锥以内与过渡带的 spot 因子。
            LocalLight spotAsPoint = spot;
            spotAsPoint.type = LocalLightType::Point;
            Lighting spotAsPointLighting = baseLight;
            spotAsPointLighting.localLights = {spotAsPoint};

            for (const auto& mat : {lambertMat, pbrMat})
            {
                const auto hdrSpot = renderMatHdr(mat, spotLighting);
                const auto hdrRefPoint = renderMatHdr(mat, spotAsPointLighting);
                const auto cInnerSpot = sampleWorld(hdrSpot, 0.0f, 0.0f);
                const auto cInnerRef = sampleWorld(hdrRefPoint, 0.0f, 0.0f);
                EXPECT_NEAR(cInnerSpot[1], cInnerRef[1], 1e-3f);

                const auto cMidSpot = sampleWorld(hdrSpot, 0.9f, 0.0f);
                const auto cMidRef = sampleWorld(hdrRefPoint, 0.9f, 0.0f);
                EXPECT_GT(cMidSpot[1], 0.05f);
                EXPECT_LT(cMidSpot[1], cMidRef[1] * 0.85f);

                const auto cOutsideSpot = sampleWorld(hdrSpot, 2.0f, 0.0f);
                const auto cOutsideRef = sampleWorld(hdrRefPoint, 2.0f, 0.0f);
                EXPECT_GT(cOutsideRef[1], 0.05f);
                EXPECT_FLOAT_EQ(cOutsideSpot[1], 0.0f);
            }

            // 4. 多光源线性累加 + disabled 光源跳过 + Unlit 不受局部光源影响。
            LocalLight disabledGreen = point;
            disabledGreen.enabled = false;
            disabledGreen.color = {0.0f, 1.0f, 0.0f};
            disabledGreen.intensity = 50.0f;

            LocalLight redPoint = point;
            redPoint.color = {1.0f, 0.0f, 0.0f};
            LocalLight blueSpot = spot;
            blueSpot.color = {0.0f, 0.0f, 1.0f};

            Lighting combinedLighting = baseLight;
            combinedLighting.localLights = {redPoint, disabledGreen, blueSpot};
            Lighting redOnly = baseLight;
            redOnly.localLights = {redPoint};
            Lighting blueOnly = baseLight;
            blueOnly.localLights = {blueSpot};

            for (const auto& mat : {lambertMat, pbrMat})
            {
                const auto hdrComb = renderMatHdr(mat, combinedLighting);
                const auto hdrRed = renderMatHdr(mat, redOnly);
                const auto hdrBlue = renderMatHdr(mat, blueOnly);
                const auto cComb = sampleWorld(hdrComb, 0.0f, 0.0f);
                const auto cRed = sampleWorld(hdrRed, 0.0f, 0.0f);
                const auto cBlue = sampleWorld(hdrBlue, 0.0f, 0.0f);
                EXPECT_NEAR(cComb[0], cRed[0] + cBlue[0], 0.02f);
                EXPECT_FLOAT_EQ(cComb[1], 0.0f);  // disabled 绿灯未生效
                EXPECT_NEAR(cComb[2], cRed[2] + cBlue[2], 0.02f);
            }

            const auto hdrUnlit = renderMatHdr(unlitMat, combinedLighting);
            const auto cUnlit = sampleWorld(hdrUnlit, 0.0f, 0.0f);
            EXPECT_NEAR(cUnlit[0], 0.2f, 0.01f);
            EXPECT_NEAR(cUnlit[1], 0.3f, 0.01f);
            EXPECT_NEAR(cUnlit[2], 0.4f, 0.01f);
        }
        expectCleanValidation();
    }
}  // namespace
