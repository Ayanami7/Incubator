#include "model_preparation_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Render;
    using namespace Incubator::Test;
    namespace fs = std::filesystem;

    class ModelSceneIntegrationTest : public ModelPreparationTest
    {};


    TEST_F(ModelSceneIntegrationTest, ModelShadowSamplingBoundsBiasAndPcfForLambertAndPbr)
    {
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            Model::PrepareOptions options;
            options.shading = ShadingMode::Lambert;
            const auto receiver = Model::prepareModel(renderer, "models/shadows/receiver.obj", options);
            const auto caster = Model::prepareModel(renderer, "models/shadows/caster.obj");
            EXPECT_EQ(caster->source().options().shading, ShadingMode::Unlit);
            ASSERT_FALSE(receiver->textures().empty());
            const auto unlit = renderer.createMaterial(MaterialDesc{receiver->textures()[0]});
            Scene::Scene scene;
            scene.createEntity(receiver);
            const auto casterId = scene.createEntity(caster, Scene::Transform({0, 0.75f, 5}));
            ShadowSettings settings;
            std::vector<uint8_t> baseline;
            for (int i = 0; i < 13; ++i)
            {
                SCOPED_TRACE(i);
                if (i == 1)
                    shadowShader(renderer);
                scene.setCamera(shadowReviewCamera(i == 10 ? glm::vec3(0.25f, -3, 3) : glm::vec3(0, -3, 3)));
                Lighting lighting{{0, 0, -1}, {0.6f, 0.7f, 0.8f}, 0.8f, 0.2f};
                if (i == 6)
                    lighting.direction = {0, -1, -1};
                if (i == 8 || i == 9)
                    lighting.intensity = 0;
                if (i == 9)
                    lighting.ambient = 0;
                scene.setLighting(lighting);
                scene.setTransform(casterId, Scene::Transform({i == 5 ? 3.0f : 0.0f, 0.75f, 5}));
                settings.enabled = i != 0 && i != 3 && i != 12;
                auto frame = Scene::extractSceneFrame(scene, {384, 256}, true, settings);
                ASSERT_TRUE(frame);
                // 投射物完全在主相机外，但它仍应遮挡画面内的接收面。
                ASSERT_EQ(frame->draws.size(), receiver->draws().size());
                if (frame->shadow)
                    EXPECT_GT(frame->shadowDraws.size(), frame->draws.size());
                if (i == 2 || i == 11)
                    frame->shadowDraws.clear();
                if (i == 7)
                    for (auto& draw : frame->draws)
                        draw.material = unlit;
                const auto pixels = shadowScenePixels(*native, renderer, *frame);
                const auto diffuse = std::max(glm::normalize(-lighting.direction).z, 0.0f);
                const auto lit = glm::vec3(lighting.ambient) + lighting.color * lighting.intensity * diffuse;
                const bool occluded = i == 1 || i == 4 || i == 10;
                expectShadowPixel(*frame, pixels, {0, 0.75f, 0},
                                  i == 7     ? glm::vec3(1)
                                  : occluded ? glm::vec3(lighting.ambient)
                                             : lit);
                expectShadowPixel(*frame, pixels, {0, -0.75f, 0}, i == 7 ? glm::vec3(1) : lit);
                if (i == 0)
                {
                    baseline = pixels;
                    const auto direct = TextureGpuReview::captureFrame(renderer, frame->input());
                    expectShadowPixel(*frame, direct, {0, 0.75f, 0}, lit);
                }
                if (i == 12)
                    EXPECT_EQ(pixels, baseline);
                EXPECT_NEAR(pixels[0], 31, 1);
                EXPECT_NEAR(pixels[1], 41, 1);
                EXPECT_NEAR(pixels[2], 56, 1);
            }

            // 同会话验证光源视锥边界外点亮语义与接收面 depthBias 门限。
            {
                Scene::Scene biasScene;
                biasScene.createEntity(receiver);
                biasScene.setCamera(shadowReviewCamera());
                const Lighting lighting{{0, 0, -1}, {0.6f, 0.7f, 0.8f}, 0.8f, 0.2f};
                biasScene.setLighting(lighting);
                struct Sample
                {
                    glm::vec3 center;
                    glm::vec2 halfExtent;
                    float nearPlane, farPlane, casterZ, bias;
                    bool occluded;
                };
                const std::array<Sample, 9> samples{{{{-4, 0.75f, 0}, {1, 1}, 1, 9, 2, 0.001f, false},
                                                     {{4, 0.75f, 0}, {1, 1}, 1, 9, 2, 0.001f, false},
                                                     {{0, 4.75f, 0}, {1, 1}, 1, 9, 2, 0.001f, false},
                                                     {{0, -3.25f, 0}, {1, 1}, 1, 9, 2, 0.001f, false},
                                                     {{0, 0.75f, 6}, {1, 1}, 1, 3, 6.5f, 0.001f, false},
                                                     {{0, 0.75f, -6}, {1, 1}, 1, 3, -5.5f, 0.001f, false},
                                                     {{0, 0, 0}, {4, 4}, 1, 9, 0.004f, 0, true},
                                                     {{0, 0, 0}, {4, 4}, 1, 9, 0.004f, 0.00025f, true},
                                                     {{0, 0, 0}, {4, 4}, 1, 9, 0.004f, 0.001f, false}}};
                for (size_t i = 0; i < samples.size(); ++i)
                {
                    const auto& s = samples[i];
                    auto frame = Scene::extractSceneFrame(
                        biasScene, {384, 256}, true, {true, s.center, s.halfExtent, s.nearPlane, s.farPlane, s.bias});
                    ASSERT_TRUE(frame);
                    frame->shadowDraws = {
                        {receiver->meshes()[0], {},
                         glm::translate(glm::mat4(1), {s.center.x, s.center.y, s.casterZ}) *
                             glm::scale(glm::mat4(1), {4, 4, 1})}};
                    expectShadowPixel(*frame, shadowScenePixels(*native, renderer, *frame), {0, 0.75f, 0},
                                      s.occluded ? glm::vec3(lighting.ambient)
                                                 : glm::vec3(lighting.ambient) + lighting.color * lighting.intensity);
                }
            }

            // 同会话验证 Lambert/PBR 斜边 PCF 软阴影过滤与 Unlit 不受影响语义。
            {
                Model::PrepareOptions pbrOpt;
                pbrOpt.shading = ShadingMode::Pbr;
                const auto pbrReceiver = Model::prepareModel(renderer, "models/shadows/receiver.obj", pbrOpt);
                const auto unlitReceiver = Model::prepareModel(renderer, "models/shadows/receiver.obj");
                const Lighting pcfLighting{{0, 0, -1}, {0.6f, 0.7f, 0.8f}, 0.8f, 0.2f};
                const ShadowSettings pcfSettings{true, {0, 0, 0}, {6, 6}, 1, 9, 0.001f};
                const glm::mat4 casterModel =
                    glm::translate(glm::mat4(1), {0.0f, 0.75f, 2.0f}) *
                    glm::rotate(glm::mat4(1), glm::radians(25.0f), {0, 0, 1}) *
                    glm::scale(glm::mat4(1), {1.0f, 0.5f, 1.0f});
                const auto scanEdge = [&](const std::shared_ptr<const Model::PreparedModel>& model) {
                    Scene::Scene pcfScene;
                    pcfScene.createEntity(model);
                    pcfScene.setCamera(shadowReviewCamera());
                    pcfScene.setLighting(pcfLighting);
                    auto frame = Scene::extractSceneFrame(pcfScene, {384, 256}, true, pcfSettings);
                    EXPECT_TRUE(frame);
                    frame->shadowDraws = {{model->meshes()[0], {}, casterModel}};
                    const auto pixels = shadowScenePixels(*native, renderer, *frame);
                    const auto toPixel = [&](glm::vec3 world) {
                        const auto clip = frame->camera.projection * frame->camera.view * glm::vec4(world, 1.0f);
                        const auto ndc = glm::vec3(clip) / clip.w;
                        return std::pair{int((ndc.x * 0.5f + 0.5f) * 384.0f), int((ndc.y * 0.5f + 0.5f) * 256.0f)};
                    };
                    const auto [xLit, yLit] = toPixel({0.0f, -0.75f, 0.0f});
                    const auto [xDark, yDark] = toPixel({0.0f, 0.75f, 0.0f});
                    const int yMin = std::min(yLit, yDark);
                    const int yMax = std::max(yLit, yDark);
                    const uint8_t litR = pixels[(size_t(yLit) * 384u + size_t(xLit)) * 4u];
                    const uint8_t darkR = pixels[(size_t(yDark) * 384u + size_t(xDark)) * 4u];
                    int occludedCount = 0, partialCount = 0, litCount = 0;
                    for (int y = yMin; y <= yMax; ++y)
                    {
                        const uint8_t r = pixels[(size_t(y) * 384u + size_t(xLit)) * 4u];
                        if (r <= darkR + 2) ++occludedCount;
                        else if (r >= litR - 2) ++litCount;
                        else ++partialCount;
                    }
                    return std::tuple{darkR, litR, occludedCount, partialCount, litCount};
                };

                const auto [lambertDark, lambertLit, lambertOcc, lambertPart, lambertFull] = scanEdge(receiver);
                EXPECT_LT(lambertDark + 40, lambertLit);
                EXPECT_GT(lambertOcc, 5);
                EXPECT_GT(lambertFull, 5);
                EXPECT_GT(lambertPart, 0);

                const auto [pbrDark, pbrLit, pbrOcc, pbrPart, pbrFull] = scanEdge(pbrReceiver);
                EXPECT_LT(pbrDark + 40, pbrLit);
                EXPECT_GT(pbrOcc, 5);
                EXPECT_GT(pbrFull, 5);
                EXPECT_GT(pbrPart, 0);

                const auto [unlitDark, unlitLit, unlitOcc, unlitPart, unlitFull] = scanEdge(unlitReceiver);
                EXPECT_NEAR(unlitDark, unlitLit, 2);
                EXPECT_EQ(unlitPart, 0);
            }

            for (int i = 0; i < 4; ++i)
            {
                native->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const FrameExtent extent{i < 2 ? 384u : 256u, 256};
                const auto output = renderer.prepareSceneTarget(extent);
                settings.enabled = i != 2;
                const auto frame = Scene::extractSceneFrame(scene, extent, true, settings);
                ASSERT_TRUE(frame);
                auto* ui = ImGui::GetBackgroundDrawList();
                ui->AddImage(renderer.sceneImage(output), {0, 0}, {256, 256});
                ui->AddRectFilled({8, 8}, {64, 64}, IM_COL32(255, 0, 255, 255));
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame->input(), {0.12f, 0.16f, 0.22f, 1}, [&](void* c) {
                    renderer.imgui().recordDrawData(renderer.imgui().drawData(), static_cast<VkCommandBuffer>(c));
                });
            }
            // 不等待回读即关闭，覆盖阴影采样、视口重建、UI 合成和在途描述符的共同持有期。
        }
        EXPECT_EQ(occurrences(messages_.str(), "Created shadow target:"), 2u);
        cleanValidation();
    }

    TEST_F(ModelSceneIntegrationTest, ModelShadowDepthSlotsValidationAndSourceTransforms)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            ShaderResource empty({}, {});
            EXPECT_THROW(renderer.prepareShadowResources(empty), Error::Exception);
            shaders(renderer);
            EXPECT_THROW(renderer.prepareShadowResources(empty), Error::Exception);
            native->pollEvents();
            ASSERT_TRUE(renderer.beginUiFrame());
            auto initOutput = renderer.prepareSceneTarget({128, 128});
            FrameInput invalidInput{{128, 128}, {glm::mat4(1), glm::mat4(1)}, {}, {}, ShadowFrame{}, {}};
            renderer.imgui().endFrame();
            bool recorded = false;
            const auto reject = [&](auto&& submit) {
                EXPECT_THROW(submit(), Error::Exception);
                EXPECT_FALSE(recorded);
            };
            reject([&] { renderer.drawSceneTarget(initOutput, invalidInput, {0, 0, 0, 1}, [&](void*) { recorded = true; }); });
            shadowShader(renderer);
            const std::array<DrawItem, 1> invalid{DrawItem{}};
            invalidInput.shadowDraws = invalid;
            reject([&] { renderer.drawSceneTarget(initOutput, invalidInput, {0, 0, 0, 1}, [&](void*) { recorded = true; }); });
            reject([&] { renderer.drawFrame(invalidInput, {0, 0, 0, 1}, [&](void*) { recorded = true; }); });
            invalidInput.shadow.reset();
            EXPECT_NO_THROW(renderer.drawSceneTarget(initOutput, invalidInput, {0, 0, 0, 1}));
            EXPECT_EQ(occurrences(messages_.str(), "Created shadow target:"), 0u);

            auto& manager = ResourceManager::instance();
            const auto vert = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/cube.vert.spv");
            const auto frag = manager.hold<ShaderResource>(ResourceType::SHADER, "shaders/cube.frag.spv");
            ASSERT_TRUE(vert && frag);
            const auto vertWords = vert.get<ShaderResource>()->words();
            const auto fragWords = frag.get<ShaderResource>()->words();

            const std::array<MeshVertex, 4> vertices{
                {{{-2, -2, 0}, {0, 0}}, {{2, -2, 0}, {1, 0}}, {{2, 2, 0}, {1, 1}}, {{-2, 2, 0}, {0, 1}}}};
            const auto mesh = renderer.createMesh(vertices, std::array<uint32_t, 6>{0, 1, 2, 0, 2, 3});
            const auto reversed = renderer.createMesh(vertices, std::array<uint32_t, 6>{2, 1, 0, 3, 2, 0});
            ShadowSettings settings{true, {0, 0, 0}, {4, 4}, 1, 9, 0.001f};
            const auto shadow = makeShadowFrame(settings, {0, 0, -1});
            const std::array<float, 6> expected{0.25f, 0.75f, 1, 0.125f, 1, 0.25f};
            std::array<VkImage, 2> handles{};
            std::array<SceneOutput, 2> outputs;
            for (size_t i = 0; i < expected.size(); ++i)
            {
                native->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                const FrameExtent extent{i < 3 ? 192u : 256u, 128};
                auto output = renderer.prepareSceneTarget(extent);
                std::vector<DrawItem> casters;
                if (i != 2 && i != 4)
                {
                    casters.push_back({mesh, {}, glm::translate(glm::mat4(1), {0, 0, -2})});
                    if (i != 1)
                    {
                        const auto model = glm::translate(glm::mat4(1), {0, 0, i == 3 ? 3 : 2}) *
                                           glm::scale(glm::mat4(1), {-0.5f, 1.5f, 0});
                        casters.push_back({i == 3 ? reversed : mesh, {}, model});
                    }
                }
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, {extent, {glm::mat4(1), glm::mat4(1)}, {}, {},
                                                  i == 4 ? std::nullopt : shadow, casters},
                                         {0, 0, 0, 1});
                outputs[i % 2] = output;
                if (i == 4)
                    continue;
                Detail::RendererReviewAccess::withShadowOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        EXPECT_EQ(image.extent().width, 1024u);
                        EXPECT_EQ(image.mipLevels(), 1u);
                        if (!handles[i % 2])
                            handles[i % 2] = image.handle();
                        EXPECT_EQ(image.handle(), handles[i % 2]);
                        EXPECT_NEAR(readShadowDepth(device, image), expected[i], 0.00003f);
                        EXPECT_NEAR(readShadowDepth(device, image, 0, 0), 1, 0.00003f);
                        if (i == 0)
                        {
                            const double encoded = 1.055 * std::pow(0.25, 1.0 / 2.4) - 0.055;
                            EXPECT_NEAR(sampleShadowDepth(device, image, vertWords, fragWords), encoded * 255, 2);
                        }
                    });
                if (i == 1)
                    Detail::RendererReviewAccess::withShadowOutput(
                        renderer, outputs[0], [&](const auto& device, const auto& image) {
                            EXPECT_NE(handles[0], handles[1]);
                            EXPECT_NEAR(readShadowDepth(device, image), 0.25f, 0.00003f);
                        });
            }

            // 源根平移先于实体旋转与缩放：验证在途深度槽保护与中心深度 0.25
            auto model = Model::prepareModel(renderer, "models/shadow-source.gltf");
            auto srcCasters = expandModel(*model, Scene::Transform({1, 2, 2}, {1, 0, 0, 1}, {2, -3, 0}).matrix());
            for (auto& d : srcCasters)
                d.material = {};
            ShadowSettings srcSettings{true, {2, 68.0f / 3, 0}, {3, 3}, 1, 9, 0.001f};
            for (int i = 0; i < 4; ++i)
            {
                native->pollEvents();
                ASSERT_TRUE(renderer.beginUiFrame());
                auto output = renderer.prepareSceneTarget({256, 128});
                srcSettings.enabled = i != 2;
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(
                    output, {{256, 128}, {glm::mat4(1), glm::mat4(1)}, {}, {}, makeShadowFrame(srcSettings, {0, 0, -1}),
                             srcCasters},
                    {0, 0, 0, 1});
                if (i == 0)
                    Detail::RendererReviewAccess::withShadowOutput(
                        renderer, output, [&](const auto& device, const auto& image) {
                            EXPECT_NEAR(readShadowDepth(device, image), 0.25f, 0.00003f);
                        });
            }
        }
        EXPECT_EQ(occurrences(messages_.str(), "Created shadow target:"), 2u);
        EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline: color format 0,"), 2u);
        cleanValidation();
    }

    TEST_F(ModelSceneIntegrationTest, IndependentSceneTargetsFollowFrameSlotsAndExpireAfterResize)
    {
        auto native = window();
        SceneOutput retained;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto model = Model::prepareModel(renderer, GLTF);
            EXPECT_FALSE(renderer.prepareSceneTarget({0, 100}).valid());
            for (uint32_t i = 0; i < 10; ++i)
            {
                native->pollEvents();
                const auto windowExtent = renderer.beginUiFrame();
                ASSERT_TRUE(windowExtent);
                const FrameExtent extent{i < 4 ? 192u : 256u, 128};
                auto output = renderer.prepareSceneTarget(extent);
                ASSERT_TRUE(output.valid());
                EXPECT_EQ(output.extent(), extent);
                if (i == 0)
                    retained = output;
                if (i == 4)
                    EXPECT_FALSE(retained.valid());
                Scene::Scene scene;
                scene.setCamera(Scene::Camera({0, 0, 3}));
                if (i % 2)
                    scene.createEntity(model);
                const auto frame = Scene::extractSceneFrame(scene, extent);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame->input(), {0.2f, 0.3f, 0.4f, 1});
                if (i == 9)
                    retained = output;
                Detail::RendererReviewAccess::withSceneOutput(
                    renderer, output, [&](const auto& device, const auto& image) {
                        const auto pixels =
                            TextureGpuReview::captureMip(device, image, 0, {extent.width, extent.height});
                        EXPECT_NEAR(pixels[0], 51, 1);
                        EXPECT_NEAR(pixels[1], 77, 1);
                        EXPECT_NEAR(pixels[2], 102, 1);
                        if (i % 2)
                            EXPECT_TRUE(std::any_of(pixels.begin(), pixels.end(), [](auto value) {
                                return value != 51 && value != 77 && value != 102 && value != 255;
                            }));
                    });
            }
        }
        EXPECT_FALSE(retained.valid());
        EXPECT_EQ(occurrences(messages_.str(), "Scene target created:"), 2u);
        EXPECT_EQ(occurrences(messages_.str(), "Scene target resized:"), 2u);
        EXPECT_NE(messages_.str().find("192x128 -> 256x128, color format"), std::string::npos);
        cleanValidation();
    }

    TEST_F(ModelSceneIntegrationTest, FixedShowcaseModelsPrepareReleaseCpuAndDrawSharedInstances)
    {
        copyShowcaseModels();
        auto native = window();
        std::vector<std::shared_ptr<const Model::PreparedModel>> models;
        size_t expectedMeshes = 0, expectedTextures = 0, expectedMaterials = 0;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            for (const auto& [path, scale, y] :
                 {std::tuple{"models/utah_teapot/utah_teapot.obj", 0.075f, 0.0f},
                  std::tuple{"models/avocado/glTF/Avocado.gltf", 24.0f, -0.78f}})
            {
                auto prepared = Model::prepareModel(renderer, path);
                ASSERT_FALSE(prepared->draws().empty());
                expectedMeshes += prepared->meshes().size();
                expectedTextures += prepared->textures().size();
                expectedMaterials += prepared->materials().size();
                models.push_back(std::move(prepared));
                released(ResourceType::MODEL, path);
                Scene::Scene comparison;
                comparison.setCamera(Scene::Camera({0, 0, 6}));
                comparison.createEntity(models.back(), Scene::Transform({0, y, 0}, {1, 0, 0, 0}, glm::vec3(scale)));
                comparison.createEntity(models.back(),
                                        Scene::Transform({1000, y, 0}, {1, 0, 0, 0}, glm::vec3(scale)));
                const auto enabled = Scene::extractSceneFrame(comparison, {384, 256});
                const auto disabled = Scene::extractSceneFrame(comparison, {384, 256}, false);
                ASSERT_TRUE(enabled && disabled);
                EXPECT_EQ(enabled->draws.size(), models.back()->draws().size());
                EXPECT_EQ(disabled->draws.size(), models.back()->draws().size() * 2);
                const auto pixels = TextureGpuReview::captureFrame(renderer, enabled->input());
                EXPECT_EQ(pixels, TextureGpuReview::captureFrame(renderer, disabled->input()));
                auto blank = *enabled;
                blank.draws.clear();
                EXPECT_NE(pixels, TextureGpuReview::captureFrame(renderer, blank.input()));
            }
            released(ResourceType::TEXTURE, "models/avocado/glTF/Avocado_baseColor.png");
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), expectedMeshes);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), expectedTextures);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), expectedMaterials);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 4u);
        }
        for (const auto& model : models)
            EXPECT_FALSE(model->meshes()[0].valid());
        cleanValidation();
    }

    TEST_F(ModelSceneIntegrationTest, LambertInterpolatedNormalsMatchFinalRootNodeEntityInverseTranspose)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<TransformModelLoader>());
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            Model::PrepareOptions options;
            options.shading = ShadingMode::Lambert;
            const auto model = Model::prepareModel(renderer, "models/normal-transforms.gltf", options);
            released(ResourceType::MODEL, "models/normal-transforms.gltf");
            EXPECT_EQ(model->rootNode(), 2u);
            Scene::Scene scene;
            const auto entity = scene.createEntity(model);
            const Lighting lighting{{-0.3f, -0.6f, -1}, {0.4f, 0.8f, 0.6f}, 0.8f, 0.12f};
            scene.setLighting(lighting);
            const auto root = glm::translate(glm::mat4(1), glm::vec3(10, 0, 0));
            const auto parent = glm::rotate(glm::mat4(1), glm::radians(90.0f), glm::vec3(0, 0, 1));
            const auto leaf =
                glm::translate(glm::mat4(1), glm::vec3(2, 0, 0)) * glm::scale(glm::mat4(1), glm::vec3(2, 3, 1));
            const glm::quat rotation(glm::vec3(0.3f, 0.4f, 0.2f));
            const std::array<glm::vec3, 3> positions{glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, 1, 0)};
            const std::array<glm::vec3, 3> normals{glm::vec3(1, 1, 0), glm::vec3(0, 1, 1), glm::vec3(1, 0, 1)};
            for (const auto& scale : {glm::vec3(1), glm::vec3(2, 3, 0.5f), glm::vec3(-2, 3, 0.5f), glm::vec3(0.001f)})
            {
                SCOPED_TRACE(scale.x);
                const Scene::Transform transform({-0.5f, 0.3f, 0.1f}, rotation, scale);
                ASSERT_TRUE(scene.setTransform(entity, transform));
                const auto expectedModel = glm::translate(glm::mat4(1), transform.position()) *
                                           glm::mat4_cast(rotation) * glm::scale(glm::mat4(1), scale) * root * parent *
                                           leaf;
                auto snapshot = Scene::extractSceneFrame(scene, {384, 256}, false);
                ASSERT_TRUE(snapshot && snapshot->draws.size() == 5u);
                std::array draws{snapshot->draws[2]};
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        EXPECT_NEAR(draws[0].model[c][r], expectedModel[c][r], 0.00001f);
                const auto center = glm::vec3(expectedModel * glm::vec4(1.0f / 3, 1.0f / 3, 0, 1));
                float radius = 0;
                for (const auto& pos : positions)
                    radius = std::max(radius, glm::length(glm::vec3(expectedModel * glm::vec4(pos, 1)) - center));
                radius *= 1.3f;
                auto projection = glm::orthoRH_ZO(-radius * 1.5f, radius * 1.5f, -radius, radius, 0.1f, 20.0f);
                projection[1][1] *= -1;
                const CameraMatrices camera{glm::lookAtRH(center + glm::vec3(0, 0, 10), center, glm::vec3(0, 1, 0)),
                                            projection};
                std::array<glm::vec2, 3> screen;
                std::array<glm::dvec3, 3> worldNormals;
                const auto normalMatrix = glm::transpose(glm::inverse(glm::dmat3(expectedModel)));
                Detail::RendererReviewAccess::withFrame(
                    renderer, {{384, 256}, camera, draws, snapshot->lighting},
                    [&](const auto&, const auto&, auto, auto nativeDraws) {
                        ASSERT_EQ(nativeDraws.size(), 1u);
                        for (int c = 0; c < 3; ++c)
                            for (int r = 0; r < 3; ++r)
                                EXPECT_NEAR(nativeDraws[0].object.normalColumns[c][r], normalMatrix[c][r],
                                            0.00001 * std::max(1.0, std::abs(normalMatrix[c][r])));
                    });
                for (size_t corner = 0; corner < 3; ++corner)
                {
                    const auto clip = camera.projection * camera.view * expectedModel * glm::vec4(positions[corner], 1);
                    screen[corner] = {(clip.x / clip.w + 1) * 192, (clip.y / clip.w + 1) * 128};
                    worldNormals[corner] = glm::normalize(normalMatrix * glm::dvec3(normals[corner]));
                }
                const auto cross = [](glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; };
                const glm::vec2 sample(192.5f, 128.5f);
                const float area = cross(screen[1] - screen[0], screen[2] - screen[0]);
                const float a = cross(screen[1] - sample, screen[2] - sample) / area;
                const float b = cross(screen[2] - sample, screen[0] - sample) / area;
                const float c = 1 - a - b;
                ASSERT_GT(std::min({a, b, c}), 0.05f);
                const auto interpolated = glm::normalize(double(a) * worldNormals[0] + double(b) * worldNormals[1] +
                                                         double(c) * worldNormals[2]);
                const double diffuse =
                    std::max(glm::dot(interpolated, -glm::normalize(glm::dvec3(lighting.direction))), 0.0);
                const auto expected =
                    glm::dvec3(0.5, 1, 0.5) *
                    (glm::dvec3(lighting.ambient) + glm::dvec3(lighting.color) * double(lighting.intensity) * diffuse);
                const auto pixels =
                    TextureGpuReview::captureFrame(renderer, {{384, 256}, camera, draws, snapshot->lighting});
                ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                for (int channel = 0; channel < 3; ++channel)
                {
                    const double linear = std::clamp(expected[channel], 0.0, 1.0);
                    const double encoded =
                        linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1.0 / 2.4) - 0.055;
                    EXPECT_NEAR(pixels[(128 * 384 + 192) * 4 + channel], encoded * 255, 2);
                }
                EXPECT_EQ(pixels[(128 * 384 + 192) * 4 + 3], 255);
            }
        }
        cleanValidation();
    }

    TEST_F(ModelSceneIntegrationTest, DemoModelsWithUnicodeDependenciesPreserveDirectionAndExactGpuColor)
    {
        const std::string gltfPath = "models/场景😀-gltf/模型.gltf";
        const std::string objPath = "models/场景😀-obj/模型.obj";
        const std::string pngPath = "textures/方向😀.png";
        const auto read = [](std::string_view path) {
            std::ifstream input(Utils::Utf8::toFsPath(std::string("assets/") + std::string(path)), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(input), {});
        };
        const auto write = [](std::string_view path, const std::string& contents) {
            const auto native = Utils::Utf8::toFsPath(std::string("assets/") + std::string(path));
            fs::create_directories(native.parent_path());
            std::ofstream(native, std::ios::binary).write(contents.data(), contents.size());
        };
        const auto replace = [](std::string& text, std::string_view from, std::string_view to) {
            text.replace(text.find(from), from.size(), to);
        };
        auto gltf = read(GLTF);
        replace(gltf, "geometry/mesh.bin", "几何😀/网格.bin");
        replace(gltf, "../../textures/direction_rgba.png", "../../textures/方向😀.png");
        write(gltfPath, gltf);
        write("models/场景😀-gltf/几何😀/网格.bin", read("models/minimal_gltf/geometry/mesh.bin"));
        auto obj = read(OBJ);
        replace(obj, "materials/model.mtl", "材质😀/颜色.mtl");
        write(objPath, obj);
        auto mtl = read("models/minimal_obj/materials/model.mtl");
        replace(mtl, "../../../textures/direction_rgba.png", "../../../textures/方向😀.png");
        write("models/场景😀-obj/材质😀/颜色.mtl", mtl);
        write(pngPath, read(PNG));

        auto native = window(true);
        ASSERT_TRUE(SetWindowPos(glfwGetWin32Window(static_cast<GLFWwindow*>(native->getNativeHandle())), HWND_TOPMOST,
                                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE));
        std::array<std::shared_ptr<const Model::PreparedModel>, 2> models;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            constexpr Model::PrepareOptions exactSampling{
                TextureMipMode::BaseLevel, {TextureFilter::Nearest, TextureFilter::Nearest, MipFilter::BaseLevel}};
            models = {Model::prepareModel(renderer, gltfPath, exactSampling),
                      Model::prepareModel(renderer, objPath, exactSampling)};
            ResourceManager::instance().reset();
            released(ResourceType::MODEL, gltfPath);
            released(ResourceType::MODEL, objPath);
            released(ResourceType::TEXTURE, pngPath);
            const std::array<std::array<int, 4>, 12> direction = {{{255, 255, 255, 255}, {255, 128, 0, 255},
                                                                   {0, 0, 0, 255},       {0, 0, 255, 255},
                                                                   {128, 128, 128, 255}, {255, 0, 0, 255},
                                                                   {0, 255, 0, 255},     {255, 0, 255, 255},
                                                                   {0, 255, 255, 255},   {17, 34, 51, 255},
                                                                   {255, 255, 0, 255},   {128, 0, 255, 255}}};
            auto ortho = glm::orthoRH_ZO(-1.5f, 1.5f, -1.0f, 1.0f, 0.1f, 10.0f);
            ortho[1][1] *= -1;
            const CameraMatrices cam{glm::lookAtRH(glm::vec3(0, 0, 3), glm::vec3(0), glm::vec3(0, 1, 0)), ortho};
            for (const auto& model : models)
            {
                const auto draws = expandModel(*model, glm::inverse(model->draws()[0].model));
                const auto pixels = TextureGpuReview::captureFrame(renderer, {{384, 256}, cam, draws});
                ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 3; ++col)
                    {
                        const float x = -1 + (col + 0.5f) / 3, y = 0.5f - (row + 0.5f) / 4;
                        const size_t idx = (size_t((1 - y) * 128) * 384 + size_t((x / 1.5f + 1) * 192)) * 4;
                        for (size_t c = 0; c < 4; ++c)
                            EXPECT_NEAR(pixels[idx + c], direction[row * 3 + col][c], c == 3 ? 0 : 1);
                    }
                const size_t tintIdx = (size_t((1 + 1.0f / 6) * 128) * 384 + size_t((0.6f / 1.5f + 1) * 192)) * 4;
                EXPECT_NEAR(pixels[tintIdx + 0], 188, 1);
                EXPECT_NEAR(pixels[tintIdx + 1], 255, 1);
                EXPECT_NEAR(pixels[tintIdx + 2], 188, 1);
            }
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 4u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 4u);
        }
        cleanValidation();
    }

    TEST_F(ModelSceneIntegrationTest, AvocadoPbrPreparationReleasesCpuTexturesAndDiffersFromLambert)
    {
        constexpr std::string_view avocado = "models/avocado/glTF/Avocado.gltf";
        constexpr std::string_view basePng = "models/avocado/glTF/Avocado_baseColor.png";
        constexpr std::string_view normalPng = "models/avocado/glTF/Avocado_normal.png";
        constexpr std::string_view mrPng = "models/avocado/glTF/Avocado_roughnessMetallic.png";

        auto native = window(true);
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const Model::PrepareOptions pbrOptions{
                TextureMipMode::FullChain,
                {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear},
                ShadingMode::Pbr};
            const Model::PrepareOptions lambertOptions{
                TextureMipMode::FullChain,
                {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear},
                ShadingMode::Lambert};

            const auto pbrModel = Model::prepareModel(renderer, avocado, pbrOptions);
            const auto lambertModel = Model::prepareModel(renderer, avocado, lambertOptions);
            EXPECT_EQ(pbrModel->textures().size(), 3u);
            EXPECT_EQ(lambertModel->textures().size(), 1u);

            ResourceManager::instance().reset();
            released(ResourceType::MODEL, avocado);
            released(ResourceType::TEXTURE, basePng);
            released(ResourceType::TEXTURE, normalPng);
            released(ResourceType::TEXTURE, mrPng);

            ASSERT_TRUE(pbrModel->bounds().has_value());
            const auto center = (pbrModel->bounds()->min + pbrModel->bounds()->max) * 0.5f;
            const float radius = std::max(glm::length(pbrModel->bounds()->max - pbrModel->bounds()->min) * 0.55f, 0.01f);
            auto projection = glm::perspectiveRH_ZO(glm::radians(45.0f), 384.0f / 256.0f, 0.001f, 10.0f);
            projection[1][1] *= -1.0f;
            const CameraMatrices cam{
                glm::lookAtRH(center + glm::vec3(0.0f, radius * 0.25f, radius * 2.2f), center,
                              glm::vec3(0.0f, 1.0f, 0.0f)),
                projection};
            const Lighting lighting{glm::normalize(glm::vec3(0.5f, -0.7f, -0.5f)), {1.0f, 1.0f, 1.0f}, 1.4f, 0.12f};

            const auto pbrPixels = TextureGpuReview::captureFrame(
                renderer, {{384, 256}, cam, expandModel(*pbrModel, glm::mat4(1.0f)), lighting});
            const auto lambertPixels = TextureGpuReview::captureFrame(
                renderer, {{384, 256}, cam, expandModel(*lambertModel, glm::mat4(1.0f)), lighting});
            ASSERT_EQ(pbrPixels.size(), 384u * 256u * 4u);
            ASSERT_EQ(lambertPixels.size(), pbrPixels.size());

            size_t diffPixels = 0;
            for (size_t i = 0; i < pbrPixels.size(); i += 4)
            {
                const int diff = std::abs(int(pbrPixels[i]) - int(lambertPixels[i])) +
                                 std::abs(int(pbrPixels[i + 1]) - int(lambertPixels[i + 1])) +
                                 std::abs(int(pbrPixels[i + 2]) - int(lambertPixels[i + 2]));
                if (diff > 12)
                    ++diffPixels;
            }
            EXPECT_GT(diffPixels, 500u);
        }
        cleanValidation();
    }

    TEST_F(ModelSceneIntegrationTest, ParentChildWorldDrawsShadowCullingAndPickMatch)
    {
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto model = Model::prepareModel(renderer, GLTF);
            ASSERT_TRUE(model && !model->draws().empty() && model->bounds().has_value());

            Scene::Scene scene;
            scene.setCamera(Scene::Camera({2.5f, 1.5f, 5.0f}));
            const auto parent = scene.createEntity({}, Scene::Transform({2.0f, 0.0f, 0.0f}), true, "Parent");
            const auto child = scene.createEntity(model, Scene::Transform({0.0f, 1.0f, 0.0f}), true, "Child");
            ASSERT_TRUE(scene.setParent(child, parent));
            const auto world = scene.worldMatrix(child);
            ASSERT_TRUE(world.has_value());

            Render::ShadowSettings shadows;
            shadows.enabled = true;
            const auto frame = Scene::extractSceneFrame(scene, {320, 240}, true, shadows);
            ASSERT_TRUE(frame.has_value());
            ASSERT_EQ(frame->draws.size(), model->draws().size());
            ASSERT_EQ(frame->shadowDraws.size(), model->draws().size());
            const auto expectedDraw = (*world) * model->draws()[0].model;
            EXPECT_EQ(frame->draws[0].model, expectedDraw);
            EXPECT_EQ(frame->shadowDraws[0].model, expectedDraw);

            // 拾取命中世界位置，不命中局部位置。
            const auto worldCenter = glm::vec3((*world) * glm::vec4((model->bounds()->min + model->bounds()->max) * 0.5f, 1.0f));
            const auto hitWorld = scene.pick(Math::LineSegment{worldCenter + glm::vec3(0, 0, 2), worldCenter - glm::vec3(0, 0, 2)});
            ASSERT_TRUE(hitWorld.has_value());
            EXPECT_EQ(hitWorld->id, child);
            const auto localCenter = (model->bounds()->min + model->bounds()->max) * 0.5f + glm::vec3(0.0f, 1.0f, 0.0f);
            EXPECT_FALSE(scene.pick(Math::LineSegment{localCenter + glm::vec3(0, 0, 2), localCenter - glm::vec3(0, 0, 2)}).has_value());

            // 父级移出视锥后子级主绘制被裁剪，阴影仍保留。
            ASSERT_TRUE(scene.setTransform(parent, Scene::Transform({200.0f, 0.0f, 0.0f})));
            const auto culled = Scene::extractSceneFrame(scene, {320, 240}, true, shadows);
            ASSERT_TRUE(culled.has_value());
            EXPECT_TRUE(culled->draws.empty());
            EXPECT_EQ(culled->shadowDraws.size(), model->draws().size());

            // 隐藏父级不隐藏子级：子级保持自身可见性、绘制与拾取。
            ASSERT_TRUE(scene.setTransform(parent, Scene::Transform({2.0f, 0.0f, 0.0f})));
            ASSERT_TRUE(scene.setVisible(parent, false));
            const auto parentHidden = Scene::extractSceneFrame(scene, {320, 240}, true, shadows);
            ASSERT_TRUE(parentHidden.has_value());
            EXPECT_EQ(parentHidden->draws.size(), model->draws().size());
            EXPECT_EQ(parentHidden->shadowDraws.size(), model->draws().size());
            const auto hitWhenParentHidden =
                scene.pick(Math::LineSegment{worldCenter + glm::vec3(0, 0, 2), worldCenter - glm::vec3(0, 0, 2)});
            ASSERT_TRUE(hitWhenParentHidden.has_value());
            EXPECT_EQ(hitWhenParentHidden->id, child);
        }
        cleanValidation();
    }

    TEST_F(ModelSceneIntegrationTest, ExtractSceneFrameOrdersOpaqueMaskBlendSortedAndExcludesBlendShadows)
    {
        class AlphaLayoutModelLoader final : public LoaderBase
        {
        public:
            std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view path) override
            {
                MeshData quad;
                quad.vertices = {
                    {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
                    {{1.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
                    {{1.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
                    {{-1.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
                };
                quad.indices = {0, 1, 2, 0, 2, 3};

                auto makeColorMaterial = [](std::array<float, 3> color, Resource::AlphaMode mode, float baseAlpha) {
                    ModelMaterial mat;
                    mat.color = color;
                    mat.alphaMode = mode;
                    mat.baseAlpha = baseAlpha;
                    mat.doubleSided = true;
                    return mat;
                };

                if (path == "models/alpha_mix.gltf")
                {
                    // 特意让 Mask 先于 Opaque，Blend 居中，验证帧提取严格按 Opaque -> Mask -> Blend 输出。
                    std::vector<ModelMaterial> materials{
                        makeColorMaterial({0.2f, 0.8f, 0.2f}, Resource::AlphaMode::Mask, 1.0f),
                        makeColorMaterial({0.8f, 0.8f, 0.8f}, Resource::AlphaMode::Opaque, 1.0f),
                        makeColorMaterial({0.1f, 0.9f, 0.1f}, Resource::AlphaMode::Blend, 0.5f),
                    };
                    std::vector<ModelPart> parts{{0, 0}, {0, 1}, {0, 2}};
                    std::vector<ModelNode> nodes{
                        {"Root", glm::mat4(1.0f), {1, 2, 3}, {}},
                        {"MaskChild", glm::mat4(1.0f), {}, {0}},
                        {"OpaqueChild", glm::mat4(1.0f), {}, {1}},
                        {"BlendChild", glm::mat4(1.0f), {}, {2}},
                    };
                    return std::make_unique<ModelResource>(id, std::vector<MeshData>{quad}, std::move(materials),
                                                           std::move(parts), std::move(nodes), 0);
                }

                const auto color = path == "models/blend_red.gltf" ? std::array{1.0f, 0.0f, 0.0f}
                                                                   : std::array{0.0f, 0.0f, 1.0f};
                std::vector<ModelMaterial> materials{makeColorMaterial(color, Resource::AlphaMode::Blend, 0.5f)};
                std::vector<ModelPart> parts{{0, 0}};
                std::vector<ModelNode> nodes{{"Root", glm::mat4(1.0f), {}, {0}}};
                return std::make_unique<ModelResource>(id, std::vector<MeshData>{quad}, std::move(materials),
                                                       std::move(parts), std::move(nodes), 0);
            }
        };

        auto& manager = ResourceManager::instance();
        manager.registerLoader(ResourceType::MODEL, std::make_unique<AlphaLayoutModelLoader>());

        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            shadowShader(renderer);

            const Model::PrepareOptions unlit{{}, {}, ShadingMode::Unlit};
            const auto mixed = Model::prepareModel(renderer, "models/alpha_mix.gltf", unlit);
            const auto blendRed = Model::prepareModel(renderer, "models/blend_red.gltf", unlit);
            const auto blendBlue = Model::prepareModel(renderer, "models/blend_blue.gltf", unlit);
            ASSERT_EQ(mixed->draws().size(), 3u);
            ASSERT_EQ(mixed->drawBounds().size(), 3u);

            Scene::Scene scene;
            scene.setCamera(Scene::Camera({0.0f, 0.0f, 5.0f}));
            scene.createEntity(blendRed, Scene::Transform({0.0f, 0.0f, 2.0f}), true, "Near");
            scene.createEntity(mixed, Scene::Transform({0.0f, 0.0f, 0.0f}), true, "Mixed");
            scene.createEntity(blendBlue, Scene::Transform({0.0f, 0.0f, -2.0f}), true, "Far");
            // 相机位于 (0,0,5)，以下两个实体距相机平方距离完全相同，应保持稳定插入顺序：TieRed 在 TieBlue 之前。
            scene.createEntity(blendRed, Scene::Transform({1.0f, 0.0f, 2.0f}), true, "TieRed");
            scene.createEntity(blendBlue, Scene::Transform({-1.0f, 0.0f, 2.0f}), true, "TieBlue");

            Render::ShadowSettings shadows;
            shadows.enabled = true;
            const auto frame = Scene::extractSceneFrame(scene, {320, 240}, false, shadows);
            ASSERT_TRUE(frame.has_value());
            ASSERT_EQ(frame->draws.size(), 7u);
            // 阴影投射列表仅包含 Mixed 的 Opaque 与 Mask，不包含任何 Blend 绘制项。
            ASSERT_EQ(frame->shadowDraws.size(), 2u);

            const auto matAlpha = [](const DrawItem& draw) {
                return Detail::RendererReviewAccess::materialDescription(draw.material).alphaMode;
            };
            const auto matColor = [](const DrawItem& draw) {
                return Detail::RendererReviewAccess::materialDescription(draw.material).color;
            };

            EXPECT_EQ(matAlpha(frame->draws[0]), Render::AlphaMode::Opaque);
            EXPECT_EQ(matAlpha(frame->draws[1]), Render::AlphaMode::Mask);
            // Blend 段从远到近：Far(-2, d^2=49) -> MixedBlend(0, d^2=25) -> TieRed(±1,2, d^2=10) -> TieBlue -> Near(2, d^2=9)
            EXPECT_EQ(matAlpha(frame->draws[2]), Render::AlphaMode::Blend);
            EXPECT_EQ(matColor(frame->draws[2]), (std::array{0.0f, 0.0f, 1.0f}));
            EXPECT_EQ(frame->draws[2].model[3][2], -2.0f);
            EXPECT_EQ(matAlpha(frame->draws[3]), Render::AlphaMode::Blend);
            EXPECT_EQ(matColor(frame->draws[3]), (std::array{0.1f, 0.9f, 0.1f}));
            EXPECT_EQ(matColor(frame->draws[4]), (std::array{1.0f, 0.0f, 0.0f}));
            EXPECT_EQ(frame->draws[4].model[3][0], 1.0f);
            EXPECT_EQ(matColor(frame->draws[5]), (std::array{0.0f, 0.0f, 1.0f}));
            EXPECT_EQ(frame->draws[5].model[3][0], -1.0f);
            EXPECT_EQ(matColor(frame->draws[6]), (std::array{1.0f, 0.0f, 0.0f}));
            EXPECT_EQ(frame->draws[6].model[3][2], 2.0f);
            EXPECT_EQ(matAlpha(frame->shadowDraws[0]), Render::AlphaMode::Mask);
            EXPECT_EQ(matAlpha(frame->shadowDraws[1]), Render::AlphaMode::Opaque);

            // 双层重叠半透明物体：验证相机翻转前后按远到近排序与 HDR 混合像素同步反转。
            Scene::Scene layered;
            layered.createEntity(blendRed, Scene::Transform({0.0f, 0.0f, -1.0f}), true, "RedBack");
            layered.createEntity(blendBlue, Scene::Transform({0.0f, 0.0f, 1.0f}), true, "BlueFront");

            constexpr FrameExtent extent{64, 64};
            constexpr size_t center = (extent.height / 2) * extent.width + extent.width / 2;
            const auto readCenterHdr = [&](const FrameInput& input) {
                std::array<float, 4> pixel{};
                Detail::RendererReviewAccess::withHdrSceneTarget(
                    renderer, input, {0, 0, 0, 0}, [&](const auto& device, const auto& image) {
                        const auto pixels = TextureGpuReview::captureRgba16f(device, image, {extent.width, extent.height});
                        ASSERT_GT(pixels.size(), center);
                        pixel = pixels[center];
                    });
                return pixel;
            };

            layered.setCamera(Scene::Camera({0.0f, 0.0f, 4.0f}));
            const auto frontView = Scene::extractSceneFrame(layered, extent);
            ASSERT_TRUE(frontView.has_value());
            ASSERT_EQ(frontView->draws.size(), 2u);
            EXPECT_EQ(matColor(frontView->draws[0]), (std::array{1.0f, 0.0f, 0.0f}));
            EXPECT_EQ(matColor(frontView->draws[1]), (std::array{0.0f, 0.0f, 1.0f}));
            const auto frontPixel = readCenterHdr(frontView->input());
            // 远红(0.5)+近蓝(0.5) 预乘输出：R = 0.25, B = 0.5, A = 0.75
            EXPECT_NEAR(frontPixel[0], 0.25f, 0.005f);
            EXPECT_NEAR(frontPixel[1], 0.0f, 0.005f);
            EXPECT_NEAR(frontPixel[2], 0.5f, 0.005f);
            EXPECT_NEAR(frontPixel[3], 0.75f, 0.005f);

            // 相机移到 -Z 并绕 Y 轴旋转 180 度朝 +Z 看，RedBack 变为更近、BlueFront 变为更远。
            layered.setCamera(Scene::Camera({0.0f, 0.0f, -4.0f}, glm::quat(0.0f, 0.0f, 1.0f, 0.0f)));
            const auto backView = Scene::extractSceneFrame(layered, extent);
            ASSERT_TRUE(backView.has_value());
            ASSERT_EQ(backView->draws.size(), 2u);
            EXPECT_EQ(matColor(backView->draws[0]), (std::array{0.0f, 0.0f, 1.0f}));
            EXPECT_EQ(matColor(backView->draws[1]), (std::array{1.0f, 0.0f, 0.0f}));
            const auto backPixel = readCenterHdr(backView->input());
            // 远蓝(0.5)+近红(0.5) 预乘输出：R = 0.5, B = 0.25, A = 0.75
            EXPECT_NEAR(backPixel[0], 0.5f, 0.005f);
            EXPECT_NEAR(backPixel[1], 0.0f, 0.005f);
            EXPECT_NEAR(backPixel[2], 0.25f, 0.005f);
            EXPECT_NEAR(backPixel[3], 0.75f, 0.005f);
        }
        cleanValidation();
    }
}  // namespace
