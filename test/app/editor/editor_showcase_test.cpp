#include "editor_test_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Test;
    namespace fs = std::filesystem;
    namespace NativeIO = Incubator::FileIO;

    class EditorShowcaseTest : public EditorAppTest
    {};


    TEST_F(EditorShowcaseTest, BuiltinPbrShowcaseOpensViaFileBrowserMatchesPbrBaselineAndExportsComparison)
    {
        // 验证构建部署产物就位，且默认启动配置仍指向阴影场景。
        ASSERT_TRUE(fs::exists(previous_ / "assets/scenes/pbr-showcase.json"));
        ASSERT_TRUE(fs::exists(previous_ / "incubator.json"));
        {
            auto buildIo = NativeIO::createPlatformFileIO();
            const auto startupConfig =
                App::readStartupConfig(*buildIo, Utils::Utf8::toUtf8String(previous_ / "incubator.json"));
            EXPECT_EQ(startupConfig.startupScene, "assets/scenes/shadows.json");
        }

        copyShowcaseModels();
        {
            Editor::EditorApp session(options(true, 1280, 800));
            Editor::EditorScene editor;
            int frame = 0;
            int step = 0;
            int settleFrames = 0;
            bool done = false;
            Render::FrameExtent captureExtent{};
            std::vector<uint8_t> pbrPixels;
            std::vector<uint8_t> lambertPixels;
            auto* handle = static_cast<GLFWwindow*>(session.window().getNativeHandle());
            enableGlfwCallbacks(handle, false);

            const auto reviewDir = previous_ / "review";
            fs::create_directories(reviewDir);

            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/shadows.json");
                     app.shadowSettings().enabled = true;
                 },
                 [&](auto& app, const auto&) {
                     if (++frame > 240)
                     {
                         ADD_FAILURE() << "PBR showcase test timeout at step=" << step;
                         app.requestExit();
                         return;
                     }
                     auto* toolbarWin = ImGui::FindWindowByName("##toolbar");
                     if (frame < 6 || !toolbarWin || !app.sceneOutput().valid())
                         return;

                     switch (step)
                     {
                         case 0:
                             // 通过工具栏 Open... 触发与 File -> Open 相同的场景文件列表，验证三个内置场景均可见。
                             ImGui::ActivateItemByID(toolbarWin->GetID("Open..."));
                             ++step;
                             break;
                         case 1:
                             if (auto* openWin = ImGui::FindWindowByName("Open scene");
                                 openWin && !openWin->DC.ChildWindows.empty())
                             {
                                 const auto& files = editor.controller().sceneFiles();
                                 const auto hasScene = [&](std::string_view target) {
                                     return std::find(files.begin(), files.end(), target) != files.end();
                                 };
                                 EXPECT_TRUE(hasScene("assets/scenes/mixed.json"));
                                 EXPECT_TRUE(hasScene("assets/scenes/shadows.json"));
                                 EXPECT_TRUE(hasScene("assets/scenes/pbr-showcase.json"));
                                 auto* filesChild = openWin->DC.ChildWindows[0];
                                 ImGui::ActivateItemByID(filesChild->GetID("assets/scenes/pbr-showcase.json"));
                                 ++step;
                             }
                             break;
                         case 2:
                             EXPECT_EQ(std::string(editor.controller().pathBufferData().data()),
                                       "assets/scenes/pbr-showcase.json");
                             ASSERT_TRUE(editor.confirmFileAction(app));
                             // 验证原有 mixed.json 与 shadows.json 仍可正常打开，再切回 pbr-showcase.json 检查存储值。
                             ASSERT_TRUE(editor.load(app, "assets/scenes/mixed.json"));
                             EXPECT_EQ(app.scene().entities().size(), 2u);
                             ASSERT_TRUE(editor.load(app, "assets/scenes/shadows.json"));
                             EXPECT_EQ(app.scene().entities().size(), 3u);
                             ASSERT_TRUE(editor.load(app, "assets/scenes/pbr-showcase.json"));

                             ASSERT_EQ(app.scene().entities().size(), 3u);
                             EXPECT_EQ(app.scene().entities()[0].name, "Avocado Cut");
                             EXPECT_EQ(app.scene().entities()[1].name, "Avocado Peel");
                             EXPECT_EQ(app.scene().entities()[2].name, "Ground");
                             EXPECT_EQ(app.scene().entities()[0].model, app.scene().entities()[1].model);
                             for (const auto& entity : app.scene().entities())
                             {
                                 EXPECT_FALSE(entity.parent.valid());
                                 ASSERT_NE(entity.model, nullptr);
                                 EXPECT_EQ(entity.model->source().options().shading, Render::ShadingMode::Pbr);
                                 EXPECT_EQ(entity.model->source().options().mipMode, Render::TextureMipMode::FullChain);
                                 EXPECT_EQ(entity.model->source().options().sampler.minFilter,
                                           Render::TextureFilter::Linear);
                                 EXPECT_EQ(entity.model->source().options().sampler.magFilter,
                                           Render::TextureFilter::Linear);
                                 EXPECT_EQ(entity.model->source().options().sampler.mipFilter,
                                           Render::MipFilter::Linear);
                             }
                             EXPECT_FLOAT_EQ(app.scene().camera().position().x, 0.2f);
                             EXPECT_FLOAT_EQ(app.scene().camera().position().y, 0.62f);
                             EXPECT_FLOAT_EQ(app.scene().camera().position().z, 3.45f);
                             EXPECT_FLOAT_EQ(app.scene().lighting().intensity, 1.55f);
                             EXPECT_FLOAT_EQ(app.scene().lighting().ambient, 0.14f);
                             EXPECT_EQ(app.scene().display().toneMapping, Render::ToneMappingMode::Reinhard);
                             EXPECT_FLOAT_EQ(app.scene().display().exposureEv, 0.15f);
                             EXPECT_FLOAT_EQ(app.clearColor()[0], 0.08f);
                             EXPECT_TRUE(app.shadowSettings().enabled);
                             // 取消选中以获得无 Gizmo/包围盒干扰的纯净展示画面。
                             editor.select(app, Scene::EntityId{});
                             settleFrames = 0;
                             ++step;
                             break;
                         case 3:
                             if (++settleFrames < 2)
                                 return;
                             // 同机位、光照、曝光与视口尺寸记录 PBR 画面与最终展示图。
                             captureExtent = app.sceneOutput().extent();
                             ASSERT_GT(captureExtent.width, 0u);
                             ASSERT_GT(captureExtent.height, 0u);
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& device, const auto& image) {
                                     pbrPixels = TextureGpuReview::captureMip(
                                         device, image, 0, {captureExtent.width, captureExtent.height});
                                 });
                             ASSERT_FALSE(pbrPixels.empty());
                             WindowCapture::save(
                                 {int(captureExtent.width), int(captureExtent.height), pbrPixels},
                                 reviewDir / "pbr-showcase-pbr.ppm");
                             WindowCapture::save(
                                 {int(captureExtent.width), int(captureExtent.height), pbrPixels},
                                 reviewDir / "pbr-showcase-final.ppm");

                             // 保持同一相机、灯光、曝光、阴影与视口尺寸，仅切换主体与地面为 Lambert 记录对照。
                             for (const auto id : std::array{app.scene().entities()[0].id, app.scene().entities()[1].id,
                                                            app.scene().entities()[2].id})
                             {
                                 editor.select(app, id);
                                 ASSERT_TRUE(editor.setEntityShading(app, Render::ShadingMode::Lambert));
                             }
                             editor.select(app, Scene::EntityId{});
                             settleFrames = 0;
                             ++step;
                             break;
                         case 4:
                             if (++settleFrames < 2)
                                 return;
                             EXPECT_EQ(app.sceneOutput().extent(), captureExtent);
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& device, const auto& image) {
                                     lambertPixels = TextureGpuReview::captureMip(
                                         device, image, 0, {captureExtent.width, captureExtent.height});
                                 });
                             ASSERT_EQ(lambertPixels.size(), pbrPixels.size());
                             EXPECT_NE(lambertPixels, pbrPixels);
                             WindowCapture::save(
                                 {int(captureExtent.width), int(captureExtent.height), lambertPixels},
                                 reviewDir / "pbr-showcase-lambert.ppm");
                             done = true;
                             app.requestExit();
                             break;
                     }
                 },
                 [&](auto& app) { editor.ui(app); },
                 [&](auto& app) { editor.cancelInteraction(app); },
                 [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
                 [&](auto& app) { editor.layout(app); },
                 [&](auto& app) { editor.requestClose(app); }});
            EXPECT_TRUE(done);
        }
        cleanValidation();
    }

    TEST_F(EditorShowcaseTest, LightingShowcaseOpensWithCsmLocalLightsSkyboxAcesBloomAndExportsPngUnderValidation)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(false, 1280, 800));
            Editor::EditorScene editor;
            int step = 0;
            bool done = false;
            const fs::path pngPath = "lighting-showcase-capture.png";
            fs::remove(pngPath);

            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/lighting-showcase.json");
                     app.shadowSettings().enabled = true;
                     app.shadowSettings().cascadeCount = 3u;
                     app.shadowSettings().resolution = 2048u;
                     app.shadowSettings().fitToBounds = true;
                     app.shadowSettings().normalBias = 0.02f;
                     app.iblEnabled() = true;
                     ASSERT_EQ(app.scene().entities().size(), 4u);
                     EXPECT_EQ(app.scene().lighting().localLights.size(), 2u);
                     EXPECT_EQ(app.scene().display().toneMapping, Render::ToneMappingMode::AcesFilmic);
                     EXPECT_TRUE(app.scene().display().skyboxEnabled);
                     EXPECT_TRUE(app.scene().display().bloomEnabled);
                     editor.select(app.scene().entities()[1].id);
                 },
                 [&](auto& app, const auto&) {
                     if (app.sceneExtent().width == 0u || app.sceneExtent().height == 0u)
                         return;
                     if (++step < 2)
                         return;
                     ASSERT_TRUE(editor.exportScreenshot(app, pngPath)) << editor.message();
                     ASSERT_TRUE(fs::exists(pngPath));
                     auto io = FileIO::createPlatformFileIO();
                     Resource::AssetSource cwdSource(*io, fs::current_path());
                     Resource::TextureLoader loader(cwdSource);
                     const auto loaded = loader.load(
                         Resource::ResourceId::fromPath(Resource::ResourceType::TEXTURE, pngPath.string()),
                         pngPath.string());
                     const auto* tex = dynamic_cast<const Resource::TextureResource*>(loaded.get());
                     ASSERT_NE(tex, nullptr);
                     EXPECT_EQ(tex->width(), app.sceneExtent().width);
                     EXPECT_EQ(tex->height(), app.sceneExtent().height);
                     done = true;
                     app.requestExit();
                 },
                 [&](auto& app) { editor.ui(app); },
                 [&](auto& app) { editor.cancelInteraction(app); },
                 [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
                 [&](auto& app) { editor.layout(app); },
                 [&](auto& app) { editor.requestClose(app); }});
            fs::remove(pngPath);
            EXPECT_TRUE(done);
        }
        cleanValidation();
    }
}  // namespace
