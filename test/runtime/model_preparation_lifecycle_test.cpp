#include "model_preparation_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Render;
    using namespace Incubator::Test;
    namespace fs = std::filesystem;

    class ModelPreparationLifecycleTest : public ModelPreparationTest
    {};

    TEST_F(ModelPreparationLifecycleTest, SpecifiedAssetRootPreparesBothFormatsWithoutFallbackToDefaultDirectory)
    {
        const auto root = sandbox_ / Utils::Utf8::toFsPath("独立 资产根");
        fs::rename(sandbox_ / "assets", root);
        fs::create_directories(sandbox_ / "assets");
        fs::copy(root / "fonts", sandbox_ / "assets/fonts", fs::copy_options::recursive);
        auto& manager = ResourceManager::instance();
        manager.reset();
        source_ = std::make_unique<AssetSource>(*fileIO_, root);
        manager.bindAssetSource(source_.get());
        manager.registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(*source_));
        manager.registerLoader(ResourceType::TEXTURE, std::make_unique<TextureLoader>(*source_));
        manager.registerLoader(ResourceType::SHADER, std::make_unique<ShaderLoader>(*source_));
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto gltf = Model::prepareModel(renderer, GLTF);
            const auto obj = Model::prepareModel(renderer, OBJ);
            ASSERT_FALSE(gltf->textures().empty());
            ASSERT_FALSE(obj->textures().empty());
            EXPECT_EQ(gltf->source().path(), GLTF);
            EXPECT_EQ(obj->source().path(), OBJ);
            draw(*native, renderer, *gltf);
            draw(*native, renderer, *obj);
            fs::remove(root / "models/minimal_gltf/geometry/mesh.bin");
            EXPECT_THROW(Model::prepareModel(renderer, GLTF), Error::Exception);
            fs::remove(root / "models/minimal_obj/materials/model.mtl");
            EXPECT_THROW(Model::prepareModel(renderer, OBJ), Error::Exception);
        }
        EXPECT_EQ(fs::current_path(), sandbox_);
        EXPECT_FALSE(fs::exists(sandbox_ / "assets/models"));
        EXPECT_FALSE(fs::exists(sandbox_ / "assets/textures"));
        EXPECT_FALSE(fs::exists(sandbox_ / "assets/shaders"));
        manager.reset();
        manager.bindAssetSource(nullptr);
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, LambertPreparationKeepsImportedUnlitAndSharesRepeatedGeometryAndWhite)
    {
        constexpr const char* path = "models/normals/folded.gltf";
        const auto sourcePath = fs::path("assets") / path;
        std::string source;
        {
            std::ifstream stream(sourcePath, std::ios::binary);
            source.assign(std::istreambuf_iterator<char>(stream), {});
        }
        const std::string primitive =
            R"({"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0})";
        const auto at = source.find(primitive);
        ASSERT_NE(at, std::string::npos);
        auto ordinary = primitive;
        const std::string material = "\"material\": 0";
        ordinary.replace(ordinary.find(material), material.size(), "\"material\": 1");
        source.replace(at, primitive.size(), primitive + "," + ordinary);
        const std::string node = R"({"name": "folded", "mesh": 0})";
        const auto nodeAt = source.find(node);
        ASSERT_NE(nodeAt, std::string::npos);
        source.replace(nodeAt, node.size(), R"({"name":"folded","mesh":0,"children":[1]},{"name":"shared","mesh":0})");
        {
            std::ofstream stream(sourcePath, std::ios::binary | std::ios::trunc);
            stream << source;
        }
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            Model::PrepareOptions options;
            options.shading = static_cast<ShadingMode>(99);
            EXPECT_THROW(Model::prepareModel(renderer, path, options), Error::Exception);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 0u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 0u);
            const auto legacy = Model::prepareModel(renderer, path);
            EXPECT_EQ(legacy->source(), Model::ModelReference(path));
            options.shading = ShadingMode::Lambert;
            std::string sourcePath = "Models/normals/../normals/FOLDED.gltf";
            const auto lambert = Model::prepareModel(renderer, sourcePath, options);
            sourcePath.clear();
            options.shading = ShadingMode::Pbr;
            const auto pbr = Model::prepareModel(renderer, path, options);
            options.shading = ShadingMode::Unlit;
            EXPECT_EQ(lambert->source().path(), path);
            EXPECT_EQ(lambert->source().options().shading, ShadingMode::Lambert);
            EXPECT_EQ(pbr->source().options().shading, ShadingMode::Pbr);
            released(ResourceType::MODEL, path);
            source_.reset();
            fileIO_.reset();
            for (const auto& prepared : {legacy, lambert, pbr})
            {
                EXPECT_EQ(prepared->meshes().size(), 2u);
                EXPECT_EQ(prepared->materials().size(), 2u);
                EXPECT_EQ(prepared->textures().size(), 1u);
                ASSERT_EQ(prepared->draws().size(), 4u);
                std::array draws{prepared->draws()[0], prepared->draws()[1]};
                draws[0].model = glm::translate(glm::mat4(1), glm::vec3(-0.9f, -0.3f, 0.2f)) *
                                 glm::scale(glm::mat4(1), glm::vec3(0.8f));
                draws[1].model = glm::translate(glm::mat4(1), glm::vec3(0.1f, -0.3f, 0.2f)) *
                                 glm::scale(glm::mat4(1), glm::vec3(0.8f));
                const FrameInput frame{{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws, {{0, 0, -1}, {1, 1, 1}, 0, 0}};
                const auto pixels = TextureGpuReview::captureFrame(renderer, frame);
                ASSERT_EQ(pixels.size(), 384u * 256 * 4);
                for (int channel = 0; channel < 3; ++channel)
                {
                    EXPECT_NEAR(pixels[(115 * 384 + 57) * 4 + channel], 255, 2);
                    EXPECT_NEAR(pixels[(115 * 384 + 249) * 4 + channel], prepared == legacy ? 255 : 0, 2);
                }
                EXPECT_EQ(pixels[(115 * 384 + 57) * 4 + 3], 255);
                EXPECT_EQ(pixels[(115 * 384 + 249) * 4 + 3], 255);
            }
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 6u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 3u);
            EXPECT_EQ(occurrences(messages_.str(), "Created graphics pipeline:"), 4u);
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, SharedModelReusesGeometryTexturesAndPreservesOtherCpuOwners)
    {
        auto& manager = ResourceManager::instance();
        auto& registry = ResourceRegistry::instance();
        manager.registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>());
        auto modelHold = manager.hold<ModelResource>(ResourceType::MODEL, "models/shared.gltf");
        auto textureHold = manager.hold<TextureResource>(ResourceType::TEXTURE, PNG);
        ASSERT_TRUE(modelHold && textureHold);
        const auto modelId = modelHold.id();
        const auto textureId = textureHold.id();
        const auto* cpu = modelHold.get<ModelResource>();
        const auto* pixels = textureHold.get<TextureResource>();
        auto native = window(true);
        std::shared_ptr<const Model::PreparedModel> model;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            model = Model::prepareModel(renderer, "models/shared.gltf");
            EXPECT_EQ(model->meshes().size(), 1u);
            EXPECT_EQ(model->materials().size(), 4u);
            EXPECT_EQ(model->textures().size(), 2u);
            EXPECT_EQ(model->parts().size(), 4u);
            EXPECT_EQ(model->nodes()[1].parts[0], model->nodes()[0].parts[0]);
            EXPECT_FALSE(registry.lookup(ResourceType::TEXTURE, "textures/unused.png").valid());
            EXPECT_EQ(registry.refCount(modelId), 1u);
            EXPECT_EQ(registry.refCount(textureId), 1u);
            EXPECT_EQ(registry.getObject(modelId), cpu);
            EXPECT_EQ(registry.getObject(textureId), pixels);
            modelHold.reset();
            textureHold.reset();
            released(ResourceType::MODEL, "models/shared.gltf");
            released(ResourceType::TEXTURE, PNG);
            for (int frame = 0; frame < 6; ++frame)
            {
                draw(*native, renderer, *model, 2);
                glfwWaitEventsTimeout(0.06);
            }
            const auto image = WindowCapture::capture(*native);
            const auto center = (static_cast<size_t>(image.height / 2) * image.width + image.width / 2) * 4;
            for (int channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(image.rgba[center + channel], 188, 12);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), 1u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 4u);
        }
        EXPECT_FALSE(model->meshes()[0].valid());
        EXPECT_FALSE(model->materials()[0].valid());
        EXPECT_FALSE(model->textures()[0].valid());
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, ExplicitSamplingPreservesSharingMetadataAndDefaultWhileUsingMipsOnGpu)
    {
        class BlackWhiteLoader final : public LoaderBase
        {
        public:
            std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view) override
            {
                return std::make_unique<TextureResource>(
                    id, 2, 2, std::vector<uint8_t>{0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0, 255, 255, 255, 255});
            }
        };
        auto& manager = ResourceManager::instance();
        manager.registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>());
        manager.registerLoader(ResourceType::TEXTURE, std::make_unique<BlackWhiteLoader>());
        auto native = window();
        std::vector<std::shared_ptr<const Model::PreparedModel>> models;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const std::array options{
                Model::PrepareOptions{},
                Model::PrepareOptions{TextureMipMode::FullChain,
                                      {TextureFilter::Linear, TextureFilter::Linear, MipFilter::BaseLevel}},
                Model::PrepareOptions{TextureMipMode::FullChain,
                                      {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear}}};
            for (size_t policy = 0; policy < options.size(); ++policy)
            {
                const auto model = policy == 0 ? Model::prepareModel(renderer, "models/shared.gltf")
                                               : Model::prepareModel(renderer, "models/shared.gltf", options[policy]);
                EXPECT_EQ(model->source(), Model::ModelReference("models/shared.gltf", options[policy]));
                models.push_back(model);
                released(ResourceType::MODEL, "models/shared.gltf");
                released(ResourceType::TEXTURE, PNG);
                ASSERT_TRUE(model->bounds());
                EXPECT_EQ(model->bounds()->min, models[0]->bounds()->min);
                EXPECT_EQ(model->bounds()->max, models[0]->bounds()->max);
                EXPECT_EQ(model->meshes().size(), 1u);
                EXPECT_EQ(model->textures().size(), 2u);
                EXPECT_EQ(model->materials().size(), 4u);
                ASSERT_EQ(model->draws().size(), 5u);
                if (policy == 2)
                {
                    const FrameExtent extent{1, 1};
                    const auto transform = glm::translate(glm::mat4(1), glm::vec3(-0.4f, 0, 0.5f));
                    const std::array draws{DrawItem{model->meshes()[0], model->materials()[0], transform}};
                    std::vector<uint8_t> pixels;
                    Detail::RendererReviewAccess::withFrame(
                        renderer, FrameInput{extent, {glm::mat4(1), glm::mat4(1)}, draws},
                        [&](const auto& device, const auto& pipeline, auto frameSet, auto nativeDraws) {
                            pixels = TextureGpuReview::captureDraws(
                                device, pipeline, frameSet, nativeDraws,
                                {0, 0, float(extent.width), float(extent.height), 0, 1});
                        });
                    for (int channel = 0; channel < 3; ++channel)
                        EXPECT_NEAR(pixels[channel], 188, 2);
                    EXPECT_EQ(pixels[3], 255);
                }
            }
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::TEXTURE, "textures/unused.png").valid());
            EXPECT_EQ(occurrences(messages_.str(), "2x2, format 43, 16 bytes, 2 mips"), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "2x2, format 43, 16 bytes, 1 mips"), 1u);
            EXPECT_EQ(occurrences(messages_.str(), "1x1, format 43, 4 bytes, 1 mips"), 3u);
        }
        for (const auto& model : models)
            EXPECT_FALSE(model->materials()[0].valid());
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, MissingInvalidPngAndMaterialFailureRecoverCleanly)
    {
        auto& manager = ResourceManager::instance();
        auto& registry = ResourceRegistry::instance();
        auto modelHold = manager.hold<ModelResource>(ResourceType::MODEL, GLTF);
        auto otherHold = manager.hold<TextureResource>(ResourceType::TEXTURE, "textures/reference_rgba.png");
        ASSERT_TRUE(modelHold && otherHold);
        const auto modelId = modelHold.id();
        const auto otherId = otherHold.id();
        auto native = window();
        std::shared_ptr<const Model::PreparedModel> recovered;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            // 未准备材质着色器时在纹理上传后失败并回滚资源，补齐着色器后可恢复
            EXPECT_THROW(Model::prepareModel(renderer, GLTF), Error::Exception);
            EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).textures, 0u);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(occurrences(messages_.str(), "Created material:"), 0u);
            released(ResourceType::TEXTURE, PNG);

            shaders(renderer);
            recovered = Model::prepareModel(renderer, GLTF);
            for (int frame = 0; frame < 3; ++frame)
                draw(*native, renderer, *recovered);

            // 缺失/损坏 PNG 应在创建任何 GPU 对象前失败，同时保留外部已有 CPU 持有
            fs::remove(fs::path("assets") / PNG);
            for (bool corrupt : {false, true})
            {
                if (corrupt)
                    std::ofstream(fs::path("assets") / PNG) << "invalid PNG";
                try
                {
                    Model::prepareModel(renderer, GLTF);
                    FAIL() << "Accepted failed dependency";
                }
                catch (const Error::Exception& error)
                {
                    EXPECT_EQ(error.code(), Error::Code::RENDER);
                    EXPECT_NE(error.message().find(corrupt ? "Not a PNG" : "PNG file was not found"),
                              std::string::npos);
                }
                EXPECT_EQ(registry.refCount(modelId), 1u);
                EXPECT_EQ(registry.refCount(otherId), 1u);
            }
        }
        EXPECT_FALSE(recovered->meshes()[0].valid());
        modelHold.reset();
        otherHold.reset();
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, UnusedPartsAndInvalidOptionsCreateNoGpuResources)
    {
        ResourceManager::instance().registerLoader(ResourceType::MODEL, std::make_unique<SharedModelLoader>(true));
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            Model::PrepareOptions invalid;
            invalid.mipMode = static_cast<TextureMipMode>(99);
            EXPECT_THROW(Model::prepareModel(renderer, "models/empty.gltf", invalid), Error::Exception);
            invalid = {};
            invalid.sampler.magFilter = static_cast<TextureFilter>(99);
            EXPECT_THROW(Model::prepareModel(renderer, "models/empty.gltf", invalid), Error::Exception);
            EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::MODEL, "models/empty.gltf").valid());
            auto model = Model::prepareModel(renderer, "models/empty.gltf");
            EXPECT_TRUE(model->meshes().empty() && model->materials().empty() && model->draws().empty());
            EXPECT_FALSE(model->bounds());
            Scene::Scene scene;
            scene.createEntity(model);
            EXPECT_TRUE(Scene::extractSceneFrame(scene, {400, 400})->draws.empty());
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded"), 0u);
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, GlbImportsUploadsEmbeddedTexturesAndReloadsChangedDiskModel)
    {
        const auto external = sandbox_ / "external.glb";
        fs::copy_file("assets/models/embedded_textures.glb", external);
        const auto imported = importExternalModel(*source_, external);
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            auto& resources = ResourceManager::instance();
            resources.reset();
            resources.registerLoader(ResourceType::MODEL, std::make_unique<ModelLoader>(*source_));
            Model::ModelManager cache(renderer);
            const Model::ModelReference unlit(imported.virtualPath);
            const Model::ModelReference pbr(imported.virtualPath, {TextureMipMode::FullChain, {}, ShadingMode::Pbr});
            const auto oldUnlit = cache.resolve(unlit);
            const auto oldPbr = cache.resolve(pbr);
            ASSERT_EQ(oldUnlit->textures().size(), 2u);
            ASSERT_EQ(oldPbr->textures().size(), 4u);
            EXPECT_TRUE(oldPbr->materialSlots().front().sourceDesc.normalTexture.valid());
            EXPECT_TRUE(oldPbr->materialSlots().front().sourceDesc.metallicRoughnessTexture.valid());
            EXPECT_TRUE(oldPbr->materialSlots().front().sourceDesc.emissiveTexture.valid());
            const auto log = messages_.str();
            EXPECT_NE(log.find("3x4, format 43"), std::string::npos);
            EXPECT_NE(log.find("3x4, format 37"), std::string::npos);
            EXPECT_NE(log.find("3x2, format 37"), std::string::npos);
            released(ResourceType::MODEL, imported.virtualPath);

            const auto pixels = [&](const Model::PreparedModel& model) {
                auto draw = model.draws().front();
                draw.model = glm::translate(glm::mat4(1), glm::vec3(0.5f, 0, 0.2f));
                const std::array draws{draw};
                return TextureGpuReview::captureFrame(renderer, {{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws});
            };
            const auto before = pixels(*oldUnlit);
            ASSERT_EQ(before.size(), 384u * 256u * 4u);

            const auto diskPath = source_->resolve(imported.virtualPath);
            std::ifstream input(diskPath, std::ios::binary);
            std::string bytes{std::istreambuf_iterator<char>(input), {}};
            input.close();
            for (const auto& [from, to] : {std::pair<std::string_view, std::string_view>{
                                              R"("baseColorTexture":{"index":0})",
                                              R"("baseColorTexture":{"index":1})"},
                                          {"0.25,0,0,1", "0.75,0,0,1"}})
            {
                const auto offset = bytes.find(from);
                ASSERT_NE(offset, std::string::npos);
                bytes.replace(offset, from.size(), to);
            }
            std::ofstream(diskPath, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            const auto reloaded = cache.reloadByPath(imported.virtualPath);
            ASSERT_EQ(reloaded.size(), 2u);
            EXPECT_EQ(cache.size(), 2u);
            const auto current = cache.resolve(unlit);
            EXPECT_NE(current, oldUnlit);
            EXPECT_NE(cache.resolve(pbr), oldPbr);
            EXPECT_NE(current->draws().front().model, oldUnlit->draws().front().model);
            const auto after = pixels(*current);
            ASSERT_EQ(after.size(), before.size());
            EXPECT_NE(after, before);
            EXPECT_EQ(pixels(*oldUnlit), before);
            released(ResourceType::MODEL, imported.virtualPath);
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, PreservesAlphaDoubleSidedAndEmissiveThroughMaterialOverride)
    {
        constexpr auto path = "assets/models/minimal_gltf/model.gltf";
        std::ifstream input(path, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(input), {}};
        input.close();
        constexpr std::string_view original = R"("alphaMode": "OPAQUE",)";
        const auto mode = text.find(original);
        ASSERT_NE(mode, std::string::npos);
        text.replace(mode, original.size(), R"("alphaMode":"MASK","alphaCutoff":0.625,"doubleSided":true,
                       "emissiveFactor":[0.125,0.25,0.5],"emissiveTexture":{"index":0},)");
        const auto baseColor = text.find("\"baseColorFactor\"");
        ASSERT_NE(baseColor, std::string::npos);
        const auto close = text.find(']', baseColor);
        ASSERT_NE(close, std::string::npos);
        text.replace(baseColor, close - baseColor + 1, R"("baseColorFactor":[1,1,1,0.375])");
        std::ofstream(path, std::ios::binary) << text;

        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto model = Model::prepareModel(renderer, GLTF,
                                                   {TextureMipMode::FullChain,
                                                    {TextureFilter::Linear, TextureFilter::Linear, MipFilter::Linear},
                                                    ShadingMode::Pbr});
            ASSERT_EQ(model->textures().size(), 2u);
            ASSERT_FALSE(model->materialSlots().empty());
            const auto& slot = model->materialSlots().front();
            const auto checkMetadata = [](const MaterialDesc& desc) {
                EXPECT_EQ(desc.alphaMode, Render::AlphaMode::Mask);
                EXPECT_FLOAT_EQ(desc.alphaCutoff, 0.625f);
                EXPECT_FLOAT_EQ(desc.baseAlpha, 0.375f);
                EXPECT_TRUE(desc.doubleSided);
                EXPECT_EQ(desc.emissiveFactor, (std::array<float, 3>{0.125f, 0.25f, 0.5f}));
                EXPECT_TRUE(desc.emissiveTexture.valid());
            };
            checkMetadata(slot.sourceDesc);
            const auto edited = Model::prepareMaterialOverride(renderer, *model, slot.sourceMaterialIndex,
                                                               {0.75f, 0.5f, 0.25f},
                                                               MaterialParameterMode::FixedValue, 0.25f,
                                                               MaterialParameterMode::TextureFactor, 0.75f);
            const auto desc = Detail::RendererReviewAccess::materialDescription(edited->material);
            checkMetadata(desc);
            EXPECT_EQ(desc.color, (std::array<float, 3>{0.75f, 0.5f, 0.25f}));
            EXPECT_FLOAT_EQ(desc.roughness, 0.25f);
            EXPECT_FLOAT_EQ(desc.metallic, 0.75f);
            EXPECT_EQ(desc.roughnessMode, MaterialParameterMode::FixedValue);
            EXPECT_EQ(desc.metallicMode, MaterialParameterMode::TextureFactor);

            auto bad = desc;
            bad.alphaMode = static_cast<Render::AlphaMode>(99);
            EXPECT_THROW(renderer.createMaterial(bad), Error::Exception);
            bad = desc;
            bad.alphaCutoff = -1.0f;
            EXPECT_THROW(renderer.createMaterial(bad), Error::Exception);
            bad = desc;
            bad.baseAlpha = 1.1f;
            EXPECT_THROW(renderer.createMaterial(bad), Error::Exception);
            bad = desc;
            bad.emissiveFactor[0] = std::numeric_limits<float>::quiet_NaN();
            EXPECT_THROW(renderer.createMaterial(bad), Error::Exception);
            bad = desc;
            const TextureResource linear({}, 1, 1, {255, 255, 255, 255});
            bad.emissiveTexture = renderer.createTexture(linear, TextureMipMode::BaseLevel, TextureColorSpace::Linear);
            EXPECT_THROW(renderer.createMaterial(bad), Error::Exception);
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, ReloadReadsChangedTextureAndCommitsAllVariantsTogether)
    {
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            Model::ModelManager cache(renderer);
            const Model::ModelReference unlit(GLTF);
            const Model::ModelReference pbr(GLTF, {{}, {}, ShadingMode::Pbr});
            const auto oldUnlit = cache.resolve(unlit);
            const auto oldPbr = cache.resolve(pbr);
            const auto pixels = [&](const Model::PreparedModel& model) {
                auto draw = model.draws().front();
                draw.model = glm::translate(glm::mat4(1), glm::vec3(0.5f, 0, 0.2f));
                const std::array draws{draw};
                return TextureGpuReview::captureFrame(
                    renderer, {{384, 256}, {glm::mat4(1), glm::mat4(1)}, draws});
            };
            const auto initialPixels = pixels(*oldUnlit);
            ASSERT_EQ(initialPixels.size(), 384u * 256u * 4u);

            auto& resources = ResourceManager::instance();
            {
                const auto held = resources.hold<ModelResource>(ResourceType::MODEL, GLTF);
                ASSERT_TRUE(held);
                EXPECT_THROW(cache.reload(unlit), Model::LoadError);
                EXPECT_THROW(cache.reloadByPath(GLTF), Model::LoadError);
            }
            {
                const auto held = resources.hold<TextureResource>(ResourceType::TEXTURE, PNG);
                ASSERT_TRUE(held);
                EXPECT_THROW(cache.reloadByPath(GLTF), Model::LoadError);
            }
            fs::copy_file("assets/textures/reference_rgba.png", fs::path("assets") / PNG,
                          fs::copy_options::overwrite_existing);
            EXPECT_THROW(cache.reloadByPath(GLTF, [](std::span<const Model::ReloadedModel> variants) {
                EXPECT_EQ(variants.size(), 2u);
                throw Error::Exception(Error::Code::RENDER, "Rejected prepared variants");
            }), Error::Exception);
            EXPECT_EQ(cache.resolve(unlit), oldUnlit);
            EXPECT_EQ(cache.resolve(pbr), oldPbr);

            const auto changed = cache.reloadByPath("Models/minimal_gltf/./MODEL.GLTF");
            ASSERT_EQ(changed.size(), 2u);
            EXPECT_EQ(cache.size(), 2u);
            EXPECT_EQ(changed[0].previous, oldUnlit);
            EXPECT_EQ(changed[1].previous, oldPbr);
            const auto newUnlit = cache.resolve(unlit);
            const auto newPbr = cache.resolve(pbr);
            EXPECT_EQ(changed[0].current, newUnlit);
            EXPECT_EQ(changed[1].current, newPbr);
            EXPECT_NE(newUnlit, oldUnlit);
            EXPECT_NE(newPbr, oldPbr);
            const auto reloadedPixels = pixels(*newUnlit);
            ASSERT_EQ(reloadedPixels.size(), initialPixels.size());
            EXPECT_NE(reloadedPixels, initialPixels);
            EXPECT_EQ(pixels(*oldUnlit), initialPixels);

            std::ofstream(fs::path("assets") / GLTF) << "{";
            EXPECT_THROW(cache.reloadByPath(GLTF), Model::LoadError);
            EXPECT_EQ(cache.resolve(unlit), newUnlit);
            EXPECT_EQ(cache.resolve(pbr), newPbr);
            EXPECT_EQ(cache.size(), 2u);
            EXPECT_EQ(pixels(*newUnlit), reloadedPixels);
            EXPECT_TRUE(cache.reloadByPath(OBJ).empty());
            const Model::ModelReference obj(OBJ);
            const auto inserted = cache.reload(obj);
            EXPECT_EQ(inserted, cache.resolve(obj));
            EXPECT_EQ(cache.size(), 3u);
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, SceneRebuildReusesSessionModelsAndPreservesFailures)
    {
        using Model::ModelManager;
        auto native = window();
        std::weak_ptr<const Model::PreparedModel> firstSession;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            ModelManager cache(renderer);
            const Model::ModelReference gltf("models/minimal_gltf/model.gltf");
            const Model::ModelReference obj("models/minimal_obj/model.obj");
            const auto first = cache.resolve(gltf);
            firstSession = first;
            EXPECT_EQ(first, cache.resolve(Model::ModelReference("MODELS/minimal_gltf/../minimal_gltf/MODEL.gltf")));
            const auto second = cache.resolve(obj);
            EXPECT_EQ(cache.size(), 2u);
            Scene::SceneDescription description;
            description.entities = {{gltf, Scene::Transform({1, 2, 3}, {1, 0, 0, 0}, {-2, 0, 3}), false},
                                    {obj, Scene::Transform({-1, 0, 0}), true},
                                    {gltf, Scene::Transform({4, 0, 0}), true},
                                    {std::nullopt, Scene::Transform{}, true}};
            auto resolver = [&](const Model::ModelReference& ref) { return cache.resolve(ref); };
            const auto uploads = occurrences(messages_.str(), "Uploaded mesh:");
            auto built = Scene::rebuildScene(description, resolver);
            ASSERT_EQ(built.scene->entities().size(), 4u);
            EXPECT_EQ(built.scene->entities()[0].model, first);
            EXPECT_EQ(built.scene->entities()[1].model, second);
            EXPECT_EQ(built.scene->entities()[2].model, first);
            EXPECT_EQ(built.scene->entities()[3].model, nullptr);
            EXPECT_FALSE(built.scene->entities()[0].visible);
            EXPECT_EQ(built.scene->entities()[0].transform.matrix(), description.entities[0].transform.matrix());
            const auto captured = Scene::captureScene(*built.scene, built.clearColor);
            EXPECT_EQ(captured.entities[0].model, description.entities[0].model);
            EXPECT_EQ(captured.entities[1].model, description.entities[1].model);
            fs::create_directories(sandbox_ / "outside");
            const auto scenePath =
                Utils::Utf8::toUtf8String(sandbox_ / "outside" / Utils::Utf8::toFsPath("场景😀.json"));
            Scene::writeScene(*fileIO_, scenePath, captured);
            EXPECT_EQ(Scene::rebuildScene(Scene::readScene(*fileIO_, scenePath), resolver).scene->entities()[0].model,
                      first);
            EXPECT_EQ(Scene::rebuildScene(captured, resolver).scene->entities()[0].model, first);
            EXPECT_EQ(occurrences(messages_.str(), "Uploaded mesh:"), uploads);
            const auto oldId = built.scene->entities()[0].id;
            EXPECT_EQ(occurrences(messages_.str(), "Prepared model '"), 2u);
            auto missing = description;
            missing.entities[1].model = Model::ModelReference("models/missing.gltf");
            EXPECT_THROW(Scene::rebuildScene(missing, resolver), Error::Exception);
            EXPECT_NE(built.scene->find(oldId), nullptr);
            EXPECT_EQ(Scene::rebuildScene(description, resolver).scene->entities()[0].model, first);

            const auto sourcePath = fs::path("assets") / gltf.path();
            std::ifstream input(sourcePath, std::ios::binary);
            const std::string sourceText{std::istreambuf_iterator<char>(input), {}};
            input.close();
            std::ofstream(sourcePath) << "{";
            Model::PrepareOptions parseOptions;
            parseOptions.shading = ShadingMode::Lambert;
            EXPECT_THROW(cache.resolve(Model::ModelReference(gltf.path(), parseOptions)), Model::LoadError);
            EXPECT_EQ(cache.size(), 2u);
            draw(*native, renderer, *first);
            std::ofstream(sourcePath) << sourceText;

            const auto texturePath = fs::path("assets") / PNG;
            fs::remove(texturePath);
            Model::PrepareOptions missingTextureOptions;
            missingTextureOptions.sampler.mipFilter = MipFilter::Linear;
            auto missingTexture = description;
            missingTexture.entities[0].model = Model::ModelReference(gltf.path(), missingTextureOptions);
            EXPECT_THROW(Scene::rebuildScene(missingTexture, resolver), Error::Exception);
            EXPECT_NE(built.scene->find(oldId), nullptr);
            fs::copy_file(Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR) / PNG, texturePath);
            EXPECT_NE(cache.resolve(*missingTexture.entities[0].model), first);

            for (const auto& variant : {Model::PrepareOptions{TextureMipMode::FullChain},
                                        Model::PrepareOptions{{}, {TextureFilter::Linear}},
                                        Model::PrepareOptions{{}, {TextureFilter::Nearest, TextureFilter::Linear}},
                                        Model::PrepareOptions{{}, {TextureFilter::Nearest, TextureFilter::Nearest, MipFilter::Nearest}},
                                        Model::PrepareOptions{{}, {}, ShadingMode::Lambert}})
                EXPECT_NE(cache.resolve(Model::ModelReference(gltf.path(), variant)), first);
            released(ResourceType::MODEL, gltf.path());
            EXPECT_EQ(cache.size(), 8u);
            EXPECT_EQ(cache.collectUnused(), 6u);
            EXPECT_EQ(cache.size(), 2u);
            EXPECT_EQ(occurrences(resourceMessages_.str(), "load:"), 3u);
            draw(*native, renderer, *first);
            EXPECT_TRUE(ResourceManager::instance().hasLoader(ResourceType::MODEL));
        }
        EXPECT_TRUE(firstSession.expired());
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, ModelManagerCollectsAfterInFlightFramesAndHandlesShutdownOrder)
    {
        auto native = window();
        std::unique_ptr<Model::ModelManager> lateCache;
        std::shared_ptr<const Model::PreparedModel> external;
        Mesh lateMesh;
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            shadowShader(renderer);
            Model::ModelManager models(renderer);
            auto model = models.resolve(Model::ModelReference(GLTF));
            const auto mesh = model->meshes()[0];
            std::weak_ptr<const Model::PreparedModel> weak = model;
            Scene::Scene scene;
            scene.createEntity(model);
            scene.createEntity(model, Scene::Transform({1000, 0, 0}), false);  // 隐藏实体但仍可作阴影投射示例
            const auto casterOnly = scene.createEntity(model, Scene::Transform({1000, 0, 0}), true);
            (void)casterOnly;
            model.reset();
            EXPECT_EQ(models.collectUnused(), 0u);
            ShadowSettings settings;
            settings.enabled = true;
            const auto frame = Scene::extractSceneFrame(scene, {384, 256}, true, settings);
            ASSERT_TRUE(frame && !frame->shadowDraws.empty());
            SceneOutput output;
            for (int slot = 0; slot != 2; ++slot)
            {
                ASSERT_TRUE(renderer.beginUiFrame());
                output = renderer.prepareSceneTarget(frame->extent);
                renderer.imgui().endFrame();
                renderer.drawSceneTarget(output, frame->input(), {0.2f, 0.24f, 0.28f, 1});
            }
            scene.clear();
            EXPECT_EQ(models.collectUnused(), 1u);
            EXPECT_TRUE(weak.expired());
            EXPECT_TRUE(mesh.valid());  // 飞行帧槽仍保护 GPU 资源
            renderer.prepareSceneTarget(frame->extent);
            EXPECT_TRUE(mesh.valid());
            Detail::RendererReviewAccess::withSceneOutput(renderer, output, [&](const auto& device, const auto& image) {
                EXPECT_EQ(TextureGpuReview::captureMip(device, image, 0, {384, 256}).size(), 384u * 256u * 4u);
            });
            EXPECT_FALSE(mesh.valid());
            EXPECT_EQ(Detail::RendererReviewAccess::resourceCounts(renderer).meshes, 0u);

            // 缓存早于外部使用者析构，以及 Renderer 早于缓存析构均安全
            auto transient = std::make_unique<Model::ModelManager>(renderer);
            external = transient->resolve(Model::ModelReference(GLTF));
            transient.reset();
            EXPECT_TRUE(external->meshes()[0].valid());
            lateCache = std::make_unique<Model::ModelManager>(renderer);
            lateMesh = lateCache->resolve(Model::ModelReference(OBJ))->meshes()[0];
        }
        EXPECT_FALSE(external->meshes()[0].valid());
        EXPECT_FALSE(lateMesh.valid());
        external.reset();
        EXPECT_EQ(lateCache->collectUnused(), 1u);
        lateCache.reset();
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, UnavailablePartBoundsInvalidateWholeModelWithoutDiscardingDraws)
    {
        glm::mat4 projective(1.0f), overflow(1.0f);
        projective[0][3] = 0.5f;
        overflow[0][0] = overflow[3][0] = std::numeric_limits<float>::max();
        auto native = window();
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            for (const auto& matrix : {projective, overflow})
            {
                ResourceManager::instance().registerLoader(ResourceType::MODEL,
                                                           std::make_unique<TransformModelLoader>(matrix));
                const auto model = Model::prepareModel(renderer, "models/unavailable-bounds.gltf");
                EXPECT_FALSE(model->bounds());
                ASSERT_EQ(model->draws().size(), 5u);
                Scene::Scene scene;
                scene.createEntity(model, Scene::Transform({1000, 0, -10}));
                EXPECT_FALSE(scene.pick(Math::LineSegment{{1000, 0, 0}, {1000, 0, -20}}));
                EXPECT_EQ(Scene::extractSceneFrame(scene, {400, 400})->draws.size(), 5u);
                EXPECT_EQ(model->draws().back().model, glm::translate(glm::mat4(1), glm::vec3(9, 0, 0)));
            }
        }
        cleanValidation();
    }

    TEST_F(ModelPreparationLifecycleTest, ImportedSnapshotSubmitsAfterSceneClear)
    {
        auto native = window(true);
        Scene::Scene scene;
        scene.setCamera(Scene::Camera({0, 0, 1}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            shaders(renderer);
            const auto gltf = Model::prepareModel(renderer, GLTF);
            const auto obj = Model::prepareModel(renderer, OBJ);
            scene.createEntity(gltf, Scene::Transform({-0.65f, 0.45f, 0}, {1, 0, 0, 0}, glm::vec3(0.25f)));
            scene.createEntity(obj, Scene::Transform({0.65f, -0.55f, 0}, {1, 0, 0, 0}, glm::vec3(0.35f)));
            scene.createEntity();
            released(ResourceType::MODEL, GLTF);
            released(ResourceType::MODEL, OBJ);
            released(ResourceType::TEXTURE, PNG);
            native->pollEvents();
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            auto retained = Scene::extractSceneFrame(scene, *extent);
            ASSERT_TRUE(retained && retained->draws.size() == 6u);
            scene.clear();
            renderer.imgui().endFrame();
            renderer.drawFrame(retained->input(), {0.2f, 0.24f, 0.28f, 1});
            retained.reset();
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            const auto image = WindowCapture::capture(*native);
            for (const auto& pt : {std::array{-0.510897f, 0.567851f}, std::array{0.86f, -0.608333f}})
            {
                const int px = image.width / 2 + int(pt[0] * image.height / 2);
                const int py = image.height / 2 - int(pt[1] * image.height / 2);
                EXPECT_NEAR(image.rgba[(size_t(py) * image.width + px) * 4 + 0], 188, 12);
                EXPECT_NEAR(image.rgba[(size_t(py) * image.width + px) * 4 + 1], 255, 12);
                EXPECT_NEAR(image.rgba[(size_t(py) * image.width + px) * 4 + 2], 188, 12);
            }
        }
        cleanValidation();
    }
}  // namespace
