#include "editor_test_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Test;
    namespace fs = std::filesystem;
    namespace NativeIO = Incubator::FileIO;

    class EditorRenderWiringTest : public EditorAppTest
    {};


    TEST_F(EditorRenderWiringTest, ShadowPreviewUsesCurrentFrameAndSurvivesFilesAndWindowRecovery)
    {
        copyShowcaseModels();
        Editor::EditorApp session(options(true, 1920, 1080));
        auto demo = std::make_shared<Editor::EditorScene>();
        Render::SceneOutput previous;
        std::array<std::shared_ptr<const Model::PreparedModel>, 3> models;
        WindowCapture::Image on, off, changedLight;
        std::set<VkImage> shadowImages;
        float previewBias = 0;
        int frame = 0, uiCallsWhileMinimized = 0;
        const auto difference = [](const WindowCapture::Image& a, const WindowCapture::Image& b) {
            size_t changed = 0;
            for (size_t i = 0; i + 3 < std::min(a.rgba.size(), b.rgba.size()); i += 4)
                changed += (std::abs(int(a.rgba[i]) - int(b.rgba[i])) > 2 ||
                            std::abs(int(a.rgba[i + 1]) - int(b.rgba[i + 1])) > 2 ||
                            std::abs(int(a.rgba[i + 2]) - int(b.rgba[i + 2])) > 2);
            return changed;
        };
        const auto capture = [&](Editor::EditorApp& app) {
            WindowCapture::Image image;
            const auto ex = previous.extent();
            image.width = int(ex.width);
            image.height = int(ex.height);
            Render::Detail::RendererReviewAccess::withSceneOutput(app.renderer(), previous, [&](const auto& d, const auto& t) {
                image.rgba = TextureGpuReview::captureMip(d, t, 0, {ex.width, ex.height});
            });
            Render::Detail::RendererReviewAccess::withShadowOutput(app.renderer(), previous, [&](const auto&, const auto& t) {
                shadowImages.insert(t.handle());
                EXPECT_TRUE(t.extent().width == 1024u && t.extent().height == 1024u);
            });
            return image;
        };
        session.run({[&](auto& app) {
                         demo->initialize(app, "assets/scenes/shadows.json");
                         ASSERT_EQ(app.scene().entities().size(), 3u);
                         for (size_t i = 0; i < 3; ++i) models[i] = app.scene().entities()[i].model;
                         app.shadowSettings().enabled = true;
                         previewBias = app.shadowSettings().depthBias;
                     },
                     [&](auto& app, const auto&) {
                         ++frame;
                         if (frame == 5) on = capture(app);
                         else if (frame == 6)
                         {
                             off = capture(app);
                             EXPECT_GT(difference(on, off), 500u);
                             size_t darker = 0;
                             for (size_t i = 0; i + 3 < on.rgba.size(); i += 4)
                                 darker += int(off.rgba[i]) - int(on.rgba[i]) > 8;
                             EXPECT_GT(darker, 500u);
                         }
                         else if (frame == 7 || frame == 12) EXPECT_EQ(capture(app).rgba, on.rgba);
                         else if (frame == 8) { changedLight = capture(app); EXPECT_GT(difference(on, changedLight), 500u); }
                         else if (frame == 9) EXPECT_GT(difference(changedLight, capture(app)), 500u);
                         auto* h = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                         if (frame == 13)
                         {
                             glfwIconifyWindow(h);
                             for (int i = 0; i < 100 && !app.window().isIconified(); ++i) glfwWaitEventsTimeout(0.005);
                             ASSERT_TRUE(app.window().isIconified());
                         }
                         else if (frame == 16)
                         {
                             EXPECT_EQ(uiCallsWhileMinimized, 0);
                             glfwRestoreWindow(h);
                             for (int i = 0; i < 100 && app.window().isIconified(); ++i) glfwWaitEventsTimeout(0.005);
                             ASSERT_FALSE(app.window().isIconified());
                             glfwSetWindowSize(h, 1280, 720);
                             demo->restoreLayout();
                         }
                         else if (frame >= 19) { if (frame == 19) { EXPECT_TRUE(previous.valid()); capture(app); } app.requestExit(); }
                     },
                     [&](auto& app) {
                         if (frame >= 13 && frame < 16) ++uiCallsWhileMinimized;
                         demo->ui(app);
                         if (frame == 5) app.shadowSettings().enabled = false;
                         else if (frame == 6) app.shadowSettings().enabled = true;
                         else if (frame == 7) { auto l = app.scene().lighting(); l.direction = {-1, -0.9f, -0.2f}; app.scene().setLighting(l); }
                         else if (frame == 8) { auto pose = app.scene().entities()[0].transform; pose.setPosition(pose.position() + glm::vec3(0.4f, 0, 0.3f)); app.scene().setTransform(app.scene().entities()[0].id, pose); }
                         else if (frame == 9)
                         {
                             app.shadowSettings().depthBias = 0.002f;
                             ASSERT_TRUE(demo->save(app, "shadow-saved.json"));
                             { std::ifstream file("shadow-saved.json"); const std::string json((std::istreambuf_iterator<char>(file)), {}); EXPECT_EQ(json.find("\"shadow"), std::string::npos); }
                             app.shadowSettings().enabled = false; app.shadowSettings().depthBias = 0.003f;
                             ASSERT_TRUE(demo->load(app, "shadow-saved.json"));
                             EXPECT_TRUE(demo->scenePath() == "shadow-saved.json" && !app.shadowSettings().enabled && app.shadowSettings().depthBias == 0.003f);
                         }
                         else if (frame == 10)
                         {
                             ASSERT_TRUE(demo->load(app, "assets/scenes/mixed.json"));
                             ASSERT_EQ(app.scene().entities().size(), 2u);
                             EXPECT_TRUE(app.scene().entities()[0].model == models[0] && app.scene().entities()[1].model == models[1]);
                             ASSERT_TRUE(demo->save(app, "mixed-copy.json") && demo->load(app, "mixed-copy.json"));
                             EXPECT_FALSE(app.shadowSettings().enabled);
                         }
                         else if (frame == 11)
                         {
                             ASSERT_TRUE(demo->load(app, "assets/scenes/shadows.json"));
                             ASSERT_EQ(app.scene().entities().size(), 3u);
                             for (size_t i = 0; i < 3; ++i) EXPECT_EQ(app.scene().entities()[i].model, models[i]);
                             app.shadowSettings().enabled = true; app.shadowSettings().depthBias = previewBias;
                         }
                         previous = app.renderer().prepareSceneTarget(app.sceneExtent());
                         glfwWaitEventsTimeout(0.04);
                     },
                     [demo](auto& app) { demo->cancelInteraction(app); },
                     [demo](auto& app, auto extent, const auto& input) { demo->input(app, extent, input); },
                     [demo](auto& app) { demo->layout(app); }});
        EXPECT_TRUE(shadowImages.size() == 2u && count("Created shadow target:") == 2u);
        cleanValidation();
    }

    TEST_F(EditorRenderWiringTest, EntityShadingComboSwitchPbrUndoRedoFailureCacheAndScreenshotUnderValidation)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(true, 1280, 800));
            Editor::EditorScene editor;
            Scene::EntityId avocadoId{}, teapotId{}, unlitId{};
            int frame = 0, step = 0;
            bool done = false;
            size_t initialCached = 0, afterPbrCached = 0;
            WindowCapture::Image pbrShot{};
            auto* handle = static_cast<GLFWwindow*>(session.window().getNativeHandle());

            session.run(
                {[&](auto& app) {
                     editor.initialize(app);
                     EditorTest::setSharedEditorScene(app, editor, false);
                     avocadoId = editor.selected();
                     // 添加 Utah Teapot 并排展示。
                     editor.selectModel(true);
                     teapotId = editor.createEntity(app);
                     app.scene().setTransform(teapotId,
                                              Scene::Transform({0.25f, 0.0f, 0.0f}, {1, 0, 0, 0}, {0.1f, 0.1f, 0.1f}));
                     // 将 minimal_gltf 的 bin 拷贝到相对位置供 unlit.gltf 引用。
                     std::filesystem::create_directories("assets/models/minimal_gltf/geometry");
                     std::filesystem::copy_file(
                         Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR) / "models/minimal_gltf/geometry/mesh.bin",
                         "assets/models/minimal_gltf/geometry/mesh.bin",
                         std::filesystem::copy_options::overwrite_existing);
                     const auto binSize = std::filesystem::file_size("assets/models/minimal_gltf/geometry/mesh.bin");
                     // 添加自带 KHR_materials_unlit 的测试 glTF，验证资产自带 unlit 材质优先级。
                     const std::string unlitGltf = fmt::format(R"({{
  "asset": {{ "version": "2.0" }},
  "extensionsUsed": ["KHR_materials_unlit"],
  "extensionsRequired": ["KHR_materials_unlit"],
  "buffers": [{{ "uri": "../minimal_gltf/geometry/mesh.bin", "byteLength": {} }}],
  "bufferViews": [
    {{ "buffer": 0, "byteOffset": 0, "byteLength": 48 }},
    {{ "buffer": 0, "byteOffset": 80, "byteLength": 12 }}
  ],
  "accessors": [
    {{ "bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3" }},
    {{ "bufferView": 1, "componentType": 5123, "count": 6, "type": "SCALAR" }}
  ],
  "materials": [{{
    "pbrMetallicRoughness": {{ "baseColorFactor": [0.9, 0.2, 0.2, 1.0] }},
    "extensions": {{ "KHR_materials_unlit": {{}} }}
  }}],
  "meshes": [{{ "primitives": [{{ "attributes": {{ "POSITION": 0 }}, "indices": 1, "material": 0 }}] }}],
  "nodes": [{{ "mesh": 0 }}],
  "scenes": [{{ "nodes": [0] }}],
  "scene": 0
}})", binSize);
                     std::filesystem::create_directories("assets/models/test_unlit");
                     {
                         std::ofstream out("assets/models/test_unlit/unlit.gltf", std::ios::binary);
                         out << unlitGltf;
                     }
                     ASSERT_TRUE(editor.addModel(app, "models/test_unlit/unlit.gltf",
                                                 Scene::Transform({-0.35f, 0.0f, 0.0f}, {1, 0, 0, 0}, {0.2f, 0.2f, 0.2f})));
                     unlitId = editor.selected();
                     editor.select(app, avocadoId);
                     initialCached = editor.cachedModels();
                     enableGlfwCallbacks(handle, false);
                 },
                 [&](auto& app, const auto&) {
                     if (++frame > 180)
                     {
                         ADD_FAILURE() << "EntityShading test timeout at step=" << step;
                         app.requestExit();
                         return;
                     }
                     auto* propWin = ImGui::FindWindowByName("Properties");
                     if (frame < 5 || !propWin || app.sceneExtent().width == 0)
                         return;

                     switch (step)
                     {
                         case 0:
                             // 实际激活 Properties 面板中的 ##entity-shading 下拉框。
                             ImGui::ActivateItemByID(propWin->GetID("##entity-shading"));
                             ++step;
                             break;
                         case 1:
                             if (!ImGui::GetCurrentContext()->OpenPopupStack.empty())
                             {
                                 auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
                                 if (popup)
                                 {
                                     ImGui::ActivateItemByID(ImHashStr("PBR", 0, popup->GetID(2)));
                                     ++step;
                                 }
                             }
                             break;
                         case 2:
                             ASSERT_EQ(app.scene().find(avocadoId)->model->source().options().shading,
                                       Render::ShadingMode::Pbr);
                             // 将 Teapot 也通过 ##entity-shading 切换为 PBR。
                             editor.select(app, teapotId);
                             ++step;
                             break;
                         case 3:
                             ImGui::ActivateItemByID(propWin->GetID("##entity-shading"));
                             ++step;
                             break;
                         case 4:
                             if (!ImGui::GetCurrentContext()->OpenPopupStack.empty())
                             {
                                 auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
                                 if (popup)
                                 {
                                     ImGui::ActivateItemByID(ImHashStr("PBR", 0, popup->GetID(2)));
                                     ++step;
                                 }
                             }
                             break;
                         case 5:
                             ASSERT_EQ(app.scene().find(teapotId)->model->source().options().shading,
                                       Render::ShadingMode::Pbr);
                             // 等待下一帧完整呈现 PBR 画面后截图，并校验与 Lambert 截图存在明显像素差异。
                             ++step;
                             break;
                         case 6: {
                             glfwWaitEventsTimeout(0.06);
                             pbrShot = WindowCapture::capture(app.window());
                             WindowCapture::save(pbrShot, "review/editor-pbr.ppm");
                             EXPECT_GT(pbrShot.width, 0u);
                             afterPbrCached = editor.cachedModels();
                             EXPECT_GT(afterPbrCached, initialCached);

                             // Undo -> 回到 Lambert，Redo -> 回到 PBR，缓存条目不增长（零重复上传）。
                             EXPECT_TRUE(editor.undo(app));
                             EXPECT_EQ(app.scene().find(teapotId)->model->source().options().shading,
                                       Render::ShadingMode::Lambert);
                             EXPECT_TRUE(editor.redo(app));
                             EXPECT_EQ(app.scene().find(teapotId)->model->source().options().shading,
                                       Render::ShadingMode::Pbr);
                             EXPECT_EQ(editor.cachedModels(), afterPbrCached);

                             // 验证资产自带 unlit 材质切换后可正常撤销，且成功切换会清空旧 Redo 分支。
                             editor.select(app, unlitId);
                             EXPECT_TRUE(editor.setEntityShading(app, Render::ShadingMode::Unlit));
                             EXPECT_TRUE(editor.setEntityShading(app, Render::ShadingMode::Pbr));
                             EXPECT_TRUE(editor.undo(app));
                             EXPECT_TRUE(editor.canRedo());
                             EXPECT_TRUE(editor.setEntityShading(app, Render::ShadingMode::Lambert));
                             EXPECT_FALSE(editor.canRedo());

                             // 验证在未提交面板事务下切换着色模式失败时：先结算面板事务，再保留原模型、修改状态与历史。
                             std::filesystem::copy_file("assets/models/test_unlit/unlit.gltf",
                                                        "assets/models/test_unlit/fail_switch.gltf",
                                                        std::filesystem::copy_options::overwrite_existing);
                             ASSERT_TRUE(editor.addModel(app, "models/test_unlit/fail_switch.gltf"));
                             const Scene::EntityId failId = editor.selected();
                             editor.controller().beginPanelTransaction(app, Editor::PanelTransactionKind::Entity);
                             editor.controller().operations().rename(app.scene(), app.clearColor(), "RenamedBeforeFail");
                             ASSERT_TRUE(editor.controller().operations().inTransaction());
                             std::filesystem::remove("assets/models/test_unlit/fail_switch.gltf");
                             Resource::ResourceManager::instance().reset();
                             EXPECT_FALSE(editor.setEntityShading(app, Render::ShadingMode::Pbr));
                             EXPECT_FALSE(editor.controller().operations().inTransaction());
                             EXPECT_TRUE(editor.isDirty());
                             EXPECT_TRUE(editor.canUndo());
                             EXPECT_EQ(app.scene().find(failId)->name, "RenamedBeforeFail");
                             EXPECT_EQ(app.scene().find(failId)->model->source().options().shading,
                                       Render::ShadingMode::Lambert);
                             EXPECT_TRUE(editor.undo(app));
                             EXPECT_NE(app.scene().find(failId)->name, "RenamedBeforeFail");
                             done = true;
                             app.requestExit();
                             break;
                         }
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

    TEST_F(EditorRenderWiringTest, DisplayControlsHdrComparisonUndoRedoSaveAndRestoreUnderValidation)
    {
        copyShowcaseModels();
        std::filesystem::create_directories("assets/models/shadows");
        std::filesystem::copy_file(Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR) / "models/shadows/receiver.obj",
                                   "assets/models/shadows/receiver.obj",
                                   std::filesystem::copy_options::overwrite_existing);
        {
            Editor::EditorApp session(options(true, 1280, 800));
            Editor::EditorScene editor;
            int step = 0, frame = 0;
            bool done = false;
            WindowCapture::Image offShot{}, reinhardShot{};
            auto* handle = static_cast<GLFWwindow*>(session.window().getNativeHandle());
            auto hooks = hookWindowCallbacks(handle);
            float scanY = 0.0f;
            double foundX = 0.0, foundY = 0.0;
            const auto scanSlider = [&](ImGuiWindow* props, ImGuiID target, int nextStep) {
                if (ImGui::GetHoveredID() == target)
                {
                    foundX = ImGui::GetIO().MousePos.x;
                    foundY = ImGui::GetIO().MousePos.y;
                    scanY = 0.0f;
                    step = nextStep;
                    return;
                }
                if (scanY <= props->InnerRect.Min.y)
                    scanY = props->InnerRect.Min.y + 4.0f;
                else
                    scanY += 8.0f;
                if (scanY > props->InnerRect.Max.y)
                    scanY = props->InnerRect.Min.y + 4.0f;
                const float availW = props->WorkRect.GetWidth();
                const float fontSz = ImGui::GetFontSize();
                const float labelW = std::clamp(availW * 0.34f, fontSz * 5.5f, fontSz * 8.0f);
                hooks.cursor(handle, double(props->WorkRect.Min.x + labelW + 24.0f), double(scanY));
            };

            session.run(
                {[&](auto& app) {
                     // 1. 打开仓库自带场景 mixed.json，验证采用文件中显式显示设置。
                     editor.initialize(app, "assets/scenes/mixed.json");
                     EXPECT_EQ(app.scene().display(), (Render::DisplaySettings{Render::ToneMappingMode::Reinhard, 0.0f}));

                     // 验证读取显式 Off / +1.5 EV 的场景文件后直接采用文件中的设置。
                     auto explicitScene = Scene::captureScene(app.scene(), app.clearColor());
                     explicitScene.display = {Render::ToneMappingMode::Off, 1.5f};
                     Scene::writeScene(app.fileIO(), "assets/scenes/explicit_display.json", explicitScene);
                     ASSERT_TRUE(editor.load(app, "assets/scenes/explicit_display.json"));
                     EXPECT_EQ(app.scene().display(), (Render::DisplaySettings{Render::ToneMappingMode::Off, 1.5f}));

                     // 切回 mixed.json（默认 Reinhard / 0 EV，且清空历史），再搭建 Avocado + Teapot + 地面 + 高强方向光场景。
                     ASSERT_TRUE(editor.load(app, "assets/scenes/mixed.json"));
                     const Scene::EntityId avocadoId = app.scene().entities()[0].id;
                     editor.select(app, avocadoId);
                     ASSERT_TRUE(editor.setEntityShading(app, Render::ShadingMode::Pbr));
                     editor.selectModel(true);
                     const Scene::EntityId teapotId = editor.createEntity(app);
                     app.scene().setTransform(teapotId,
                                              Scene::Transform({0.25f, 0.0f, 0.0f}, {1, 0, 0, 0}, {0.1f, 0.1f, 0.1f}));
                     ASSERT_TRUE(editor.setEntityShading(app, Render::ShadingMode::Pbr));
                     ASSERT_TRUE(editor.addModel(
                         app, "models/shadows/receiver.obj",
                         Scene::Transform({0.0f, -0.18f, 0.0f},
                                          glm::angleAxis(-glm::half_pi<float>(), glm::vec3(1, 0, 0)),
                                          {1.5f, 1.5f, 1.0f})));
                     editor.select(app, avocadoId);
                     app.shadowSettings().enabled = true;
                     app.scene().setLighting({{-0.4f, -0.8f, -0.6f}, {1.0f, 0.98f, 0.92f}, 3.5f, 0.1f});
                     app.scene().setDisplay({Render::ToneMappingMode::Reinhard, 0.0f});
                     hooks = hookWindowCallbacks(handle);
                     enableGlfwCallbacks(handle);
                     hooks.focus(handle, GLFW_TRUE);
                 },
                 [&](auto& app, const auto&) {
                     if (++frame > 400)
                     {
                         ADD_FAILURE() << "DisplayControls test timeout at step=" << step;
                         app.requestExit();
                         return;
                     }
                     auto* propWin = ImGui::FindWindowByName("Properties");
                     if (frame < 5 || !propWin || app.sceneExtent().width == 0)
                         return;
                     hooks.focus(handle, GLFW_TRUE);

                     switch (step)
                     {
                         case 0:
                             // 确保 Properties 滚动到底部使 Scene settings 显示控件可见。
                             propWin->Scroll.y = propWin->ScrollMax.y;
                             ++step;
                             break;
                         case 1:
                             // 采集同机位 Reinhard 截图并激活 ##display-tone-mapping Combo。
                             glfwWaitEventsTimeout(0.06);
                             reinhardShot = WindowCapture::capture(app.window());
                             WindowCapture::save(reinhardShot, "review/hdr-reinhard.ppm");
                             ImGui::ActivateItemByID(propWin->GetID("##display-tone-mapping"));
                             ++step;
                             break;
                         case 2:
                             if (!ImGui::GetCurrentContext()->OpenPopupStack.empty())
                             {
                                 auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
                                 if (popup)
                                 {
                                     ImGui::ActivateItemByID(ImHashStr("Off", 0, popup->GetID(int(0))));
                                     ++step;
                                 }
                             }
                             break;
                         case 3:
                             if (app.scene().display().toneMapping == Render::ToneMappingMode::Off)
                             {
                                 EXPECT_FLOAT_EQ(app.scene().display().exposureEv, 0.0f);
                                 EXPECT_TRUE(editor.canUndo());
                                 ++step;
                             }
                             break;
                         case 4:
                             // 等待一帧渲染后采集同机位 Off 截图。
                             glfwWaitEventsTimeout(0.06);
                             offShot = WindowCapture::capture(app.window());
                             WindowCapture::save(offShot, "review/hdr-off.ppm");
                             EXPECT_GT(offShot.width, 0u);
                             // 撤销回到 Reinhard 并扫描定位 ##display-exposure-ev。
                             EXPECT_TRUE(editor.undo(app));
                             EXPECT_EQ(app.scene().display().toneMapping, Render::ToneMappingMode::Reinhard);
                             EXPECT_FLOAT_EQ(app.scene().display().exposureEv, 0.0f);
                             ++step;
                             break;
                         case 5:
                             scanSlider(propWin, propWin->GetID("##display-exposure-ev"), 6);
                             break;
                         case 6:
                             // 在 Reinhard 下通过实际 ImGui SliderFloat 连续拖动曝光，验证仅记 1 步撤销历史。
                             hooks.mouse(handle, 0, GLFW_PRESS, 0);
                             ++step;
                             break;
                         case 7:
                             hooks.cursor(handle, foundX + 30.0, foundY);
                             ++step;
                             break;
                         case 8:
                             hooks.cursor(handle, foundX + 60.0, foundY);
                             ++step;
                             break;
                         case 9:
                             hooks.mouse(handle, 0, GLFW_RELEASE, 0);
                             ++step;
                             break;
                         case 10: {
                             const float draggedEv = app.scene().display().exposureEv;
                             EXPECT_GT(draggedEv, 0.5f);
                             // 连续拖动仅增加 1 条历史，撤销一次应直接回到 0 EV。
                             EXPECT_TRUE(editor.undo(app));
                             EXPECT_FLOAT_EQ(app.scene().display().exposureEv, 0.0f);
                             EXPECT_TRUE(editor.redo(app));
                             EXPECT_FLOAT_EQ(app.scene().display().exposureEv, draggedEv);

                             // 切到 Off 并切回 Reinhard，确认 EV 保留。
                             EXPECT_TRUE(editor.controller().setDisplay(app, {Render::ToneMappingMode::Off, draggedEv}));
                             EXPECT_TRUE(editor.controller().setDisplay(
                                 app, {Render::ToneMappingMode::Reinhard, draggedEv}));

                             // 保存到新文件并加载恢复显示模式与 EV，同时验证最小化恢复。
                             ASSERT_TRUE(editor.save(app, "assets/scenes/hdr_roundtrip.json"));
                             ASSERT_TRUE(editor.load(app, "assets/scenes/hdr_roundtrip.json"));
                             EXPECT_EQ(app.scene().display().toneMapping, Render::ToneMappingMode::Reinhard);
                             EXPECT_NEAR(app.scene().display().exposureEv, draggedEv, 1e-4f);

                             glfwIconifyWindow(handle);
                             glfwPollEvents();
                             glfwRestoreWindow(handle);
                             glfwPollEvents();
                             ++step;
                             break;
                         }
                         case 11:
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

    TEST_F(EditorRenderWiringTest, FxaaViewCheckboxNextFrameOutputResizeOverlayAndCleanDocumentUnderValidation)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(true, 1280, 800));
            Editor::EditorScene editor;
            Scene::EntityId targetId{};
            Render::FrameExtent baseExtent{};
            std::vector<uint8_t> offPixels, onPixels;
            int frame = 0, step = 0, resizeWaitFrames = 0;
            bool done = false;
            bool checkedSameFrameOn = false, checkedSameFrameOff = false;
            auto* handle = static_cast<GLFWwindow*>(session.window().getNativeHandle());
            enableGlfwCallbacks(handle, false);

            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/mixed.json");
                     ASSERT_FALSE(app.scene().entities().empty());
                     targetId = app.scene().entities()[0].id;
                     editor.select(app, targetId);
                     EXPECT_FALSE(app.fxaaEnabled());
                     EXPECT_FALSE(Resource::ResourceRegistry::instance()
                                      .lookup(Resource::ResourceType::SHADER, "shaders/fxaa.frag.spv")
                                      .valid());
                 },
                 [&](auto& app, const auto&) {
                     if (++frame > 180)
                     {
                         ADD_FAILURE() << "FXAA editor integration test timeout at step=" << step;
                         app.requestExit();
                         return;
                     }
                     auto* propsWin = ImGui::FindWindowByName("Properties");
                     if (frame < 5 || !propsWin || !app.sceneOutput().valid())
                         return;

                     switch (step)
                     {
                         case 0: {
                             // 默认关闭且不加载 FXAA Shader；抓取原 LDR 输出基线，并在本帧 UI 阶段点击复选框开启 FXAA。
                             EXPECT_FALSE(app.fxaaEnabled());
                             EXPECT_FALSE(app.sceneOutput().fxaaEnabled());
                             EXPECT_FALSE(Resource::ResourceRegistry::instance()
                                              .lookup(Resource::ResourceType::SHADER, "shaders/fxaa.frag.spv")
                                              .valid());
                             baseExtent = app.sceneOutput().extent();
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& device, const auto& image) {
                                     offPixels = TextureGpuReview::captureMip(
                                         device, image, 0, {baseExtent.width, baseExtent.height});
                                 });
                             ASSERT_FALSE(offPixels.empty());
                             propsWin->Scroll.y = propsWin->ScrollMax.y;
                             ImGui::ActivateItemByID(propsWin->GetID("##fxaa"));
                             ++step;
                             break;
                         }
                         case 1:
                             // 上一帧（触发复选框的同一帧）仍输出原 LDR；等待当前帧（已用开启状态提交）完成绘制。
                             EXPECT_TRUE(app.fxaaEnabled());
                             EXPECT_FALSE(app.sceneOutput().fxaaEnabled());
                             ++step;
                             break;
                         case 2: {
                             // 下一有效视口帧已按 FXAA 输出；验证按需加载并已释放 CPU 持有、边缘与原图存在差异、纯背景一致。
                             EXPECT_TRUE(app.sceneOutput().fxaaEnabled());
                             EXPECT_EQ(app.sceneOutput().extent(), baseExtent);
                             const auto fxaaId = Resource::ResourceRegistry::instance().lookup(
                                 Resource::ResourceType::SHADER, "shaders/fxaa.frag.spv");
                             ASSERT_TRUE(fxaaId.valid());
                             EXPECT_EQ(Resource::ResourceRegistry::instance().refCount(fxaaId), 0u);
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& device, const auto& image) {
                                     onPixels = TextureGpuReview::captureMip(
                                         device, image, 0, {baseExtent.width, baseExtent.height});
                                 });
                             ASSERT_EQ(onPixels.size(), offPixels.size());
                             EXPECT_NE(onPixels, offPixels);
                             for (int c = 0; c < 4; ++c)
                                 EXPECT_EQ(onPixels[size_t(4 * baseExtent.width + 4) * 4u + c],
                                           offPixels[size_t(4 * baseExtent.width + 4) * 4u + c]);

                             // 验证拾取仍沿用原图像区域，且切换未产生脏状态、撤销记录或场景文件字段。
                             const auto region = app.viewportRegion();
                             bool picked = false;
                             for (int y = 1; y < 20 && !picked; ++y)
                                 for (int x = 1; x < 20 && !picked; ++x)
                                     if (const auto hit = app.scene().pick(baseExtent, {x / 20.0f, y / 20.0f});
                                         hit && hit->id == targetId)
                                         picked = true;
                             EXPECT_TRUE(picked && region.size.x > 0 && region.size.y > 0);
                             EXPECT_FALSE(editor.isDirty());
                             EXPECT_FALSE(editor.canUndo());
                             ASSERT_TRUE(editor.save(app, "fxaa-clean.json"));
                             std::ifstream savedFile("fxaa-clean.json");
                             const std::string json((std::istreambuf_iterator<char>(savedFile)), {});
                             EXPECT_EQ(json.find("fxaa"), std::string::npos);

                             glfwSetWindowSize(handle, 960, 560);
                             ++step;
                             break;
                         }
                         case 3:
                             if (app.sceneOutput().extent() != baseExtent)
                             {
                                 std::vector<uint8_t> resizedPixels;
                                 EXPECT_TRUE(app.sceneOutput().fxaaEnabled());
                                 const auto resizedExtent = app.sceneOutput().extent();
                                 Render::Detail::RendererReviewAccess::withSceneOutput(
                                     app.renderer(), app.sceneOutput(), [&](const auto& device, const auto& image) {
                                         resizedPixels = TextureGpuReview::captureMip(
                                             device, image, 0, {resizedExtent.width, resizedExtent.height});
                                     });
                                 EXPECT_EQ(resizedPixels.size(),
                                           size_t(resizedExtent.width) * resizedExtent.height * 4u);
                                 ImGui::ActivateItemByID(propsWin->GetID("##fxaa"));
                                 ++step;
                             }
                             else if (++resizeWaitFrames > 40)
                             {
                                 ADD_FAILURE() << "Viewport resize did not propagate";
                                 app.requestExit();
                             }
                             break;
                         case 4:
                             EXPECT_FALSE(app.fxaaEnabled());
                             EXPECT_TRUE(app.sceneOutput().fxaaEnabled());
                             ++step;
                             break;
                         case 5:
                             EXPECT_FALSE(app.sceneOutput().fxaaEnabled());
                             EXPECT_FALSE(editor.isDirty());
                             EXPECT_FALSE(editor.canUndo());
                             done = true;
                             app.requestExit();
                             break;
                     }
                 },
                 [&](auto& app) {
                     auto* sceneList = app.sceneDrawList();
                     const int beforeVtx = sceneList ? sceneList->VtxBuffer.Size : 0;
                     editor.ui(app);
                     const int afterVtx = sceneList ? sceneList->VtxBuffer.Size : 0;
                     // 文字、选中框与 Gizmo 仍在 FXAA 之后经 UI 绘制列表叠加。
                     if (sceneList && app.sceneOutput().valid())
                         EXPECT_GT(afterVtx, beforeVtx);
                     if (step == 1 && !checkedSameFrameOn)
                     {
                         EXPECT_TRUE(app.fxaaEnabled());
                         EXPECT_FALSE(app.sceneOutput().fxaaEnabled());
                         checkedSameFrameOn = true;
                     }
                     if (step == 4 && !checkedSameFrameOff)
                     {
                         EXPECT_FALSE(app.fxaaEnabled());
                         EXPECT_TRUE(app.sceneOutput().fxaaEnabled());
                         checkedSameFrameOff = true;
                     }
                 },
                 [&](auto& app) { editor.cancelInteraction(app); },
                 [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
                 [&](auto& app) { editor.layout(app); },
                 [&](auto& app) { editor.requestClose(app); }});
            EXPECT_TRUE(done && checkedSameFrameOn && checkedSameFrameOff);
        }
        cleanValidation();
    }

    TEST_F(EditorRenderWiringTest, ViewStatsPanelCpuGpuAndDrawCountsTrackShadowToggle)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(true, 1280, 800));
            Editor::EditorScene editor;
            int frame = 0;
            int step = 0;
            int waitCounter = 0;
            uint32_t expectedSceneDraws = 0;
            bool done = false;
            auto* handle = static_cast<GLFWwindow*>(session.window().getNativeHandle());
            enableGlfwCallbacks(handle, false);

            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/mixed.json");
                     EXPECT_FALSE(app.shadowSettings().enabled);
                     EXPECT_FALSE(app.viewportStats().hasCompletedSample);
                 },
                 [&](auto& app, const auto&) {
                     if (++frame > 180)
                     {
                         ADD_FAILURE() << "View stats editor integration test timeout at step=" << step;
                         app.requestExit();
                         return;
                     }
                     EXPECT_TRUE(std::isfinite(app.lastFrameTime().deltaSeconds()));
                     EXPECT_GE(app.lastFrameTime().deltaSeconds(), 0.0);

                     auto* propsWin = ImGui::FindWindowByName("Properties");
                     if (frame < 6 || !propsWin || !app.sceneOutput().valid())
                         return;

                     const auto stats = app.viewportStats();
                     switch (step)
                     {
                         case 0:
                             if (stats.hasCompletedSample)
                             {
                                 EXPECT_GT(stats.sceneDrawCount, 0u);
                                 EXPECT_EQ(stats.shadowDrawCount, 0u);
                                 if (stats.gpuTimestampAvailable)
                                 {
                                     ASSERT_TRUE(stats.gpuTimeMs.has_value());
                                     EXPECT_GE(*stats.gpuTimeMs, 0.0);
                                 }
                                 expectedSceneDraws = stats.sceneDrawCount;
                                 propsWin->Scroll.y = propsWin->ScrollMax.y;
                                 ImGui::ActivateItemByID(propsWin->GetID("##shadows"));
                                 waitCounter = 0;
                                 ++step;
                             }
                             break;
                         case 1:
                             EXPECT_TRUE(app.shadowSettings().enabled);
                             if (++waitCounter >= 4 && stats.hasCompletedSample && stats.shadowDrawCount > 0u)
                             {
                                 EXPECT_EQ(stats.sceneDrawCount, expectedSceneDraws);
                                 EXPECT_EQ(stats.shadowDrawCount, expectedSceneDraws);
                                 propsWin->Scroll.y = propsWin->ScrollMax.y;
                                 ImGui::ActivateItemByID(propsWin->GetID("##shadows"));
                                 waitCounter = 0;
                                 ++step;
                             }
                             break;
                         case 2:
                             EXPECT_FALSE(app.shadowSettings().enabled);
                             if (++waitCounter >= 4 && stats.hasCompletedSample && stats.shadowDrawCount == 0u)
                             {
                                 EXPECT_EQ(stats.sceneDrawCount, expectedSceneDraws);
                                 EXPECT_FALSE(editor.isDirty());
                                 done = true;
                                 app.requestExit();
                             }
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

    TEST_F(EditorRenderWiringTest, StudioIblViewCheckboxDefaultOffLazyLoadSameFrameOutputAndCleanDocumentUnderValidation)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(true, 1280, 800));
            Editor::EditorScene editor;
            Render::FrameExtent baseExtent{};
            std::vector<uint8_t> offPixels, onPixels, offAgainPixels;
            int frame = 0, step = 0, waitFrames = 0;
            bool done = false;
            auto* handle = static_cast<GLFWwindow*>(session.window().getNativeHandle());
            enableGlfwCallbacks(handle, false);

            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/pbr-showcase.json");
                     app.shadowSettings().enabled = true;
                     EXPECT_FALSE(app.iblEnabled());
                     EXPECT_FALSE(ResourceRegistry::instance()
                                      .lookup(ResourceType::TEXTURE, "ibl/studio_irradiance.ktx")
                                      .valid());
                 },
                 [&](auto& app, const auto&) {
                     if (++frame > 180)
                     {
                         ADD_FAILURE() << "Studio IBL editor integration test timeout at step=" << step;
                         app.requestExit();
                         return;
                     }
                     auto* propsWin = ImGui::FindWindowByName("Properties");
                     if (frame < 5 || !propsWin || !app.sceneOutput().valid())
                         return;

                     switch (step)
                     {
                         case 0:
                             EXPECT_FALSE(app.iblEnabled());
                             EXPECT_FALSE(ResourceRegistry::instance()
                                              .lookup(ResourceType::TEXTURE, "ibl/studio_irradiance.ktx")
                                              .valid());
                             baseExtent = app.sceneOutput().extent();
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& device, const auto& image) {
                                     offPixels = TextureGpuReview::captureMip(
                                         device, image, 0, {baseExtent.width, baseExtent.height});
                                 });
                             ASSERT_FALSE(offPixels.empty());
                             propsWin->Scroll.y = propsWin->ScrollMax.y;
                             ImGui::ActivateItemByID(propsWin->GetID("##ibl"));
                             waitFrames = 0;
                             ++step;
                             break;
                         case 1:
                             EXPECT_TRUE(app.iblEnabled());
                             if (++waitFrames < 2)
                                 return;
                             for (const char* iblPath :
                                  {"ibl/studio_irradiance.ktx", "ibl/studio_prefiltered.ktx", "ibl/studio_brdf_lut.ktx"})
                             {
                                 const auto id = ResourceRegistry::instance().lookup(ResourceType::TEXTURE, iblPath);
                                 ASSERT_TRUE(id.valid());
                                 EXPECT_EQ(ResourceRegistry::instance().refCount(id), 0u);
                             }
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& device, const auto& image) {
                                     onPixels = TextureGpuReview::captureMip(
                                         device, image, 0, {baseExtent.width, baseExtent.height});
                                 });
                             ASSERT_EQ(onPixels.size(), offPixels.size());
                             EXPECT_NE(onPixels, offPixels);
                             EXPECT_FALSE(editor.isDirty());
                             EXPECT_FALSE(editor.canUndo());

                             // 关闭开关后须恢复关闭时的 PBR 像素基线且不产生脏状态或 Undo。
                             propsWin->Scroll.y = propsWin->ScrollMax.y;
                             ImGui::ActivateItemByID(propsWin->GetID("##ibl"));
                             waitFrames = 0;
                             ++step;
                             break;
                         case 2:
                             EXPECT_FALSE(app.iblEnabled());
                             if (++waitFrames < 2)
                                 return;
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& device, const auto& image) {
                                     offAgainPixels = TextureGpuReview::captureMip(
                                         device, image, 0, {baseExtent.width, baseExtent.height});
                                 });
                             EXPECT_EQ(offAgainPixels, offPixels);
                             EXPECT_FALSE(editor.isDirty());
                             EXPECT_FALSE(editor.canUndo());
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

    TEST_F(EditorRenderWiringTest, MaterialPanelSlotSelectionContinuousTransactionUndoRedoResetAndSaveReopen12UnderValidation)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(true, 1280, 800));
            Editor::EditorScene editor;
            Scene::EntityId cutId{}, peelId{};
            Render::FrameExtent extent{};
            std::vector<uint8_t> initialPixels, overriddenPixels, savedPixels, reopenedPixels, resetPixels;
            const std::string savePath = "saved-material-v12.json";
            int frame = 0, step = 0, waitFrames = 0;
            bool done = false;
            auto* handle = static_cast<GLFWwindow*>(session.window().getNativeHandle());
            enableGlfwCallbacks(handle, false);

            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/pbr-showcase.json");
                     app.shadowSettings().enabled = true;
                     app.iblEnabled() = true;
                     ASSERT_GE(app.scene().entities().size(), 2u);
                     cutId = app.scene().entities()[0].id;
                     peelId = app.scene().entities()[1].id;
                     editor.select(app, cutId);
                 },
                 [&](auto& app, const auto&) {
                     if (++frame > 240)
                     {
                         ADD_FAILURE() << "Material panel test timeout at step=" << step;
                         app.requestExit();
                         return;
                     }
                     auto* matWin = ImGui::FindWindowByName("Material");
                     if (frame < 6 || !matWin || !app.sceneOutput().valid())
                         return;

                     switch (step)
                     {
                         case 0:
                             // 确保 Material 面板聚焦在前，等 1 帧提交控件后再操作。
                             ImGui::SetWindowFocus("Material");
                             if (matWin->Hidden)
                                 return;
                             extent = app.sceneOutput().extent();
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& d, const auto& img) {
                                     initialPixels = TextureGpuReview::captureMip(d, img, 0, {extent.width, extent.height});
                                 });
                             ASSERT_FALSE(initialPixels.empty());
                             // 验证源槽位选择器存在并切到 Fixed value 金属度模式。
                             EXPECT_NE(matWin->GetID("##mat-slot"), 0u);
                             ImGui::ActivateItemByID(matWin->GetID("##mat-metallic-mode"));
                             ++step;
                             break;
                         case 1:
                             if (!ImGui::GetCurrentContext()->OpenPopupStack.empty())
                             {
                                 auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
                                 if (popup)
                                 {
                                     ImGui::ActivateItemByID(ImHashStr("Fixed value", 0, popup->GetID(1)));
                                     ++step;
                                 }
                             }
                             break;
                         case 2:
                             // 验证模式切换记 1 步撤销，随后在 PanelTransactionKind::Material 内连续拖动三步折叠为 1 步。
                             ASSERT_EQ(app.scene().find(cutId)->materialOverrides.size(), 1u);
                             EXPECT_EQ(app.scene().find(cutId)->materialOverrides[0].value.metallicMode,
                                       Render::MaterialParameterMode::FixedValue);
                             EXPECT_TRUE(editor.canUndo());
                             EXPECT_TRUE(editor.undo(app));
                             EXPECT_TRUE(app.scene().find(cutId)->materialOverrides.empty());
                             EXPECT_TRUE(editor.redo(app));
                             ASSERT_EQ(app.scene().find(cutId)->materialOverrides.size(), 1u);

                             // 开始连续拖动事务，模拟颜色+粗糙度+金属度连续调整（最后应仅压入 1 步 Undo）。
                             ASSERT_TRUE(editor.controller().beginPanelTransaction(
                                 app, Editor::PanelTransactionKind::Material));
                             for (const float rough : {0.4f, 0.25f, 0.12f})
                             {
                                 Scene::MaterialOverride ov{
                                     0u,
                                     {0.95f, 0.35f, 0.25f},
                                     Render::MaterialParameterMode::FixedValue,
                                     rough,
                                     Render::MaterialParameterMode::FixedValue,
                                     1.0f,
                                 };
                                 ASSERT_TRUE(editor.controller().operations().setMaterialOverride(
                                     app.scene(), app.clearColor(), ov));
                             }
                             editor.controller().commitPanelTransactions(app);
                             EXPECT_TRUE(editor.isDirty());
                             // 再撤销 1 步即回退整次连续拖动，重做 1 步恢复到最终覆盖。
                             EXPECT_TRUE(editor.undo(app));
                             EXPECT_FLOAT_EQ(app.scene().find(cutId)->materialOverrides[0].value.roughness, 1.0f);
                             EXPECT_TRUE(editor.redo(app));
                             EXPECT_FLOAT_EQ(app.scene().find(cutId)->materialOverrides[0].value.roughness, 0.12f);
                             EXPECT_TRUE(app.scene().find(peelId)->materialOverrides.empty());
                             waitFrames = 0;
                             ++step;
                             break;
                         case 3:
                             if (++waitFrames < 2)
                                 return;
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& d, const auto& img) {
                                     overriddenPixels =
                                         TextureGpuReview::captureMip(d, img, 0, {extent.width, extent.height});
                                 });
                             EXPECT_NE(overriddenPixels, initialPixels);

                             // 测试实体复制保留覆盖、隔离编辑与 1.2 保存重开恢复 GPU 引用。
                             ASSERT_TRUE(editor.save(app, savePath));
                             {
                                 std::ifstream in(savePath, std::ios::binary);
                                 const std::string savedText((std::istreambuf_iterator<char>(in)),
                                                             std::istreambuf_iterator<char>());
                                 EXPECT_NE(savedText.find("\"minorVersion\": 2"), std::string::npos);
                                 EXPECT_NE(savedText.find("\"materialOverrides\""), std::string::npos);
                             }
                             savedPixels = overriddenPixels;
                             ASSERT_TRUE(editor.load(app, "assets/scenes/mixed.json"));
                             ASSERT_TRUE(editor.load(app, savePath));
                             ASSERT_EQ(app.scene().entities().size(), 3u);
                             cutId = app.scene().entities()[0].id;
                             peelId = app.scene().entities()[1].id;
                             ASSERT_EQ(app.scene().find(cutId)->materialOverrides.size(), 1u);
                             EXPECT_TRUE(app.scene().find(peelId)->materialOverrides.empty());
                             editor.select(app, cutId);
                             waitFrames = 0;
                             ++step;
                             break;
                         case 4:
                             if (++waitFrames < 2)
                                 return;
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& d, const auto& img) {
                                     reopenedPixels =
                                         TextureGpuReview::captureMip(d, img, 0, {extent.width, extent.height});
                                 });
                             EXPECT_EQ(reopenedPixels, savedPixels);

                             // 点击 Reset slot 按钮重置覆盖并验证恢复源材质（随后保存写回 minorVersion: 1）。
                             ImGui::SetWindowFocus("Material");
                             if (matWin->Hidden)
                                 return;
                             ImGui::ActivateItemByID(matWin->GetID("Reset slot##mat-reset-slot"));
                             waitFrames = 0;
                             ++step;
                             break;
                         case 5:
                             EXPECT_TRUE(app.scene().find(cutId)->materialOverrides.empty());
                             if (++waitFrames < 2)
                                 return;
                             Render::Detail::RendererReviewAccess::withSceneOutput(
                                 app.renderer(), app.sceneOutput(), [&](const auto& d, const auto& img) {
                                     resetPixels =
                                         TextureGpuReview::captureMip(d, img, 0, {extent.width, extent.height});
                                 });
                             EXPECT_EQ(resetPixels, initialPixels);
                             EXPECT_TRUE(editor.undo(app));
                             EXPECT_EQ(app.scene().find(cutId)->materialOverrides.size(), 1u);
                             EXPECT_TRUE(editor.redo(app));
                             EXPECT_TRUE(app.scene().find(cutId)->materialOverrides.empty());

                             // 复制实体与更换模型时正确处理覆盖，且源 Unlit 保留导入行为不被面板覆写。
                             ASSERT_TRUE(editor.controller().setMaterialOverride(
                                 app, {0u, {0.4f, 0.8f, 0.9f}, Render::MaterialParameterMode::FixedValue, 0.2f,
                                       Render::MaterialParameterMode::FixedValue, 0.9f}));
                             const auto dupId = editor.duplicateSelected(app);
                             ASSERT_TRUE(dupId.valid());
                             EXPECT_EQ(app.scene().find(dupId)->materialOverrides.size(), 1u);
                             EXPECT_TRUE(editor.setEntityShading(app, Render::ShadingMode::Lambert));
                             EXPECT_TRUE(app.scene().find(dupId)->materialOverrides.empty());
                             EXPECT_TRUE(editor.setEntityShading(app, Render::ShadingMode::Unlit));
                             EXPECT_FALSE(editor.controller().setMaterialOverride(
                                 app, {0u, {1.0f, 1.0f, 1.0f}, Render::MaterialParameterMode::FixedValue, 0.5f,
                                       Render::MaterialParameterMode::FixedValue, 0.5f}));
                             EXPECT_TRUE(app.scene().find(dupId)->materialOverrides.empty());
                             EXPECT_EQ(app.scene().find(cutId)->materialOverrides.size(), 1u);
                             editor.select(app, cutId);
                             EXPECT_TRUE(editor.controller().resetAllMaterialOverrides(app));
                             ASSERT_TRUE(editor.save(app, savePath));
                             {
                                 std::ifstream in(savePath, std::ios::binary);
                                 const std::string savedNoOverride((std::istreambuf_iterator<char>(in)),
                                                                   std::istreambuf_iterator<char>());
                                 EXPECT_NE(savedNoOverride.find("\"minorVersion\": 1"), std::string::npos);
                             }
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

    TEST_F(EditorRenderWiringTest, LocalLightsViewControlsDebugLinesAndScreenshotPngUnderValidation)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(false, 1280, 800));
            Editor::EditorScene editor;
            int step = 0;
            bool done = false;
            const fs::path pngPath = "editor-viewport-screenshot-test.png";
            fs::remove(pngPath);

            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/shadows.json");
                     ASSERT_FALSE(app.scene().entities().empty());
                     editor.select(app.scene().entities().front().id);
                 },
                 [&](auto& app, const auto&) {
                     auto* propsWin = ImGui::FindWindowByName("Properties");
                     if (!propsWin || app.sceneExtent().width == 0u || app.sceneExtent().height == 0u)
                         return;
                     switch (step)
                     {
                         case 0:
                         {
                             // 确认新面板控件 ID 存在，且切换本地阴影与辅助线开关不标脏文档。
                             EXPECT_NE(propsWin->GetID("##shadow-csm"), 0u);
                             EXPECT_NE(propsWin->GetID("##shadow-res"), 0u);
                             EXPECT_NE(propsWin->GetID("##shadow-fit"), 0u);
                             EXPECT_NE(propsWin->GetID("##shadow-normal-bias"), 0u);
                             EXPECT_NE(propsWin->GetID("##display-skybox"), 0u);
                             EXPECT_NE(propsWin->GetID("##display-bloom"), 0u);
                             EXPECT_NE(propsWin->GetID("##display-bloom-threshold"), 0u);
                             EXPECT_NE(propsWin->GetID("##display-bloom-intensity"), 0u);
                             EXPECT_NE(propsWin->GetID("##debug-grid"), 0u);
                             EXPECT_NE(propsWin->GetID("##debug-selection-bounds"), 0u);
                             EXPECT_NE(propsWin->GetID("##debug-light-gizmos"), 0u);
                             EXPECT_NE(propsWin->GetID("##local-light-add-point"), 0u);
                             EXPECT_NE(propsWin->GetID("##local-light-add-spot"), 0u);

                             EXPECT_FALSE(editor.isDirty());
                             app.shadowSettings().enabled = true;
                             app.shadowSettings().cascadeCount = 3u;
                             app.shadowSettings().resolution = 2048u;
                             app.shadowSettings().fitToBounds = true;
                             app.shadowSettings().normalBias = 0.02f;
                             editor.debugOptions().showGrid = true;
                             editor.debugOptions().showSelectionBounds = true;
                             editor.debugOptions().showLightGizmos = true;
                             EXPECT_FALSE(editor.isDirty());

                             const auto dbgLines =
                                 Editor::buildViewportDebugLines(app.scene(), editor.selected(), editor.debugOptions());
                             EXPECT_GE(dbgLines.size(), 42u + 12u + 3u);

                             // 通过控制器添加点光/聚光并切换 ACES + Skybox + Bloom，等待下一帧完成渲染再截图。
                             ASSERT_TRUE(editor.controller().addLocalLight(app, Render::LocalLightType::Point));
                             ASSERT_TRUE(editor.controller().addLocalLight(app, Render::LocalLightType::Spot));
                             ASSERT_TRUE(editor.controller().setDisplay(
                                 app, {Render::ToneMappingMode::AcesFilmic, 0.2f, true, true, 1.0f, 0.4f}));
                             EXPECT_TRUE(editor.isDirty());
                             ++step;
                             break;
                         }
                         case 1:
                         {
                             // 额外等待一帧让启用多光源、CSM 与后处理的视口图像完整提交。
                             ++step;
                             break;
                         }
                         case 2:
                         {
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
                             const auto& px = tex->pixels();
                             bool hasVariance = false;
                             for (size_t i = 4; i + 3 < px.size(); i += 4)
                             {
                                 if (px[i] != px[0] || px[i + 1] != px[1] || px[i + 2] != px[2])
                                 {
                                     hasVariance = true;
                                     break;
                                 }
                             }
                             EXPECT_TRUE(hasVariance);

                             // 验证 Undo 连续恢复 Display 与局部光源并保持工作区可保存。
                             ASSERT_TRUE(editor.undo(app));
                             EXPECT_EQ(app.scene().display().toneMapping, Render::ToneMappingMode::Reinhard);
                             ASSERT_TRUE(editor.undo(app));
                             EXPECT_EQ(app.scene().lighting().localLights.size(), 1u);
                             done = true;
                             app.requestExit();
                             break;
                         }
                     }
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
