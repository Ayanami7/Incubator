#include "editor_test_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Test;
    namespace fs = std::filesystem;
    namespace NativeIO = Incubator::FileIO;

    class EditorWorkspaceDocsTest : public EditorAppTest
    {};

    TEST_F(EditorWorkspaceDocsTest, ReloadUpdatesSharedEntitiesUndoRedoAndRejectsIncompatibleOverrides)
    {
        Editor::EditorApp session(options());
        session.run({[&](auto& app) {
                         app.requestExit();
                         Scene::Scene scene;
                         auto clearColor = app.clearColor();
                         Editor::EditorOperations operations;
                         operations.initialize(app.renderer());
                         const auto uncached = operations.reloadModel(scene, GLTF);
                         ASSERT_TRUE(uncached);
                         EXPECT_EQ(uncached->reloadedVariants, 0u);

                         ASSERT_TRUE(operations.addModel(scene, clearColor, GLTF)) << operations.message();
                         const auto first = operations.selected();
                         ASSERT_TRUE(operations.setEntityShading(scene, clearColor, Render::ShadingMode::Pbr));
                         const Scene::MaterialOverride overrideValue{0, {0.9f, 0.5f, 0.2f}};
                         ASSERT_TRUE(operations.setMaterialOverride(scene, clearColor, overrideValue));
                         const auto child = operations.duplicate(scene, clearColor);
                         ASSERT_TRUE(child.valid());
                         ASSERT_TRUE(operations.setParent(scene, clearColor, child, first));
                         ASSERT_TRUE(operations.setVisible(scene, clearColor, child, false));
                         operations.select(first);
                         const auto oldModel = scene.find(first)->model;
                         const auto oldCompiled = scene.find(first)->materialOverrides[0].compiled;
                         ASSERT_TRUE(oldCompiled);
                         ASSERT_EQ(scene.find(child)->model, oldModel);
                         const auto childName = scene.find(child)->name;
                         const auto childTransform = scene.find(child)->transform.matrix();
                         const auto initialPosition = scene.find(first)->transform.position();
                         ASSERT_TRUE(operations.save(app.fileIO(), scene, clearColor, "reload-test.json"));
                         const glm::vec3 moved{2, 3, 4};
                         ASSERT_TRUE(operations.setTransform(scene, clearColor, Scene::Transform(moved)));

                         const auto modelFile = fs::path("assets") / GLTF;
                         std::ifstream input(modelFile, std::ios::binary);
                         std::string modelText{std::istreambuf_iterator<char>(input), {}};
                         input.close();
                         const auto translation = modelText.find("0.25");
                         ASSERT_NE(translation, std::string::npos);
                         modelText.replace(translation, 4, "0.75");
                         std::ofstream(modelFile, std::ios::binary) << modelText;

                         operations.beginTransaction(scene, clearColor);
                         EXPECT_FALSE(operations.reloadModel(scene, GLTF));
                         EXPECT_NE(operations.message().find("active edit"), std::string::npos);
                         EXPECT_EQ(scene.find(first)->model, oldModel);
                         EXPECT_FALSE(operations.commitTransaction(scene, clearColor));

                         const auto cached = operations.cachedModels();
                         ASSERT_TRUE(operations.isDirty());
                         const auto report = operations.reloadModel(scene, GLTF);
                         ASSERT_TRUE(report) << operations.message();
                         EXPECT_EQ(report->reloadedVariants, 2u);
                         EXPECT_EQ(report->updatedEntities, 2u);
                         EXPECT_GT(report->updatedHistorySnapshots, 0u);
                         EXPECT_EQ(operations.cachedModels(), cached);
                         EXPECT_TRUE(operations.isDirty());
                         EXPECT_EQ(operations.selected(), first);
                         const auto newModel = scene.find(first)->model;
                         EXPECT_NE(newModel, oldModel);
                         EXPECT_NE(newModel->draws().front().model, oldModel->draws().front().model);
                         EXPECT_EQ(scene.find(first)->id, first);
                         EXPECT_EQ(scene.find(first)->transform.position(), moved);
                         EXPECT_EQ(scene.find(child)->id, child);
                         EXPECT_EQ(scene.find(child)->parent, first);
                         EXPECT_EQ(scene.find(child)->name, childName);
                         EXPECT_EQ(scene.find(child)->transform.matrix(), childTransform);
                         EXPECT_FALSE(scene.find(child)->visible);
                         EXPECT_EQ(scene.find(child)->model, newModel);
                         ASSERT_EQ(scene.find(first)->materialOverrides.size(), 1u);
                         const auto newCompiled = scene.find(first)->materialOverrides[0].compiled;
                         ASSERT_TRUE(newCompiled);
                         EXPECT_NE(newCompiled, oldCompiled);
                         EXPECT_EQ(scene.find(first)->materialOverrides[0].value, overrideValue);

                         ASSERT_TRUE(operations.undo(scene, clearColor));
                         EXPECT_FALSE(operations.isDirty());
                         EXPECT_EQ(scene.find(first)->transform.position(), initialPosition);
                         EXPECT_EQ(scene.find(first)->model, newModel);
                         const auto cleanReload = operations.reloadModel(scene, GLTF);
                         ASSERT_TRUE(cleanReload) << operations.message();
                         EXPECT_GT(cleanReload->updatedHistorySnapshots, 1u);
                         EXPECT_FALSE(operations.isDirty());
                         const auto latestModel = scene.find(first)->model;
                         EXPECT_NE(latestModel, newModel);
                         ASSERT_TRUE(operations.redo(scene, clearColor));
                         EXPECT_EQ(scene.find(first)->transform.position(), moved);
                         EXPECT_EQ(scene.find(first)->model, latestModel);
                         const auto redoCompiled = scene.find(first)->materialOverrides[0].compiled;
                         EXPECT_NE(redoCompiled, newCompiled);
                         ASSERT_TRUE(operations.undo(scene, clearColor));
                         const auto beforeFailure = scene.find(first)->materialOverrides[0].compiled;

                         const std::string materialName = "\"name\": \"Direction\",";
                         const auto material = modelText.find(materialName);
                         ASSERT_NE(material, std::string::npos);
                         modelText.insert(material + materialName.size(),
                                          R"("extensions":{"KHR_materials_unlit":{}},)");
                         modelText.insert(1, R"("extensionsUsed":["KHR_materials_unlit"],)");
                         std::ofstream(modelFile, std::ios::binary) << modelText;
                         EXPECT_FALSE(operations.reloadModel(scene, GLTF));
                         EXPECT_TRUE(operations.messageError());
                         EXPECT_NE(operations.message().find("material slot 0"), std::string::npos)
                             << operations.message();
                         EXPECT_NE(operations.message().find("not PBR"), std::string::npos) << operations.message();
                         EXPECT_FALSE(operations.isDirty());
                         EXPECT_EQ(operations.cachedModels(), cached);
                         EXPECT_EQ(scene.find(first)->model, latestModel);
                         EXPECT_EQ(scene.find(child)->model, latestModel);
                         EXPECT_EQ(scene.find(first)->materialOverrides[0].compiled, beforeFailure);
                         ASSERT_TRUE(operations.redo(scene, clearColor));
                         EXPECT_EQ(scene.find(first)->model, latestModel);
                         EXPECT_EQ(scene.find(first)->materialOverrides[0].compiled, redoCompiled);
                         ASSERT_TRUE(operations.addModel(scene, clearColor, GLTF));
                         ASSERT_TRUE(operations.setEntityShading(scene, clearColor, Render::ShadingMode::Pbr));
                         EXPECT_EQ(scene.find(operations.selected())->model, latestModel);
                     }, {}, {}});
        cleanValidation();
    }

    TEST_F(EditorWorkspaceDocsTest, NewVirtualModelsShareAndFailuresPreserveEditingStateUntilRetry)
    {
        copyShowcaseModels();
        fs::copy("assets/models/minimal_gltf", "assets/models/imported_gltf", fs::copy_options::recursive);
        fs::copy("assets/models/minimal_obj", "assets/models/imported_obj", fs::copy_options::recursive);
        Editor::EditorApp session(options());
        bool uiVerified = false;
        session.run(
            {[&](auto& app) {
                 Editor::EditorScene editor;
                 editor.initialize(app);
                 editor.select(app.scene().entities()[0].id);
                 const auto retry = [&](const char* model, const char* dep, const char* reason) {
                     const auto selected = editor.selected();
                     const auto size = app.scene().entities().size();
                     const auto cached = editor.cachedModels();
                     const std::string file(editor.scenePath());
                     EXPECT_FALSE(editor.addModel(app, model));
                     EXPECT_EQ(editor.selected(), selected);
                     EXPECT_EQ(editor.scenePath(), file);
                     EXPECT_EQ(app.scene().entities().size(), size);
                     EXPECT_EQ(editor.cachedModels(), cached);
                     EXPECT_NE(editor.message().find(model), std::string::npos);
                     EXPECT_NE(editor.message().find(reason), std::string::npos) << editor.message();
                     fs::remove(dep);
                     fs::rename(std::string(dep) + ".backup", dep);
                     ASSERT_TRUE(editor.addModel(app, model)) << editor.message();
                     const auto first = editor.selected();
                     EXPECT_EQ(app.scene().find(first)->name, "model");
                     const auto shared = app.scene().find(first)->model;
                     ASSERT_TRUE(editor.addModel(app, model));
                     EXPECT_NE(first, editor.selected());
                     EXPECT_EQ(app.scene().find(editor.selected())->model, shared);
                     EXPECT_EQ(editor.cachedModels(), cached + 1);
                 };
                 fs::rename("assets/models/imported_gltf/geometry/mesh.bin",
                            "assets/models/imported_gltf/geometry/mesh.bin.backup");
                 retry("models/imported_gltf/model.gltf", "assets/models/imported_gltf/geometry/mesh.bin", "mesh.bin");
                 fs::rename("assets/models/imported_obj/materials/model.mtl",
                            "assets/models/imported_obj/materials/model.mtl.backup");
                 retry("models/imported_obj/model.obj", "assets/models/imported_obj/materials/model.mtl", "model.mtl");
                 fs::copy("assets/models/minimal_gltf", "assets/models/bad_json", fs::copy_options::recursive);
                 fs::copy_file("assets/models/bad_json/model.gltf", "assets/models/bad_json/model.gltf.backup");
                 std::ofstream("assets/models/bad_json/model.gltf") << "{invalid";
                 retry("models/bad_json/model.gltf", "assets/models/bad_json/model.gltf", "Invalid glTF JSON");
                 fs::copy("assets/models/minimal_gltf", "assets/models/missing_png", fs::copy_options::recursive);
                 fs::rename(std::string("assets/") + PNG, std::string("assets/") + PNG + ".backup");
                 retry("models/missing_png/model.gltf", "assets/textures/direction_rgba.png", "direction_rgba.png");
                 ASSERT_TRUE(editor.save(app, "new-models.json"));
                 const auto desc = Scene::readScene(app.fileIO(), "new-models.json");
                 EXPECT_EQ(desc.entities.back().model->path(), "models/missing_png/model.gltf");
                 EXPECT_EQ(desc.entities.back().model->options(),
                           app.scene().entities().back().model->source().options());
             },
             {},
             [&](auto& app) {
                 if (uiVerified)
                     return;
                 uiVerified = true;
                 // 在同一会话 ui() 中补充验证 save/load/clear 后同帧 extractSceneFrame 与损坏文件失败保留语义。
                 Editor::EditorScene editor;
                 editor.initialize(app);
                 EditorTest::setSharedEditorScene(app, editor, false);
                 const auto original = app.scene().entities()[0].id;
                 const Scene::Transform edited({2, 1, 0}, {1, 0, 0, 0}, {-2, 0, 3});
                 ASSERT_TRUE(app.scene().setTransform(original, edited));
                 ASSERT_TRUE(editor.save(app, "same-frame.json"));
                 app.scene().setTransform(original, Scene::Transform{});
                 ASSERT_TRUE(editor.load(app, "same-frame.json"));
                 EXPECT_TRUE(app.scene().find(original) == nullptr && !editor.selected().valid());
                 const auto sameFrame = Scene::extractSceneFrame(app.scene(), {640, 480}, false);
                 ASSERT_TRUE(sameFrame);
                 EXPECT_EQ(sameFrame->draws[0].model, edited.matrix() * app.scene().entities()[0].model->draws()[0].model);

                 auto invalidModel = Scene::captureScene(app.scene(), app.clearColor());
                 invalidModel.entities[0].model = Model::ModelReference("models/missing.gltf");
                 Scene::writeScene(app.fileIO(), "missing-model.json", invalidModel);
                 EXPECT_TRUE(!editor.load(app, "missing-model.json") && editor.scenePath() == "same-frame.json" &&
                             app.scene().entities().size() == 2u);

                 editor.clear(app);
                 const auto cleared = Scene::extractSceneFrame(app.scene(), {640, 480});
                 ASSERT_TRUE(cleared && cleared->draws.empty());
                 app.requestExit();
             }});
        EXPECT_TRUE(uiVerified);
        cleanValidation();
    }

    TEST_F(EditorWorkspaceDocsTest, UnsavedChangesDialogGuardsDirtyStateOpenClearExitAndWindowClose)
    {
        copyShowcaseModels();
        const auto cfg = options(true, 1280, 800);
        {
            Editor::EditorApp session(cfg);
            Editor::EditorScene editor;
            int frame = 0;
            session.run({[&](auto& app) {
                             editor.initialize(app, "assets/scenes/mixed.json");
                             EXPECT_FALSE(editor.isDirty());
                             EXPECT_EQ(editor.statusText().find(" * |"), std::string::npos);
                             editor.requestClose(app);
                             EXPECT_FALSE(app.window().shouldClose());
                             editor.requestOpen(app);
                             EXPECT_EQ(ImGui::FindWindowByName("Unsaved changes"), nullptr);
                             editor.cancelFileAction();
                         },
                         [&](auto& app, const auto&) {
                             if (++frame > 40) { ADD_FAILURE() << "未保存拦截超时"; app.requestExit(); return; }
                             if (frame == 2)
                             {
                                 ASSERT_TRUE(editor.addModel(app, GLTF));
                                 EXPECT_TRUE(editor.isDirty() && editor.statusText().find("assets/scenes/mixed.json * |") != std::string::npos);
                                 editor.requestOpen(app);
                             }
                             else if (frame == 5) { EXPECT_TRUE(ImGui::FindWindowByName("Open scene") == nullptr && editor.isDirty()); editor.requestClear(app); }
                             else if (frame == 8)
                             {
                                 EXPECT_TRUE(app.scene().entities().empty());
                                 ASSERT_TRUE(editor.load(app, "assets/scenes/mixed.json"));
                                 EXPECT_TRUE(!editor.isDirty() && editor.statusText().find(" * |") == std::string::npos);
                                 ASSERT_TRUE(editor.save(app, "guarded-work.json"));
                                 ASSERT_TRUE(editor.addModel(app, GLTF));
                                 EXPECT_TRUE(editor.isDirty());
                                 editor.requestOpen(app);
                             }
                             else if (frame == 11) { EXPECT_TRUE(!editor.isDirty() && ImGui::FindWindowByName("Open scene") != nullptr); editor.cancelFileAction(); }
                             else if (frame >= 13) app.requestExit();
                         },
                         [&](auto& app) {
                             if (frame == 4) activateWindowButton("Unsaved changes", "Cancel");
                             else if (frame == 7) activateWindowButton("Unsaved changes", "Discard");
                             else if (frame == 10) activateWindowButton("Unsaved changes", "Save");
                             editor.ui(app);
                         },
                         [&](auto& app) { editor.cancelInteraction(app); },
                         [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
                         [&](auto& app) { editor.layout(app); },
                         [&](auto& app) { editor.requestClose(app); }});
            EXPECT_EQ(frame, 13);
        }
        {
            Editor::EditorApp closeSession(cfg);
            Editor::EditorScene closeEditor;
            int closeFrame = 0;
            bool closeSaved = false;
            closeSession.run({[&](auto& app) {
                                  closeEditor.initialize(app, "");
                                  EXPECT_TRUE(closeEditor.scenePath().empty() && !closeEditor.isDirty());
                                  ASSERT_TRUE(closeEditor.addModel(app, GLTF));
                                  EXPECT_TRUE(closeEditor.isDirty());
                                  EXPECT_NE(closeEditor.statusText().find("(unsaved) * |"), std::string::npos);
                              },
                              [&](auto& app, const auto&) {
                                  if (++closeFrame > 40) { ADD_FAILURE() << "WM_CLOSE 拦截超时"; app.requestExit(); return; }
                                  if (closeFrame == 2 || closeFrame == 7)
                                      PostMessageW(glfwGetWin32Window(static_cast<GLFWwindow*>(app.window().getNativeHandle())), WM_CLOSE, 0, 0);
                              },
                              [&](auto& app) {
                                  if (closeFrame == 4 || closeFrame == 9) activateWindowButton("Unsaved changes", "Save");
                                  else if (closeFrame == 6) activateWindowButton("Save scene as", "Cancel");
                                  else if (closeFrame == 11) { closeEditor.operationPath("saved-on-close.json"); activateWindowButton("Save scene as", "Save"); closeSaved = true; }
                                  closeEditor.ui(app);
                              },
                              [&](auto& app) { closeEditor.cancelInteraction(app); },
                              [&](auto& app, auto extent, const auto& input) { closeEditor.input(app, extent, input); },
                              [&](auto& app) { closeEditor.layout(app); }, [&](auto& app) { closeEditor.requestClose(app); }});
            EXPECT_TRUE(closeSaved && !closeEditor.isDirty() && fs::exists("saved-on-close.json") && closeEditor.scenePath() == "saved-on-close.json");
        }
        cleanValidation();
    }

    TEST_F(EditorWorkspaceDocsTest, HistoryStackFramesAndShadowCastersProtectCollectUnusedUntilLoad)
    {
        copyShowcaseModels();
        using Review = Render::Detail::RendererReviewAccess;
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
        auto editor = std::make_shared<Editor::EditorScene>();
        int frame = 0;
        Review::ResourceCounts baseline{};
        std::weak_ptr<const Model::PreparedModel> removed;
        Render::Mesh mesh;
        Scene::EntityId source;
        session.run(
            {[&](auto& app) {
                 editor->initialize(app, "assets/scenes/shadows.json");
                 app.shadowSettings().enabled = true;
                 baseline = Review::resourceCounts(app.renderer());
             },
             [&](auto& app, const auto&) { if (++frame >= 5) app.requestExit(); },
             [&](auto& app) {
                 editor->ui(app);
                 if (frame == 1)
                 {
                     ASSERT_TRUE(editor->addModel(app, GLTF));
                     source = editor->selected();
                     const auto model = app.scene().find(source)->model;
                     removed = model;
                     mesh = model->meshes()[0];
                     EXPECT_EQ(editor->cachedModels(), 4u);
                     const auto duplicate = editor->duplicateSelected(app);
                     EXPECT_EQ(app.scene().find(duplicate)->model, model);
                     EXPECT_EQ(editor->collectUnusedModels(), 0u);
                 }
                 else if (frame == 2)
                 {
                     // 独家边界 #4：删除实例后历史栈 + 阴影投射仍保护模型，Undo 恢复 0 打开/上传，并保持当前帧在 GPU 槽中。
                     editor->deleteSelected(app);
                     editor->select(source);
                     editor->deleteSelected(app);
                     EXPECT_EQ(editor->collectUnusedModels(), 0u);
                     EXPECT_FALSE(removed.expired());
                     EXPECT_TRUE(mesh.valid());
                     EXPECT_GT(Review::resourceCounts(app.renderer()).meshes, baseline.meshes);
                     const auto snap = Scene::extractSceneFrame(app.scene(), {384, 256}, true, app.shadowSettings());
                     ASSERT_TRUE(snap && !snap->shadowDraws.empty());
                     const auto opens = state->opens, uploads = count("Uploaded mesh:");
                     ASSERT_TRUE(editor->undo(app));
                     ASSERT_NE(app.scene().find(source), nullptr);
                     EXPECT_EQ(state->opens, opens);
                     EXPECT_EQ(count("Uploaded mesh:"), uploads);
                 }
                 else if (frame == 3)
                 {
                     editor->select(source);
                     editor->deleteSelected(app);
                     EXPECT_FALSE(editor->load(app, "missing-scene.json"));
                     EXPECT_FALSE(editor->save(app, "missing-dir/bad.json"));
                     EXPECT_TRUE(editor->canUndo());

                     // loadScene 清空历史栈使 GLTF+ground 可清理，飞行中 GPU 槽仍保活 mesh 直至 waitForFrames。
                     ASSERT_TRUE(editor->load(app, "assets/scenes/mixed.json"));
                     EXPECT_FALSE(editor->canUndo() || editor->canRedo() || editor->isDirty());
                     EXPECT_EQ(editor->collectUnusedModels(), 2u);
                     EXPECT_TRUE(removed.expired());
                     EXPECT_EQ(editor->cachedModels(), 2u);
                     EXPECT_TRUE(mesh.valid());
                     Review::waitForFrames(app.renderer());
                     EXPECT_FALSE(mesh.valid());
                 }
                 else if (frame == 4)
                 {
                     ASSERT_TRUE(editor->addModel(app, GLTF));
                     EXPECT_EQ(editor->cachedModels(), 3u);
                     editor->deleteSelected(app);
                     EXPECT_EQ(editor->collectUnusedModels(), 0u);
                     const auto otherId = app.scene().entities().front().id;
                     for (size_t s = 0; s < Editor::EditorOperations::MAX_HISTORY; ++s)
                         ASSERT_TRUE(app.scene().setTransform(otherId, Scene::Transform({float(s + 1), 0, 0})));
                     editor->clear(app);
                     EXPECT_EQ(editor->collectUnusedModels(), 0u);
                     ASSERT_TRUE(editor->save(app, "empty-history-reset.json"));
                     ASSERT_TRUE(editor->load(app, "empty-history-reset.json"));
                     EXPECT_EQ(editor->collectUnusedModels(), 3u);
                     EXPECT_EQ(editor->cachedModels(), 0u);
                     Review::waitForFrames(app.renderer());
                     const auto empty = Review::resourceCounts(app.renderer());
                     EXPECT_TRUE(empty.meshes == 0u && empty.textures == 0u && empty.materials == 0u);
                 }
             },
             {}, {}, [editor](auto& app) { editor->layout(app); }});
        cleanValidation();
    }

    TEST_F(EditorWorkspaceDocsTest, StartupFailuresRouteGuardsAndShaderErrorCleanup)
    {
        {
            const auto type = static_cast<ResourceType>(0x12345678);
            auto& manager = ResourceManager::instance();
            auto destroyed = std::make_shared<bool>(false);
            manager.registerLoader(type, std::make_unique<OriginalLoader>(type, destroyed));
            auto state = std::make_shared<Lifetime>();
            EXPECT_THROW(Editor::EditorApp(options(), std::make_unique<TrackedFileIO>(state)), Error::Exception);
            EXPECT_TRUE(state->ioDestroyed && !state->routesEmptyAtIoClose && !*destroyed && manager.hasLoader(type));
            EXPECT_FALSE(manager.hasLoader(ResourceType::SHADER) || manager.hasLoader(ResourceType::TEXTURE) ||
                         manager.hasLoader(ResourceType::MODEL));
            { const auto hold = manager.hold<OriginalResource>(type, "original.txt"); ASSERT_TRUE(hold); EXPECT_EQ(hold.get<OriginalResource>()->type(), type); }
            manager.reset();
            EXPECT_TRUE(*destroyed);
        }
        EXPECT_EQ(count("Created Vulkan"), 0u);
        {
            auto state = std::make_shared<Lifetime>();
            auto cfg = options(); cfg.vertexShader = "shaders/missing.vert.spv";
            EXPECT_THROW(Editor::EditorApp(cfg, std::make_unique<TrackedFileIO>(state)), Error::Exception);
            closed(state); unloaded(ResourceType::SHADER, "shaders/missing.vert.spv"); unloaded(ResourceType::SHADER, "shaders/cube.frag.spv");
        }
        {
            auto io = NativeIO::createPlatformFileIO();
            Scene::SceneDescription desc;
            desc.entities.push_back({Model::ModelReference("models/missing.gltf"), Scene::Transform{}, true});
            Scene::writeScene(*io, "missing-model-scene.json", desc);
            auto state = std::make_shared<Lifetime>();
            Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
            auto callbacks = Editor::makeCallbacks("missing-model-scene.json");
            int updates = 0; callbacks.update = [&](auto&, const auto&) { ++updates; };
            EXPECT_THROW(session.run(std::move(callbacks)), Error::Exception);
            EXPECT_TRUE(updates == 0 && !session.isOpen());
            closed(state);
        }
        {
            const fs::path shadowSrc = "assets/shaders/shadow.vert.spv", shadowBak = "assets/shaders/shadow.vert.spv.bak";
            fs::rename(shadowSrc, shadowBak);
            Editor::EditorApp session(options());
            EXPECT_THROW(session.run({[](auto& app) { app.shadowSettings().enabled = true; }, {}, {}, {}, {}, [](auto& app) { app.sceneViewport("Scene"); }}),
                         Error::Exception);
            EXPECT_TRUE(!session.isOpen() && ResourceManager::instance().loadersEmpty());
            fs::rename(shadowBak, shadowSrc);
        }
        cleanValidation();
    }

    TEST_F(EditorWorkspaceDocsTest, WorkspacePersistsDockingResetAndStartupCustomScene)
    {
        copyShowcaseModels();
        for (int pass = 0; pass < 2; ++pass)
        {
            Editor::EditorApp session(options(true, pass ? 1280 : 1920, pass ? 720 : 1080));
            auto demo = std::make_shared<Editor::EditorScene>();
            int frame = 0;
            session.run({[demo](auto& app) { demo->initialize(app); demo->select(app.scene().entities()[0].id); },
                         [&](auto& app, const auto&) { if (++frame >= 7) app.requestExit(); },
                         [&, demo](auto& app) {
                             demo->ui(app);
                             if (frame == 2) ImGui::SetWindowFocus("Assets");
                             for (const char* name : {"Objects", "Assets", "Properties", "Scene"})
                             { const auto* p = ImGui::FindWindowByName(name); ASSERT_NE(p, nullptr); EXPECT_NE(p->DockId, 0u) << name; }
                             EXPECT_TRUE(ImGui::FindWindowByName("Files") == nullptr && ImGui::FindWindowByName("Status") == nullptr);
                             if (frame >= 5)
                             {
                                 const auto size = ImGui::GetMainViewport()->Size;
                                 EXPECT_TRUE(app.viewportRegion().size.y > size.y * 0.7f && app.viewportRegion().size.x > size.x * 0.4f);
                             }
                             if (frame == 3)
                             {
                                 const auto n = app.scene().entities().size();
                                 demo->restoreLayout();
                                 EXPECT_EQ(app.scene().entities().size(), n);
                                 ImGui::SaveIniSettingsToDisk("incubator-editor-layout.ini");
                             }
                             glfwWaitEventsTimeout(0.04);
                         },
                         {}, {}, [demo](auto& app) { demo->layout(app); }});
            EXPECT_TRUE(fs::exists("incubator-editor-layout.ini"));
        }

        ResourceRegistry::instance().clear();
        auto io = NativeIO::createPlatformFileIO();
        Scene::SceneDescription content;
        content.entities = {{Model::ModelReference(GLTF), Scene::Transform({1, 2, 0}), false},
                            {std::nullopt, Scene::Transform({4, 5, 6}), true}};
        content.camera = Scene::Camera({0, 1, 9});
        content.lighting = {{0, -2, -3}, {0.2f, 0.6f, 0.8f}, 2, 0.3f};
        content.clearColor = {0.2f, 0.3f, 0.4f, 1};
        Scene::writeScene(*io, "custom-scene.json", content);
        std::ofstream("incubator.json") << R"({"window":{"title":"配置窗口","width":640,"height":480},"startupScene":"custom-scene.json"})";
        auto cfg = App::readStartupConfig(*io);
        cfg.options.window.visible = false;
        cfg.options.validation = Render::RendererValidationMode::Required;
        Editor::EditorApp session(cfg.options, std::move(io));
        session.run({[&](auto& app) {
                         Editor::EditorScene demo;
                         demo.initialize(app, cfg.startupScene);
                         EXPECT_TRUE(demo.scenePath() == "custom-scene.json" && app.scene().entities().size() == 2u &&
                                     !app.scene().entities()[0].visible && app.scene().entities()[1].model == nullptr &&
                                     app.scene().camera().position() == content.camera.position() &&
                                     app.scene().lighting() == content.lighting && app.clearColor() == content.clearColor);
                         EXPECT_FALSE(ResourceRegistry::instance().lookup(ResourceType::MODEL, DEMO_GLTF).valid() ||
                                      ResourceRegistry::instance().lookup(ResourceType::MODEL, DEMO_OBJ).valid());
                         demo.select(app.scene().entities()[0].id);
                         EXPECT_FALSE(demo.load(app, "missing-scene.json"));
                         EXPECT_TRUE(demo.scenePath() == "custom-scene.json" && demo.selected().valid());
                         ASSERT_TRUE(demo.save(app, "另存场景.json"));
                         EXPECT_EQ(demo.scenePath(), "另存场景.json");
                         EXPECT_FALSE(demo.save(app, "missing-directory/scene.json"));
                         EXPECT_EQ(demo.scenePath(), "另存场景.json");
                         ASSERT_TRUE(demo.load(app, "custom-scene.json"));
                         EXPECT_TRUE(demo.scenePath() == "custom-scene.json" && !demo.selected().valid());
                         app.requestExit();
                     },
                     {}, {}});
        cleanValidation();
    }

    TEST_F(EditorWorkspaceDocsTest, InitializeUpdateUiCallbacksMinimizedAndExitLifecycle)
    {
        for (int phase = 0; phase < 3; ++phase)
        {
            auto state = std::make_shared<Lifetime>();
            Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
            auto hold = std::make_shared<CallbackHold>(state);
            int updates = 0, uis = 0;
            Editor::Callbacks cb{[&, hold](auto& app) {
                                     auto m = Model::prepareModel(app.renderer(), GLTF);
                                     track(app, m, state);
                                     app.scene().createEntity(m);
                                     hold->cpu = ResourceManager::instance().hold<TextureResource>(ResourceType::TEXTURE, PNG);
                                     if (phase == 0) throw std::runtime_error("init fail");
                                 },
                                 [&, hold](auto&, const auto&) { ++updates; if (phase == 1) throw std::runtime_error("update fail"); },
                                 [&, hold](auto&) { ++uis; throw std::runtime_error("ui fail"); }};
            hold.reset();
            EXPECT_THROW(session.run(std::move(cb)), std::runtime_error);
            EXPECT_TRUE(!session.isOpen() && updates == (phase == 0 ? 0 : 1) && uis == (phase == 2 ? 1 : 0));
            closed(state);
            unloaded(ResourceType::MODEL, GLTF);
            unloaded(ResourceType::TEXTURE, PNG);
        }

        for (const int mode : {1, 2, 3})
        {
            auto state = std::make_shared<Lifetime>();
            int updates = 0, uis = 0;
            {
                Editor::EditorApp session(options(), std::make_unique<TrackedFileIO>(state));
                auto m = Model::prepareModel(session.renderer(), GLTF);
                track(session, m, state);
                session.scene().createEntity(m);
                m.reset();
                if (mode < 3)
                    session.run({{},
                                 [&](auto& app, const auto&) { if (++updates == 3 && mode == 1) app.window().setShouldClose(true); },
                                 [&](auto& app) { ++uis; if (mode == 2) app.requestExit(); }});
                else session.requestExit();
            }
            EXPECT_TRUE(updates == (mode == 1 ? 3 : mode == 2 ? 1 : 0) && uis == (mode == 1 ? 2 : mode == 2 ? 1 : 0));
            closed(state);
        }

        // 独家边界 #5：窗口最小化 (0x0) 期间持续推进 update(dt) 与逻辑，跳过 ui/layout，恢复新尺寸后重建交换链。
        {
            auto state = std::make_shared<Lifetime>();
            Editor::EditorApp session(options(true), std::make_unique<TrackedFileIO>(state));
            std::vector<Editor::FrameTime> times;
            std::vector<int> uiUpdates;
            int updates = 0, cancellations = 0;
            session.run({[&](auto& app) {
                             const auto m = Model::prepareModel(app.renderer(), GLTF);
                             track(app, m, state);
                             app.scene().setCamera(Scene::Camera({0, 0, 1}, {1, 0, 0, 0}, {glm::radians(90.0f), 0.1f, 10}));
                             app.scene().createEntity(m, Scene::Transform({0, 0, 0}, {1, 0, 0, 0}, glm::vec3(0.5f)));
                         },
                         [&](auto& app, const Editor::FrameTime& time) {
                             const int frame = updates++;
                             if (frame >= 30)
                             {
                                 ADD_FAILURE() << "Minimized lifecycle timeout";
                                 app.requestExit();
                                 return;
                             }
                             times.push_back(time);
                             EXPECT_GE(time.delta, Clock::Duration::zero());
                             if (times.size() > 1) EXPECT_EQ(time.delta, time.now - times[times.size() - 2].now);
                             app.scene().createEntity();
                             auto* handle = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                             if (frame == 1)
                             {
                                 glfwIconifyWindow(handle);
                                 for (int i = 0; i < 100 && !app.window().isIconified(); ++i, glfwWaitEventsTimeout(0.005)) app.window().pollEvents();
                                 ASSERT_TRUE(app.window().isIconified());
                             }
                             else if (frame == 5)
                             {
                                 EXPECT_TRUE(uiUpdates == (std::vector<int>{0}) && app.scene().entities().size() == 7u);
                                 glfwRestoreWindow(handle);
                                 for (int i = 0; i < 100 && app.window().isIconified(); ++i, glfwWaitEventsTimeout(0.005)) app.window().pollEvents();
                                 ASSERT_FALSE(app.window().isIconified());
                                 glfwSetWindowSize(handle, 800, 400);
                             }
                             else if (frame == 10)
                             {
                                 int w = 0, h = 0;
                                 app.window().getFramebufferSize(w, h);
                                 EXPECT_TRUE(w == 800 && h == 400);
                                 const auto restored = Scene::extractSceneFrame(app.scene(), {uint32_t(w), uint32_t(h)});
                                 ASSERT_TRUE(restored && !restored->draws.empty());
                                 app.requestExit();
                             }
                         },
                         [&](auto& app) { uiUpdates.push_back(updates - 1); EXPECT_FALSE(app.window().isIconified()); glfwWaitEventsTimeout(0.04); },
                         [&](auto&) { ++cancellations; }});
            EXPECT_TRUE(updates == 11 && cancellations >= 4 && uiUpdates == (std::vector<int>{0, 5, 6, 7, 8, 9}));
            closed(state);
        }
        cleanValidation();
    }
    TEST_F(EditorWorkspaceDocsTest, AssetsImportCancelRefreshAddSaveAndReopen)
    {
        const auto external = sandbox_ / "external";
        fs::copy("assets/models/minimal_gltf", external, fs::copy_options::recursive);
        fs::create_directories(external / "textures");
        fs::copy_file(fs::path("assets") / PNG, external / PNG);
        const auto modelFile = external / "model.gltf";
        std::ifstream input(modelFile, std::ios::binary);
        std::string text{std::istreambuf_iterator<char>(input), {}};
        input.close();
        const auto uri = text.find("../../textures/direction_rgba.png");
        ASSERT_NE(uri, std::string::npos);
        text.erase(uri, 6);
        std::ofstream(modelFile, std::ios::binary) << text;

        Editor::EditorApp session(options(true, 1280, 800));
        int pickerCalls = 0;
        Editor::EditorScene editor([&](Editor::EditorApp&) -> std::optional<fs::path> {
            if (++pickerCalls == 1)
                return std::nullopt;
            return modelFile;
        });
        constexpr auto imported = "models/model/model.gltf";
        std::string originalMessage;
        Scene::EntityId addedId{};
        std::shared_ptr<const Model::PreparedModel> oldModel;
        const glm::vec3 edited{2, 3, 4};
        int step = 0;
        int frames = 0;
        const auto hasNote = [&] {
            return std::ranges::any_of(editor.assetEntries(), [](const auto& entry) { return entry.name == "note.txt"; });
        };
        session.run({[&](auto& app) {
                         editor.initialize(app, "");
                         originalMessage = editor.message();
                     },
                     [&](auto& app, const auto&) {
                         if (++frames > 120)
                         {
                             ADD_FAILURE() << "Assets import timed out at step " << step;
                             app.requestExit();
                             return;
                         }
                         auto* panel = ImGui::FindWindowByName("Assets");
                         if (!panel || frames < 4)
                             return;
                         if (step == 0)
                         {
                             ImGui::SetWindowFocus("Assets");
                             ++step;
                             return;
                         }
                         if (panel->Hidden)
                             return;
                         switch (step)
                         {
                             case 1:
                                 ImGui::ActivateItemByID(panel->GetID("Import..."));
                                 break;
                             case 2:
                                 EXPECT_EQ(pickerCalls, 1);
                                 EXPECT_EQ(editor.message(), originalMessage);
                                 EXPECT_FALSE(editor.controller().messageError());
                                 EXPECT_TRUE(editor.assetDirectory().empty());
                                 EXPECT_TRUE(editor.controller().focusedAssetPath().empty());
                                 EXPECT_FALSE(fs::exists("assets/models/model"));
                                 ImGui::ActivateItemByID(panel->GetID("Import..."));
                                 break;
                             case 3:
                                 EXPECT_EQ(pickerCalls, 2);
                                 EXPECT_EQ(editor.assetDirectory(), "models/model");
                                 EXPECT_EQ(editor.controller().focusedAssetPath(), imported);
                                 EXPECT_EQ(editor.message(), std::string("Imported: ") + imported);
                                 EXPECT_TRUE(app.scene().entities().empty());
                                 EXPECT_FALSE(editor.isDirty());
                                 EXPECT_EQ(editor.cachedModels(), 0u);
                                 std::ofstream("assets/models/model/note.txt") << "refresh";
                                 EXPECT_FALSE(hasNote());
                                 ImGui::ActivateItemByID(panel->GetID("Refresh"));
                                 break;
                             case 4:
                                 EXPECT_TRUE(hasNote());
                                 EXPECT_EQ(editor.controller().focusedAssetPath(), imported);
                                 ImGui::ActivateItemByID(assetRowAction(*panel, imported, "Add"));
                                 break;
                             case 5: {
                                 ASSERT_EQ(app.scene().entities().size(), 1u) << editor.message();
                                 addedId = app.scene().entities().front().id;
                                 oldModel = app.scene().find(addedId)->model;
                                 EXPECT_EQ(oldModel->source().path(), imported);
                                 EXPECT_TRUE(editor.isDirty());
                                 ASSERT_TRUE(editor.save(app, "imported-scene.json"));
                                 ASSERT_TRUE(editor.controller().operations().setTransform(
                                     app.scene(), app.clearColor(), Scene::Transform(edited)));
                                 const auto importedFile = fs::path("assets") / imported;
                                 std::ifstream inFile(importedFile, std::ios::binary);
                                 std::string gltfText{std::istreambuf_iterator<char>(inFile), {}};
                                 inFile.close();
                                 const auto translation = gltfText.find("0.25");
                                 ASSERT_NE(translation, std::string::npos);
                                 gltfText.replace(translation, 4, "0.75");
                                 std::ofstream(importedFile, std::ios::binary) << gltfText;
                                 ImGui::ActivateItemByID(assetRowAction(*panel, imported, "Reload"));
                                 break;
                             }
                             case 6: {
                                 const auto* entity = app.scene().find(addedId);
                                 ASSERT_NE(entity, nullptr);
                                 const auto reloaded = entity->model;
                                 ASSERT_NE(reloaded, oldModel) << editor.message();
                                 EXPECT_NE(reloaded->draws().front().model, oldModel->draws().front().model);
                                 EXPECT_EQ(entity->transform.position(), edited);
                                 EXPECT_EQ(editor.cachedModels(), 1u);
                                 EXPECT_TRUE(editor.isDirty());
                                 EXPECT_FALSE(editor.controller().messageError());
                                 ASSERT_TRUE(editor.undo(app));
                                 EXPECT_EQ(app.scene().find(addedId)->transform.position(), glm::vec3(0));
                                 EXPECT_EQ(app.scene().find(addedId)->model, reloaded);
                                 EXPECT_FALSE(editor.isDirty());
                                 ASSERT_TRUE(editor.save(app, "imported-scene.json"));
                                 editor.clear(app);
                                 ASSERT_TRUE(app.scene().entities().empty());
                                 ASSERT_TRUE(editor.load(app, "imported-scene.json", true));
                                 ASSERT_EQ(app.scene().entities().size(), 1u);
                                 EXPECT_EQ(app.scene().entities().front().model->source().path(), imported);
                                 EXPECT_FALSE(editor.isDirty());
                                 app.requestExit();
                                 break;
                             }
                         }
                         ++step;
                     },
                     [&](auto& app) { editor.ui(app); },
                     {}, {}, [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(step, 7);
        cleanValidation();
    }

    TEST_F(EditorWorkspaceDocsTest, ToolbarBreadcrumbsTransformResetAndSceneFileBrowserUnderValidation)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(true, 1280, 800));
            Editor::EditorScene editor;
            Scene::EntityId firstId, secondId;
            int step = 0, totalFrames = 0;
            bool done = false;
            WindowCapture::Image tImg, rImg, sImg;

            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/mixed.json");
                     app.shadowSettings().enabled = true;
                     ASSERT_GE(app.scene().entities().size(), 2u);
                     firstId = app.scene().entities()[0].id;
                     secondId = app.scene().entities()[1].id;
                     editor.select(app, firstId);
                     editor.controller().operations().setTransform(
                         app.scene(), app.clearColor(),
                         Scene::Transform({2.0f, 3.0f, 4.0f},
                                          glm::angleAxis(0.6f, glm::vec3(0.0f, 1.0f, 0.0f)),
                                          {2.0f, 2.0f, 2.0f}));
                     fs::create_directories("review");
                 },
                 [&](auto& app, const auto&) {
                     if (++totalFrames > 160)
                     {
                         ADD_FAILURE() << "Task04 UI test timeout step=" << step;
                         app.requestExit();
                         return;
                     }
                     auto* toolbarWin = ImGui::FindWindowByName("##toolbar");
                     auto* propsWin = ImGui::FindWindowByName("Properties");
                     auto* objectsWin = ImGui::FindWindowByName("Objects");
                     auto* assetsWin = ImGui::FindWindowByName("Assets");
                     if (!toolbarWin || !propsWin || !objectsWin || !assetsWin)
                         return;

                     switch (step)
                     {
                         case 0:
                             // 采集 Translate 模式布局截图并点击工具栏 Rotate (E)。
                             tImg = WindowCapture::capture(app.window());
                             WindowCapture::save(tImg, "review/ui-translate.ppm");
                             ImGui::ActivateItemByID(toolbarWin->GetID("Rotate (E)"));
                             ++step;
                             break;
                         case 1:
                             EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Rotate);
                             rImg = WindowCapture::capture(app.window());
                             WindowCapture::save(rImg, "review/ui-rotate.ppm");
                             ImGui::ActivateItemByID(toolbarWin->GetID("Scale (R)"));
                             ++step;
                             break;
                         case 2:
                             EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale);
                             sImg = WindowCapture::capture(app.window());
                             WindowCapture::save(sImg, "review/ui-scale.ppm");
                             // 验证 Properties 分项重置按钮（R##reset-pos/rot/scale）。
                             ImGui::ActivateItemByID(propsWin->GetID("R##reset-pos"));
                             ++step;
                             break;
                         case 3:
                             EXPECT_EQ(app.scene().find(firstId)->transform.position(), glm::vec3(0.0f));
                             ImGui::ActivateItemByID(propsWin->GetID("R##reset-rot"));
                             ++step;
                             break;
                         case 4:
                             EXPECT_NEAR(std::abs(app.scene().find(firstId)->transform.rotation().w), 1.0f, 1e-4f);
                             ImGui::ActivateItemByID(propsWin->GetID("R##reset-scale"));
                             ++step;
                             break;
                         case 5:
                             EXPECT_EQ(app.scene().find(firstId)->transform.scale(), glm::vec3(1.0f));
                             // 验证 Objects 列表内联 ##vis 不抢占当前选中实体。
                             if (!objectsWin->DC.ChildWindows.empty())
                             {
                                 auto* listChild = objectsWin->DC.ChildWindows[0];
                                 const ImGuiID secondVisId = ImHashStr(
                                     "##vis", 0, listChild->GetID(std::to_string(secondId.value()).c_str()));
                                 ImGui::ActivateItemByID(secondVisId);
                                 ++step;
                             }
                             break;
                         case 6:
                             EXPECT_EQ(editor.selected(), firstId);
                             EXPECT_FALSE(app.scene().find(secondId)->visible);
                             // 验证 Assets 进入子目录后切换标签页焦点并通过面包屑导航跳回根 "/"。
                             editor.browseDirectory("models/avocado/glTF");
                             ImGui::SetWindowFocus("Assets");
                             ++step;
                             break;
                         case 7:
                             EXPECT_EQ(editor.assetDirectory(), "models/avocado/glTF");
                             {
                                 const ImGuiID rootCrumb =
                                     ImHashStr("/", 0, assetsWin->GetID("breadcrumb"));
                                 ImGui::ActivateItemByID(rootCrumb);
                             }
                             ++step;
                             break;
                         case 8:
                             EXPECT_TRUE(editor.assetDirectory().empty());
                             // 验证工具栏固定 ID 保存按钮与 Open... 弹窗场景文件列表。
                             ImGui::ActivateItemByID(toolbarWin->GetID("###toolbar-save"));
                             ++step;
                             break;
                         case 9:
                             EXPECT_FALSE(editor.isDirty());
                             ImGui::ActivateItemByID(toolbarWin->GetID("Open..."));
                             ++step;
                             break;
                         case 10:
                             if (auto* openWin = ImGui::FindWindowByName("Open scene");
                                 openWin && !openWin->DC.ChildWindows.empty())
                             {
                                 auto* filesChild = openWin->DC.ChildWindows[0];
                                 const auto& files = editor.controller().sceneFiles();
                                 EXPECT_FALSE(files.empty());
                                 if (!files.empty())
                                 {
                                     ImGui::ActivateItemByID(filesChild->GetID(files.front().c_str()));
                                     ++step;
                                 }
                             }
                             break;
                         case 11:
                             EXPECT_EQ(std::string(editor.controller().pathBufferData().data()),
                                       editor.controller().sceneFiles().front());
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
            EXPECT_TRUE(done && tImg.width > 0 && rImg.width > 0 && sImg.width > 0);
        }
        cleanValidation();
    }

    TEST_F(EditorWorkspaceDocsTest, GlbImportMaterialOverridesPreservedAcrossSaveReopenAndReloadUnderValidation)
    {
        const auto writeTestGlb = [](const fs::path& path, float maskCutoff) {
            std::vector<uint8_t> bin(104, 0);
            const std::array<float, 9> positions{-0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f, 0.0f, 0.5f, 0.0f};
            const std::array<float, 6> uvs{0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f};
            const std::array<float, 9> normals{0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
            const std::array<uint16_t, 3> indices{0, 1, 2};
            std::memcpy(bin.data() + 0, positions.data(), sizeof(positions));
            std::memcpy(bin.data() + 36, uvs.data(), sizeof(uvs));
            std::memcpy(bin.data() + 60, normals.data(), sizeof(normals));
            std::memcpy(bin.data() + 96, indices.data(), sizeof(indices));

            std::string json = fmt::format(
                R"({{"asset":{{"version":"2.0"}},"scene":0,"scenes":[{{"nodes":[0]}}],"nodes":[{{"mesh":0}}],)"
                R"("buffers":[{{"byteLength":104}}],)"
                R"("bufferViews":[)"
                R"({{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962}},)"
                R"({{"buffer":0,"byteOffset":36,"byteLength":24,"target":34962}},)"
                R"({{"buffer":0,"byteOffset":60,"byteLength":36,"target":34962}},)"
                R"({{"buffer":0,"byteOffset":96,"byteLength":6,"target":34963}}],)"
                R"("accessors":[)"
                R"({{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-0.5,-0.5,0],"max":[0.5,0.5,0]}},)"
                R"({{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}},)"
                R"({{"bufferView":2,"componentType":5126,"count":3,"type":"VEC3"}},)"
                R"({{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}}],)"
                R"("materials":[)"
                R"({{"name":"MaskMat","alphaMode":"MASK","alphaCutoff":{:.2f},"doubleSided":true,"emissiveFactor":[0.25,0.5,0.75],"pbrMetallicRoughness":{{"baseColorFactor":[1,1,1,0.9],"metallicFactor":0.2,"roughnessFactor":0.8}}}},)"
                R"({{"name":"BlendMat","alphaMode":"BLEND","doubleSided":true,"pbrMetallicRoughness":{{"baseColorFactor":[0.8,0.9,1.0,0.4],"metallicFactor":0.1,"roughnessFactor":0.6}}}}],)"
                R"("meshes":[{{"primitives":[)"
                R"({{"attributes":{{"POSITION":0,"TEXCOORD_0":1,"NORMAL":2}},"indices":3,"material":0}},)"
                R"({{"attributes":{{"POSITION":0,"TEXCOORD_0":1,"NORMAL":2}},"indices":3,"material":1}}]}}]}})",
                maskCutoff);
            while (json.size() % 4 != 0)
                json.push_back(' ');

            const uint32_t jsonLen = static_cast<uint32_t>(json.size());
            const uint32_t binLen = static_cast<uint32_t>(bin.size());
            const uint32_t totalLen = 12u + 8u + jsonLen + 8u + binLen;
            std::vector<uint8_t> glb;
            glb.reserve(totalLen);
            const auto appendU32 = [&](uint32_t v) {
                const uint8_t bytes[4]{uint8_t(v & 0xFFu), uint8_t((v >> 8) & 0xFFu), uint8_t((v >> 16) & 0xFFu),
                                       uint8_t((v >> 24) & 0xFFu)};
                glb.insert(glb.end(), bytes, bytes + 4);
            };
            appendU32(0x46546C67u);
            appendU32(2u);
            appendU32(totalLen);
            appendU32(jsonLen);
            appendU32(0x4E4F534Au);
            glb.insert(glb.end(), json.begin(), json.end());
            appendU32(binLen);
            appendU32(0x004E4942u);
            glb.insert(glb.end(), bin.begin(), bin.end());

            fs::create_directories(path.parent_path());
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(glb.data()), std::streamsize(glb.size()));
            ASSERT_TRUE(out.good());
        };

        const fs::path externalGlb = sandbox_ / "external" / "extended_pbr.glb";
        writeTestGlb(externalGlb, 0.50f);
        constexpr const char* importedPath = "models/extended_pbr/extended_pbr.glb";

        {
            Editor::EditorApp session(options(true, 1280, 800));
            Editor::EditorScene editor([&](Editor::EditorApp&) -> std::optional<fs::path> { return externalGlb; });
            int frame = 0, step = 0;
            bool done = false;
            auto* handle = static_cast<GLFWwindow*>(session.window().getNativeHandle());
            enableGlfwCallbacks(handle, false);

            const auto checkMaterialExtensions = [&](const Scene::Entity& entity, float expectedCutoff) {
                ASSERT_TRUE(entity.model);
                ASSERT_EQ(entity.model->materialSlots().size(), 2u);
                const auto& slot0 = entity.model->materialSlots()[0].sourceDesc;
                EXPECT_EQ(slot0.alphaMode, Render::AlphaMode::Mask);
                EXPECT_NEAR(slot0.alphaCutoff, expectedCutoff, 1e-4f);
                EXPECT_NEAR(slot0.baseAlpha, 0.9f, 1e-4f);
                EXPECT_TRUE(slot0.doubleSided);
                EXPECT_EQ(slot0.emissiveFactor, (std::array{0.25f, 0.5f, 0.75f}));

                const auto& slot1 = entity.model->materialSlots()[1].sourceDesc;
                EXPECT_EQ(slot1.alphaMode, Render::AlphaMode::Blend);
                EXPECT_NEAR(slot1.baseAlpha, 0.4f, 1e-4f);
                EXPECT_TRUE(slot1.doubleSided);

                ASSERT_EQ(entity.materialOverrides.size(), 2u);
                const auto& compiled0 = Render::Detail::RendererReviewAccess::materialDescription(
                    entity.materialOverrides[0].compiled->material);
                EXPECT_EQ(compiled0.alphaMode, Render::AlphaMode::Mask);
                EXPECT_NEAR(compiled0.alphaCutoff, expectedCutoff, 1e-4f);
                EXPECT_NEAR(compiled0.baseAlpha, 0.9f, 1e-4f);
                EXPECT_TRUE(compiled0.doubleSided);
                EXPECT_EQ(compiled0.emissiveFactor, (std::array{0.25f, 0.5f, 0.75f}));
                EXPECT_EQ(compiled0.color, (std::array{0.4f, 0.6f, 0.8f}));

                const auto& compiled1 = Render::Detail::RendererReviewAccess::materialDescription(
                    entity.materialOverrides[1].compiled->material);
                EXPECT_EQ(compiled1.alphaMode, Render::AlphaMode::Blend);
                EXPECT_NEAR(compiled1.baseAlpha, 0.4f, 1e-4f);
                EXPECT_TRUE(compiled1.doubleSided);
                EXPECT_EQ(compiled1.color, (std::array{0.9f, 0.5f, 0.3f}));
            };

            session.run(
                {[&](auto& app) { editor.initialize(app, ""); },
                 [&](auto& app, const auto&) {
                     if (++frame > 180)
                     {
                         ADD_FAILURE() << "GLB extension editor test timeout at step=" << step;
                         app.requestExit();
                         return;
                     }
                     auto* assetsWin = ImGui::FindWindowByName("Assets");
                     auto* matWin = ImGui::FindWindowByName("Material");
                     if (frame < 5 || !assetsWin || !matWin || !app.sceneOutput().valid())
                         return;

                     switch (step)
                     {
                         case 0:
                             ImGui::SetWindowFocus("Assets");
                             if (assetsWin->Hidden)
                                 return;
                             ImGui::ActivateItemByID(assetsWin->GetID("Import..."));
                             ++step;
                             break;
                         case 1:
                             EXPECT_EQ(editor.controller().focusedAssetPath(), importedPath);
                             EXPECT_EQ(editor.message(), std::string("Imported: ") + importedPath);
                             ASSERT_EQ(editor.assetEntries().size(), 1u);
                             EXPECT_EQ(editor.assetEntries()[0].kind, Resource::AssetKind::Model);
                             ImGui::ActivateItemByID(assetRowAction(*assetsWin, importedPath, "Add"));
                             ++step;
                             break;
                         case 2:
                         {
                             ASSERT_EQ(app.scene().entities().size(), 1u) << editor.message();
                             const auto id = app.scene().entities()[0].id;
                             editor.select(app, id);
                             ASSERT_TRUE(editor.setEntityShading(app, Render::ShadingMode::Pbr));
                             EXPECT_TRUE(editor.controller().setMaterialOverride(
                                 app, {0u, {0.4f, 0.6f, 0.8f}, Render::MaterialParameterMode::FixedValue, 0.25f,
                                       Render::MaterialParameterMode::FixedValue, 0.9f}));
                             EXPECT_TRUE(editor.controller().setMaterialOverride(
                                 app, {1u, {0.9f, 0.5f, 0.3f}, Render::MaterialParameterMode::FixedValue, 0.3f,
                                       Render::MaterialParameterMode::FixedValue, 0.1f}));
                             checkMaterialExtensions(*app.scene().find(id), 0.50f);

                             // 打开 Material 面板展示源扩展摘要，不破坏槽位切换与控件 ID。
                             ImGui::SetWindowFocus("Material");
                             ++step;
                             break;
                         }
                         case 3:
                         {
                             if (matWin->Hidden)
                                 return;
                             EXPECT_NE(matWin->GetID("##mat-slot"), 0u);
                             EXPECT_NE(matWin->GetID("##mat-color"), 0u);

                             const std::string scenePath = "glb-extended-v12.json";
                             ASSERT_TRUE(editor.save(app, scenePath));
                             editor.clear(app);
                             ASSERT_TRUE(app.scene().entities().empty());
                             ASSERT_TRUE(editor.load(app, scenePath, true));
                             ASSERT_EQ(app.scene().entities().size(), 1u);
                             const auto reopenedId = app.scene().entities()[0].id;
                             editor.select(app, reopenedId);
                             checkMaterialExtensions(*app.scene().find(reopenedId), 0.50f);

                             // 磁盘改写 GLB（更新 alphaCutoff 为 0.35）后执行显式热重载，验证覆盖与新源属性一并保留。
                             writeTestGlb(fs::path("assets") / importedPath, 0.35f);
                             ASSERT_TRUE(editor.controller().reloadAsset(app, importedPath)) << editor.message();
                             checkMaterialExtensions(*app.scene().find(reopenedId), 0.35f);
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
}  // namespace
