#include "editor_test_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    using namespace Incubator::Test;
    namespace fs = std::filesystem;
    namespace NativeIO = Incubator::FileIO;

    class EditorInteractionTest : public EditorAppTest
    {};


    TEST_F(EditorInteractionTest, ViewportClickSelectsSharedInstancesAndPopupBlocksSelection)
    {
        copyShowcaseModels();
        Editor::EditorApp session(options(true, 1280, 800));
        Editor::EditorScene editor;
        std::array<glm::vec2, 3> points{};
        std::array<bool, 3> found{};
        Scene::EntityId first, second;
        ImVec2 popupPoint{};
        int stage = 0, frames = 0;
        bool issued = false, opened = false;
        session.run({[&](auto& app) {
                         editor.initialize(app);
                         EditorTest::setSharedEditorScene(app, editor, false);
                         first = app.scene().entities()[0].id;
                         second = app.scene().entities()[1].id;
                         editor.select({});
                         enableGlfwCallbacks(static_cast<GLFWwindow*>(app.window().getNativeHandle()));
                     },
                     [&](auto& app, const auto&) {
                         if (frames == 3) ImGui::SetWindowFocus("Objects");
                         if (++frames > 180) { ADD_FAILURE() << "视口点选与弹窗拦截超时 stage=" << stage; app.requestExit(); return; }
                         const auto& reg = app.viewportRegion();
                         if (frames < 5 || issued || reg.size.x <= 0 || reg.size.y <= 0) return;
                         if (!found[0] || !found[1] || !found[2])
                         {
                             for (int y = 1; y < 20; ++y)
                                 for (int x = 1; x < 20; ++x)
                                 {
                                     const glm::vec2 p(x / 20.0f, y / 20.0f);
                                     const auto hit = app.scene().pick(app.sceneExtent(), p);
                                     const int idx = !hit ? 2 : hit->id == first ? 0 : hit->id == second ? 1 : -1;
                                     if (idx >= 0 && !found[idx]) { points[idx] = p; found[idx] = true; }
                                 }
                             if (!found[0] || !found[1] || !found[2]) return;
                         }
                         auto& io = ImGui::GetIO();
                         if (stage < 6)
                         {
                             const auto p = points[stage / 2];
                             io.AddMousePosEvent(float(reg.origin.x + p.x * reg.size.x), float(reg.origin.y + p.y * reg.size.y));
                             io.AddMouseButtonEvent(0, stage % 2 == 0);
                             issued = true;
                         }
                         else if (stage < 8 && popupPoint.x > 0)
                         {
                             io.AddMousePosEvent(popupPoint.x, popupPoint.y);
                             io.AddMouseButtonEvent(0, stage == 6);
                             issued = true;
                         }
                     },
                     [&](auto& app) {
                         editor.ui(app);
                         if (stage >= 6)
                         {
                             if (!opened) { ImGui::OpenPopup("Selection blocker"); opened = true; }
                             if (ImGui::BeginPopupModal("Selection blocker", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
                             {
                                 ImGui::TextUnformatted("弹窗内点击不能选择场景");
                                 const auto pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
                                 popupPoint = {pos.x + size.x * 0.5f, pos.y + size.y * 0.5f};
                                 ImGui::EndPopup();
                             }
                         }
                     },
                     [&](auto& app) { editor.cancelInteraction(app); },
                     [&](auto& app, auto extent, const auto& input) {
                         editor.input(app, extent, input);
                         if (issued && ((stage % 2 == 0 && input.select.pressed) || (stage % 2 == 1 && input.select.released)))
                         {
                             if (stage == 1) EXPECT_EQ(editor.selected(), first);
                             if (stage == 3) EXPECT_EQ(editor.selected(), second);
                             if (stage == 5) { EXPECT_FALSE(editor.selected().valid()); editor.select(first); }
                             if (stage == 6 || stage == 7) { EXPECT_TRUE(input.captureMouse && editor.selected() == first); }
                             issued = false;
                             if (++stage == 8)
                             {
                                 // 在同一会话收尾验证输入竞争拦截、DPI 逻辑坐标一致性与加载失败保留/成功清空语义。
                                 const auto reg = app.viewportRegion();
                                 const auto pt = points[1];
                                 const glm::vec2 screenPos{reg.origin.x + pt.x * reg.size.x, reg.origin.y + pt.y * reg.size.y};
                                 const auto press = makeViewportInput(reg, screenPos, false, true, true, false);
                                 const auto held = makeViewportInput(reg, screenPos, false, false, true, false);
                                 const auto release = makeViewportInput(reg, screenPos, false, false, false, true);
                                 const auto preserved = [&] { EXPECT_EQ(editor.selected(), first); };
                                 editor.select(first);

                                 auto outsidePress = press;
                                 outsidePress.captureMouse = true;
                                 editor.input(app, extent, outsidePress);
                                 editor.input(app, extent, release);
                                 preserved();
                                 editor.input(app, extent, press);
                                 auto outsideHeld = held;
                                 outsideHeld.captureMouse = true;
                                 editor.input(app, extent, outsideHeld);
                                 editor.input(app, extent, release);
                                 preserved();

                                 editor.input(app, extent, press);
                                 auto dragged = held;
                                 dragged.selectDragDistanceSquared = 10000;
                                 editor.input(app, extent, dragged);
                                 editor.input(app, extent, release);
                                 preserved();

                                 for (int b = 0; b < 2; ++b)
                                 {
                                     editor.input(app, extent, press);
                                     auto cam = held;
                                     (b == 0 ? cam.rotate : cam.pan) = {true, true, false};
                                     editor.input(app, extent, cam);
                                     editor.input(app, extent, release);
                                     preserved();
                                     editor.cancelInteraction(app);
                                 }
                                 editor.input(app, extent, press);
                                 editor.cancelInteraction(app);
                                 editor.input(app, extent, release);
                                 preserved();

                                 for (float dpi : {1.0f, 1.5f, 2.0f})
                                 {
                                     editor.select(first);
                                     const Render::FrameExtent pixels{uint32_t(extent.width * dpi),
                                                                      uint32_t(extent.height * dpi)};
                                     editor.input(app, pixels, press);
                                     editor.input(app, pixels, release);
                                     EXPECT_EQ(editor.selected(), second);
                                 }
                                 editor.select(first);
                                 editor.input(app, extent, press);
                                 EXPECT_FALSE(editor.load(app, "missing-scene.json"));
                                 EXPECT_EQ(editor.selected(), first);
                                 editor.input(app, extent, release);
                                 EXPECT_EQ(editor.selected(), second);
                                 editor.input(app, extent, press);
                                 EXPECT_TRUE(editor.load(app, "assets/scenes/mixed.json"));
                                 EXPECT_FALSE(editor.selected().valid());
                                 editor.input(app, extent, release);
                                 EXPECT_FALSE(editor.selected().valid());
                                 app.requestExit();
                             }
                         }
                     },
                     [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(stage, 8);
        cleanValidation();
    }

    TEST_F(EditorInteractionTest, SelectionOverlayFollowsSameFrameEditsOrderAndSavesReviewCaptures)
    {
        copyShowcaseModels();
        Editor::EditorApp session(options(true, 1280, 800));
        auto editor = std::make_shared<Editor::EditorScene>();
        Render::SceneOutput previous;
        WindowCapture::Image unselectedScene, selectedScene;
        Render::Detail::RendererReviewAccess::ResourceCounts baseline{};
        Scene::EntityId target;
        int frame = 0;

        auto captureScene = [&](Editor::EditorApp& app) {
            WindowCapture::Image image;
            const auto extent = previous.extent();
            image.width = int(extent.width);
            image.height = int(extent.height);
            Render::Detail::RendererReviewAccess::withSceneOutput(
                app.renderer(), previous, [&](const auto& device, const auto& targetImage) {
                    image.rgba = TextureGpuReview::captureMip(device, targetImage, 0, {extent.width, extent.height});
                });
            return image;
        };

        session.run(
            {[&](auto& app) {
                 editor->initialize(app, "assets/scenes/shadows.json");
                 // 本用例专测 ImGui 二维选中叠加层不改写离屏场景图，显式关闭 3D 选中包围盒线框。
                 editor->debugOptions().showSelectionBounds = false;
                 app.shadowSettings().enabled = true;
                 ASSERT_GE(app.scene().entities().size(), 2u);
                 target = app.scene().entities()[0].id;
                 baseline = Render::Detail::RendererReviewAccess::resourceCounts(app.renderer());
             },
             [&](auto& app, const auto&) {
                 ++frame;
                 if (frame == 4) unselectedScene = captureScene(app);
                 else if (frame == 5) { selectedScene = captureScene(app); EXPECT_EQ(selectedScene.rgba, unselectedScene.rgba); }
                 else if (frame >= 11) app.requestExit();
             },
             [&](auto& app) {
                 auto* sceneList = app.sceneDrawList();
                 const int beforeVtx = sceneList ? sceneList->VtxBuffer.Size : 0;
                 if (frame == 4) editor->select(target);
                 else if (frame == 5)
                 {
                     auto pose = app.scene().find(target)->transform;
                     pose.setPosition(pose.position() + glm::vec3(0.45f, 0.25f, 0.15f));
                     pose.setRotation(glm::angleAxis(glm::radians(35.0f), glm::vec3(0, 1, 0)) * pose.rotation());
                     pose.setScale(pose.scale() * glm::vec3(1.25f, 0.9f, 1.15f));
                     ASSERT_TRUE(app.scene().setTransform(target, pose));
                 }
                 else if (frame == 6) { auto cam = app.scene().camera(); cam.setPosition(cam.position() + glm::vec3(0.8f, 0.4f, -0.8f)); app.scene().setCamera(cam); editor->beginOpen(); }
                 else if (frame == 7) { editor->cancelFileAction(); ASSERT_TRUE(app.scene().setVisible(target, false)); }
                 else if (frame == 8) { ASSERT_TRUE(app.scene().setVisible(target, true)); glfwSetWindowSize(static_cast<GLFWwindow*>(app.window().getNativeHandle()), 1024, 640); }
                 else if (frame == 10)
                 {
                     editor->deleteSelected(app);
                     ASSERT_TRUE(editor->save(app, "overlay-saved.json"));
                     std::ifstream file("overlay-saved.json");
                     const std::string json((std::istreambuf_iterator<char>(file)), {});
                     EXPECT_TRUE(json.find("selected") == std::string::npos && json.find("overlay") == std::string::npos);
                 }
                 editor->ui(app);
                 const int afterVtx = sceneList ? sceneList->VtxBuffer.Size : 0;
                 EXPECT_EQ(ImGui::GetForegroundDrawList()->VtxBuffer.Size, 0);
                 if (frame == 3 || frame == 7 || frame == 10)
                 {
                     EXPECT_EQ(afterVtx, beforeVtx);
                     if (frame == 7) EXPECT_EQ(editor->selected(), target);
                     if (frame == 10) EXPECT_FALSE(editor->selected().valid());
                 }
                 else if (frame == 4 || frame == 5 || frame == 6)
                 {
                     ASSERT_NE(sceneList, nullptr);
                     EXPECT_GT(afterVtx, beforeVtx);
                     EXPECT_FALSE(Editor::projectSelectionBox(app.scene(), editor->selected(), app.sceneExtent(), app.viewportRegion()).empty());
                 }
                 const auto counts = Render::Detail::RendererReviewAccess::resourceCounts(app.renderer());
                 EXPECT_TRUE(counts.meshes == baseline.meshes && counts.textures == baseline.textures && counts.materials == baseline.materials);
                 previous = app.renderer().prepareSceneTarget(app.sceneExtent());
                 glfwWaitEventsTimeout(0.04);
             },
             [editor](auto& app) { editor->cancelInteraction(app); },
             [editor](auto& app, auto extent, const auto& input) { editor->input(app, extent, input); },
             [editor](auto& app) { editor->layout(app); }});
        cleanValidation();
    }

    TEST_F(EditorInteractionTest, MultiInstanceClickRenameDuplicateDeleteSaveAndRestore)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(true, 1280, 800), std::make_unique<TrackedFileIO>(state));
        Editor::EditorScene editor;
        std::array<glm::vec2, 2> points{};
        std::array<bool, 2> found{};
        Scene::EntityId first, second, copy, uiCopy;
        std::shared_ptr<const Model::PreparedModel> sharedModel;
        size_t opensBeforeDuplicate = 0, uploadsBeforeDuplicate = 0;
        int stage = 0, frames = 0;
        bool issued = false;

        session.run(
            {[&](auto& app) {
                 // 验证旧 JSON v1（无 name 字段）可兼容加载且不保留选择。
                 editor.initialize(app, "assets/scenes/mixed.json");
                 EXPECT_FALSE(editor.selected().valid());
                 EditorTest::setSharedEditorScene(app, editor, false);
                 first = app.scene().entities()[0].id;
                 second = app.scene().entities()[1].id;
                 sharedModel = app.scene().find(first)->model;
                 EXPECT_EQ(app.scene().find(second)->model, sharedModel);
                 editor.select({});
                 enableGlfwCallbacks(static_cast<GLFWwindow*>(app.window().getNativeHandle()));
             },
             [&](auto& app, const auto&) {
                 if (++frames > 150)
                 {
                     ADD_FAILURE() << "多实例交互流程超时";
                     app.requestExit();
                     return;
                 }
                 const auto& region = app.viewportRegion();
                 if (frames < 5 || issued || region.size.x <= 0 || region.size.y <= 0)
                     return;
                 if (stage < 2 && (!found[0] || !found[1]))
                 {
                     for (int y = 1; y < 20; ++y)
                         for (int x = 1; x < 20; ++x)
                         {
                             const glm::vec2 p(x / 20.0f, y / 20.0f);
                             const auto hit = app.scene().pick(app.sceneExtent(), p);
                             const int idx = hit && hit->id == first ? 0 : hit && hit->id == second ? 1 : -1;
                             if (idx >= 0 && !found[idx])
                             {
                                 points[idx] = p;
                                 found[idx] = true;
                             }
                         }
                     if (!found[0] || !found[1])
                         return;
                 }
                 if (stage == 0 || stage == 1 || stage == 4 || stage == 5)
                 {
                     const auto p = points[stage < 2 ? 0 : 1];
                     auto& io = ImGui::GetIO();
                     io.AddMousePosEvent(float(region.origin.x + p.x * region.size.x),
                                         float(region.origin.y + p.y * region.size.y));
                     io.AddMouseButtonEvent(0, stage == 0 || stage == 4);
                     issued = true;
                 }
             },
             [&](auto& app) {
                 if (stage == 2)
                     activateWindowButton("Properties", "Duplicate");
                 editor.ui(app);
                 if (stage == 2)
                 {
                     // 独家边界 #2：通过 Properties Duplicate 按钮与 duplicateSelected 共享底层 PreparedModel 且 0 GPU 上传。
                     uiCopy = editor.selected();
                     ASSERT_NE(uiCopy, first);
                     EXPECT_EQ(app.scene().find(uiCopy)->model, sharedModel);
                     stage = 3;
                 }
                 else if (stage == 3)
                 {
                     editor.select(first);
                     ASSERT_TRUE(app.scene().setName(first, "主实例 ##"));
                     ASSERT_TRUE(app.scene().setVisible(first, false));
                     opensBeforeDuplicate = state->opens;
                     uploadsBeforeDuplicate = count("Uploaded mesh:");
                     copy = editor.duplicateSelected(app);
                     ASSERT_TRUE(copy.valid() && copy != first);
                     EXPECT_EQ(editor.selected(), copy);
                     EXPECT_EQ(app.scene().find(copy)->name, "主实例 ## 副本");
                     EXPECT_EQ(app.scene().find(copy)->model, sharedModel);
                     EXPECT_FALSE(app.scene().find(copy)->visible);
                     EXPECT_EQ(state->opens, opensBeforeDuplicate);
                     EXPECT_EQ(count("Uploaded mesh:"), uploadsBeforeDuplicate);
                     ASSERT_TRUE(app.scene().setTransform(
                         copy, Scene::Transform({0.0f, 1.8f, 0.0f}, {1, 0, 0, 0}, {-1, 0, 2})));
                     stage = 4;
                 }
                 else if (stage == 6)
                 {
                     ASSERT_EQ(editor.selected(), second);
                     editor.deleteSelected(app);
                     EXPECT_TRUE(!editor.selected().valid() && app.scene().find(second) == nullptr);
                     ASSERT_TRUE(editor.save(app, "multi-instance-saved.json"));
                     ASSERT_TRUE(editor.load(app, "multi-instance-saved.json"));
                     EXPECT_FALSE(editor.selected().valid());
                     ASSERT_EQ(app.scene().entities().size(), 3u);
                     const auto& es = app.scene().entities();
                     EXPECT_TRUE(es[1].model == sharedModel && es[2].model == sharedModel &&
                                 count("Uploaded mesh:") == uploadsBeforeDuplicate);
                     ASSERT_TRUE(editor.load(app, "assets/scenes/mixed.json"));
                     EXPECT_TRUE(!editor.selected().valid() && app.scene().entities().size() == 2u);
                     app.requestExit();
                 }
             },
             [&](auto& app) { editor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& input) {
                 editor.input(app, extent, input);
                 if (issued && (((stage == 0 || stage == 4) && input.select.pressed) ||
                                ((stage == 1 || stage == 5) && input.select.released)))
                 {
                     if (stage == 1) EXPECT_EQ(editor.selected(), first);
                     if (stage == 5) EXPECT_EQ(editor.selected(), second);
                     ++stage;
                     issued = false;
                 }
             },
             [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(stage, 6);
        cleanValidation();
    }

    TEST_F(EditorInteractionTest, UiInputAdapterAndOrbitCameraBatches)
    {
        auto native = Window::createPlatformWindow({"Input integration", 640, 480, true, false});
        auto* handle = static_cast<GLFWwindow*>(native->getNativeHandle());
        enableGlfwCallbacks(handle, false);
        {
            Render::Integration::ImGuiRenderer ui(*native);
            const auto sampled = [&] {
                return std::tuple{
                    glfwSetMouseButtonCallback(handle, nullptr), glfwSetCursorPosCallback(handle, nullptr),
                    glfwSetKeyCallback(handle, nullptr), glfwSetScrollCallback(handle, nullptr),
                    glfwSetCharCallback(handle, nullptr), glfwSetWindowFocusCallback(handle, nullptr)};
            }();
            const auto [mouse, position, key, scroll, character, uiFocus] = sampled;
            glfwSetMouseButtonCallback(handle, mouse);
            glfwSetCursorPosCallback(handle, position);
            glfwSetKeyCallback(handle, key);
            glfwSetScrollCallback(handle, scroll);
            glfwSetCharCallback(handle, character);
            glfwSetWindowFocusCallback(handle, uiFocus);

            auto& io = ImGui::GetIO();
            io.DisplaySize = {640, 480};
            io.DeltaTime = 1.0f / 60;
            unsigned char* pixels; int w, h;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
            App::UiInputAdapter adapter;
            App::OrbitCamera orbit;
            Scene::Camera camera({0, 0, 6});
            int characters = 0, focusPresses = 0;
            Input::PointerValue wheel;
            bool focusText = false;
            char text[32]{};
            const auto sample = [&](bool advanceFrame = false) {
                if (advanceFrame) native->beginInputFrame();
                ImGui::NewFrame();
                characters += io.InputQueueCharacters.Size;
                const auto result = adapter.sample(native->input());
                if (advanceFrame) orbit.apply(camera, result, {640, 480});
                focusPresses += result.focusPressed;
                wheel.x += result.scroll.x;
                wheel.y += result.scroll.y;
                ImGui::SetNextWindowPos({0, 0});
                ImGui::SetNextWindowSize({200, 200});
                ImGui::Begin("Panel");
                if (focusText) ImGui::SetKeyboardFocusHere();
                ImGui::InputText("Text", text, sizeof(text));
                ImGui::End();
                ImGui::EndFrame();
                return result;
            };

            position(handle, 100, 100);
            sample(); sample();
            mouse(handle, 0, GLFW_PRESS, 0);
            sample();
            native->beginInputFrame();
            mouse(handle, 0, GLFW_RELEASE, 0);
            position(handle, 300, 300);
            mouse(handle, 1, GLFW_PRESS, 0);
            EXPECT_TRUE(native->input().mouseButton(Input::MouseButton::Right).pressed && !sample().rotate.pressed);
            native->beginInputFrame();
            const auto delayed = sample();
            EXPECT_TRUE(delayed.rotate.pressed && !delayed.captureMouse);
            EXPECT_FALSE(orbit.apply(camera, delayed, {640, 480}));
            position(handle, 320, 300);
            EXPECT_TRUE(orbit.apply(camera, sample(), {640, 480}));
            const auto dragged = camera.position();
            mouse(handle, 1, GLFW_RELEASE, 0);
            key(handle, GLFW_KEY_F, 0, GLFW_PRESS, 0);
            scroll(handle, 1, 2);
            character(handle, 'x');
            for (int i = 0; i < 4; ++i) sample();
            EXPECT_TRUE(characters == 1 && focusPresses == 1 && wheel.x == 1 && wheel.y == 2);
            key(handle, GLFW_KEY_F, 0, GLFW_RELEASE, 0);
            adapter.cancel(); orbit.cancel();
            uiFocus(handle, GLFW_FALSE); uiFocus(handle, GLFW_TRUE);
            EXPECT_TRUE(native->input().focusLost());
            for (int i = 0; i < 5; ++i) EXPECT_FALSE(orbit.apply(camera, sample(), {640, 480}));
            EXPECT_EQ(camera.position(), dragged);

            // 验证三批排队（按下/释放/位移）不按帧数猜按住、平移手势及失焦排队边沿恢复。
            camera = Scene::Camera({0, 0, 6});
            position(handle, 300, 300);
            sample(true);
            mouse(handle, 2, GLFW_PRESS, 0);
            mouse(handle, 2, GLFW_RELEASE, 0);
            position(handle, 320, 300);
            const auto p1 = sample(true), p2 = sample(true), p3 = sample(true);
            EXPECT_TRUE(p1.pan.pressed && !p1.pan.released && p2.pan.released && p3.delta.x == 20);
            EXPECT_EQ(camera.position(), glm::vec3(0, 0, 6));
            mouse(handle, 2, GLFW_PRESS, 0);
            ASSERT_TRUE(sample(true).pan.pressed);
            position(handle, 340, 300);
            EXPECT_TRUE(sample(true).delta.x == 20 && camera.position() != glm::vec3(0, 0, 6));
            mouse(handle, 2, GLFW_RELEASE, 0);
            ASSERT_TRUE(sample(true).pan.released);

            const auto canceled = camera;
            adapter.cancel(); orbit.cancel();
            uiFocus(handle, GLFW_FALSE); uiFocus(handle, GLFW_TRUE);
            for (int i = 0; i < 2; ++i) { mouse(handle, 1, GLFW_PRESS, 0); mouse(handle, 1, GLFW_RELEASE, 0); }
            for (int i = 0; i < 4; ++i)
            {
                EXPECT_FALSE(sample(true).rotate.pressed);
                EXPECT_TRUE(camera.position() == canceled.position() && camera.rotation() == canceled.rotation());
            }
            EXPECT_FALSE(sample(true).rotate.down);
            mouse(handle, 1, GLFW_PRESS, 0);
            ASSERT_TRUE(sample(true).rotate.pressed);
            position(handle, 360, 310);
            EXPECT_TRUE(sample(true).delta.x == 20 && camera.rotation() != canceled.rotation());
            mouse(handle, 1, GLFW_RELEASE, 0);
            ASSERT_TRUE(sample(true).rotate.released);

            position(handle, 100, 100);
            sample(); sample();
            scroll(handle, 0, 1);
            EXPECT_TRUE(sample().captureMouse);
            focusText = true; sample();
            focusText = false; sample();
            key(handle, GLFW_KEY_F, 0, GLFW_PRESS, 0);
            const auto typing = sample();
            EXPECT_TRUE(typing.captureKeyboard && typing.focusPressed);
        }
    }

    TEST_F(EditorInteractionTest, SceneViewportLayoutOrbitDragPanAndFocus)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options());
            int frame = 0;
            bool open = true;
            Render::FrameExtent first{};
            session.run({[&](auto& app) {
                             const auto model = Model::prepareModel(app.renderer(), GLTF);
                             app.scene().createEntity(model);
                             app.scene().setCamera(Scene::Camera({0, 0, 3}));
                             app.shadowSettings().enabled = true;
                         },
                         [&](auto& app, const auto&) {
                             if (++frame >= 8) app.requestExit();
                         },
                         [&](auto& app) {
                             if (frame == 1) first = app.sceneExtent();
                             if (frame == 2) EXPECT_NE(app.sceneExtent(), first);
                             if (frame == 3 || frame == 4) EXPECT_EQ(app.sceneExtent(), Render::FrameExtent{});
                             if (frame >= 5) EXPECT_GT(app.sceneExtent().width, 0u);
                             app.scene().setTransform(app.scene().entities()[0].id,
                                                      Scene::Transform({float(frame) * 0.01f, 0, 0}));
                         },
                         {}, {},
                         [&](auto& app) {
                             open = frame != 3;
                             if (frame > 1) ImGui::SetWindowSize("Viewport", {float(400 + frame * 10), 300});
                             ImGui::SetWindowCollapsed("Viewport", frame == 4);
                             app.sceneViewport("Viewport", &open);
                         }});
            EXPECT_EQ(frame, 8);
            EXPECT_EQ(count("Created shadow target:"), 2u);
        }
        {
            Editor::EditorApp session(options());
            Editor::EditorScene demo;
            GLFWmousebuttonfun mouse = nullptr;
            GLFWcursorposfun cursor = nullptr;
            GLFWwindowfocusfun focus = nullptr;
            App::ViewportRegion region;
            Scene::Camera replaced, canceled;
            glm::quat moved{};
            int frame = -1, cancellations = 0, beforeFocusLoss = 0;
            session.run({[&](auto& app) {
                             demo.initialize(app);
                             EditorTest::setSharedEditorScene(app, demo, false);
                             const auto id = demo.selected();
                             const auto camPos = app.scene().camera().position();
                             App::UiInput in;
                             in.focusPressed = true;
                             in.captureKeyboard = true;
                             demo.input(app, {800, 600}, in);
                             EXPECT_EQ(app.scene().camera().position(), camPos);
                             demo.select({});
                             EXPECT_FALSE(demo.focusSelected(app, {800, 600}));
                             demo.select(id);
                             in.captureKeyboard = false;
                             demo.input(app, {800, 600}, in);
                             EXPECT_NE(app.scene().camera().position(), camPos);
                             demo.resetCamera(app);
                             EXPECT_EQ(app.scene().camera().position(), glm::vec3(0, 0, 6));
                             const auto hooks = hookWindowCallbacks(static_cast<GLFWwindow*>(app.window().getNativeHandle()));
                             mouse = hooks.mouse; cursor = hooks.cursor; focus = hooks.focus;
                         },
                         [&](auto& app, const auto&) {
                             auto* h = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                             switch (++frame)
                             {
                                 case 1: cursor(h, region.origin.x + 40, region.origin.y + 40); break;
                                 case 2: mouse(h, 1, GLFW_PRESS, 0); break;
                                 case 3: cursor(h, region.origin.x + 60, region.origin.y + 40); break;
                                 case 4:
                                     EXPECT_NE(app.scene().camera().rotation(), glm::quat(1, 0, 0, 0));
                                     moved = app.scene().camera().rotation();
                                     cursor(h, region.origin.x - 30, region.origin.y + 40);
                                     break;
                                 case 5: EXPECT_NE(app.scene().camera().rotation(), moved); break;
                                 case 6: {
                                     auto s = std::make_unique<Scene::Scene>();
                                     s->setCamera(Scene::Camera({0, 0, 8}));
                                     app.setScene(std::move(s));
                                     replaced = app.scene().camera();
                                     break;
                                 }
                                 case 7: cursor(h, region.origin.x + 80, region.origin.y + 50); break;
                                 case 8: EXPECT_EQ(app.scene().camera().rotation(), replaced.rotation()); mouse(h, 1, GLFW_RELEASE, 0); break;
                                 case 9: cursor(h, 500, 300); break;
                                 case 10: mouse(h, 2, GLFW_PRESS, 0); break;
                                 case 11: cursor(h, 520, 300); break;
                                 case 12: EXPECT_NE(app.scene().camera().position(), replaced.position()); mouse(h, 2, GLFW_RELEASE, 0); break;
                                 case 13: canceled = app.scene().camera(); beforeFocusLoss = cancellations; focus(h, GLFW_FALSE); break;
                                 case 14: focus(h, GLFW_TRUE); break;
                                 case 15:
                                     cursor(h, 520, 300);
                                     EXPECT_TRUE(app.scene().camera().position() == canceled.position() &&
                                                 app.scene().camera().rotation() == canceled.rotation());
                                     break;
                                 case 16: mouse(h, 1, GLFW_PRESS, 0); break;
                                 case 17: cursor(h, 540, 310); break;
                                 case 18:
                                     EXPECT_NE(app.scene().camera().rotation(), canceled.rotation());
                                     mouse(h, 1, GLFW_RELEASE, 0);
                                     app.requestExit();
                                     break;
                             }
                         },
                         {}, [&](auto& app) { ++cancellations; demo.cancelInteraction(app); },
                         [&](auto& app, auto extent, const auto& input) { demo.input(app, extent, input); },
                         [&](auto& app) { app.sceneViewport("Scene"); region = app.viewportRegion(); }});
            EXPECT_EQ(frame, 18);
            EXPECT_GE(beforeFocusLoss, 1);
            EXPECT_EQ(cancellations - beforeFocusLoss, 1);
        }
        cleanValidation();
    }

    TEST_F(EditorInteractionTest, ContinuousPropertyEditsColorPickersAndPendingUnsavedGuard)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(false, 1280, 800));
            Editor::EditorScene editor; WindowHooks hooks{}; Scene::EntityId targetId;
            std::string initialName; glm::vec3 initialPos{}; Render::Lighting initialLighting{}; std::array<float, 4> initialClearColor{};
            int phase = 0, totalFrames = 0; float scanY = 0.0f; double foundX = 0.0, foundY = 0.0;
            const auto itemId = [](ImGuiWindow* w, const char* idStr, const char* child) -> ImGuiID { return ImHashStr(child, 0, idStr ? w->GetID(idStr) : w->ID); };
            const auto zeroSubId = [](ImGuiWindow* w, const char* parent) -> ImGuiID { const int z = 0; return ImHashStr("", 0, ImHashData(&z, sizeof(z), w->GetID(parent))); };
            const auto scanItem = [&](GLFWwindow* h, ImGuiWindow* props, ImGuiID target, int nextPhase) {
                if (ImGui::GetHoveredID() == target) { foundX = ImGui::GetIO().MousePos.x; foundY = ImGui::GetIO().MousePos.y; scanY = float(foundY); phase = nextPhase; return; }
                const float minY = props->InnerRect.Min.y + 8.0f, maxY = props->InnerRect.Max.y - 8.0f;
                scanY = (scanY < minY) ? minY : scanY + 20.0f;
                if (scanY > maxY) {
                    scanY = minY;
                    const float pageStep = std::max(props->InnerRect.GetHeight() - 36.0f, 120.0f);
                    props->Scroll.y = props->Scroll.y >= props->ScrollMax.y ? 0.0f : std::min(props->Scroll.y + pageStep, props->ScrollMax.y);
                }
                const float availW = props->WorkRect.GetWidth();
                const float fontSz = ImGui::GetFontSize();
                const float labelW = std::clamp(availW * 0.34f, fontSz * 5.5f, fontSz * 8.0f);
                hooks.cursor(h, double(props->WorkRect.Min.x + labelW + 8.0f), double(scanY));
            };

            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/shadows.json");
                     targetId = app.scene().entities().front().id; editor.select(targetId);
                     initialName = app.scene().find(targetId)->name; initialPos = app.scene().find(targetId)->transform.position();
                     initialLighting = app.scene().lighting(); initialClearColor = app.clearColor();
                     auto* h = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                     hooks = hookWindowCallbacks(h); enableGlfwCallbacks(h); hooks.focus(h, GLFW_TRUE);
                 },
                 [&](auto& app, const auto&) {
                     if (phase == 99) return;
                     if (++totalFrames > 1500) { ADD_FAILURE() << "ContinuousProperty timeout phase=" << phase; app.requestExit(); return; }
                     auto* h = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                     auto* props = ImGui::FindWindowByName("Properties");
                     if (!props) return;
                     if (phase < 36) hooks.focus(h, GLFW_TRUE);
                     const std::string idStr = std::to_string(targetId.value());
                     switch (phase)
                     {
                         case 0: ImGui::ActivateItemByID(itemId(props, idStr.c_str(), "##entity-name")); ++phase; break;
                         case 1: ImGui::GetIO().AddInputCharactersUTF8("_AB"); ++phase; break;
                         case 2: case 5: ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); ++phase; break;
                         case 3: case 6: ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); ++phase; break;
                         case 4:
                             EXPECT_TRUE(editor.canUndo() && app.scene().find(targetId)->name != initialName);
                             ASSERT_TRUE(editor.undo(app));
                             EXPECT_EQ(app.scene().find(targetId)->name, initialName);
                             ImGui::ActivateItemByID(itemId(props, idStr.c_str(), "##entity-name")); ++phase; break;
                         case 7: EXPECT_FALSE(editor.canUndo()); scanItem(h, props, zeroSubId(props, "##position"), 8); break;
                         case 8: hooks.mouse(h, 0, GLFW_PRESS, 0); ++phase; break;
                         case 9: hooks.cursor(h, foundX + 30.0, foundY); ++phase; break;
                         case 10: hooks.cursor(h, foundX + 70.0, foundY); EXPECT_FALSE(editor.canUndo()); ++phase; break;
                         case 11: hooks.mouse(h, 0, GLFW_RELEASE, 0); ++phase; break;
                         case 12:
                             ASSERT_NE(app.scene().find(targetId)->transform.position(), initialPos);
                             ASSERT_TRUE(editor.undo(app));
                             EXPECT_TRUE(app.scene().find(targetId)->transform.position() == initialPos && !editor.canUndo());
                             ++phase; break;
                         case 13: scanItem(h, props, itemId(props, "##background", "##ColorButton"), 14); break;
                         case 14: case 24: hooks.mouse(h, 0, GLFW_PRESS, 0); ++phase; break;
                         case 15: case 25: hooks.mouse(h, 0, GLFW_RELEASE, 0); ++phase; break;
                         case 16: case 26:
                             if (auto* g = ImGui::GetCurrentContext(); !g->OpenPopupStack.empty() && !g->OpenPopupStack.back().Window->Hidden)
                             {
                                 const auto* win = g->OpenPopupStack.back().Window;
                                 foundX = win->InnerRect.Min.x + 50.0; foundY = win->InnerRect.Min.y + 50.0;
                                 hooks.cursor(h, foundX, foundY); ++phase;
                             }
                             break;
                         case 17: case 27: hooks.mouse(h, 0, GLFW_PRESS, 0); ++phase; break;
                         case 18: case 28: hooks.cursor(h, foundX + 60.0, foundY + 50.0); EXPECT_FALSE(editor.canUndo()); ++phase; break;
                         case 19: case 29: hooks.mouse(h, 0, GLFW_RELEASE, 0); ++phase; break;
                         case 20: case 30: ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); ++phase; break;
                         case 21: case 31: ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); ++phase; break;
                         case 22:
                             ASSERT_NE(app.clearColor(), initialClearColor); ASSERT_TRUE(editor.undo(app));
                             EXPECT_TRUE(app.clearColor() == initialClearColor && !editor.canUndo()); ++phase; break;
                         case 23: scanItem(h, props, itemId(props, "##lightColor", "##ColorButton"), 24); break;
                         case 32:
                             ASSERT_NE(app.scene().lighting().color, initialLighting.color); ASSERT_TRUE(editor.undo(app));
                             EXPECT_TRUE(app.scene().lighting().color == initialLighting.color && !editor.canUndo()); ++phase; break;
                         case 33: scanItem(h, props, props->GetID("##intensity"), 34); break;
                         case 34: hooks.mouse(h, 0, GLFW_PRESS, 0); ++phase; break;
                         case 35: hooks.cursor(h, double(props->WorkRect.Min.x + 120.0f), foundY); EXPECT_FALSE(editor.canUndo()); ++phase; break;
                         case 36: EXPECT_NE(app.scene().lighting().intensity, initialLighting.intensity); EXPECT_FALSE(editor.canUndo()); hooks.focus(h, GLFW_FALSE); ++phase; break;
                         case 37: EXPECT_FLOAT_EQ(app.scene().lighting().intensity, initialLighting.intensity); hooks.mouse(h, 0, GLFW_RELEASE, 0); hooks.focus(h, GLFW_TRUE); ++phase; break;
                         case 38:
                             EXPECT_FLOAT_EQ(app.scene().lighting().intensity, initialLighting.intensity);
                             EXPECT_FALSE(editor.canUndo()); phase = 99; app.requestExit(); break;
                     }
                 },
                 [&](auto& app) { editor.ui(app); }, [&](auto& app) { editor.cancelInteraction(app); },
                 [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
                 [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});
            EXPECT_EQ(phase, 99);
        }
        {
            Editor::EditorApp session(options(false, 1280, 800));
            Editor::EditorScene editor;
            Scene::EntityId targetId;
            std::string savedName;
            int step = 0, totalFrames = 0;
            bool done = false, cancelDialog = false;
            session.run(
                {[&](auto& app) {
                     editor.initialize(app, "assets/scenes/mixed.json");
                     targetId = app.scene().entities().front().id;
                     editor.select(targetId);
                     savedName = app.scene().find(targetId)->name;
                     enableGlfwCallbacks(static_cast<GLFWwindow*>(app.window().getNativeHandle()));
                 },
                 [&](auto& app, const auto&) {
                     if (++totalFrames > 160) { ADD_FAILURE() << "PendingProperty guard timeout step=" << step; app.requestExit(); return; }
                     auto* props = ImGui::FindWindowByName("Properties");
                     if (!props) return;
                     const ImGuiID nameId = ImHashStr("##entity-name", 0, props->GetID(std::to_string(targetId.value()).c_str()));
                     auto* h = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                     switch (step)
                     {
                         case 0: ImGui::SetWindowFocus("Properties"); ImGui::ActivateItemByID(nameId); ++step; break;
                         case 1:
                             if (!ImGui::GetIO().WantTextInput) { ImGui::SetWindowFocus("Properties"); ImGui::ActivateItemByID(nameId); break; }
                             ImGui::GetIO().AddInputCharacter('A'); ++step; break;
                         case 2:
                             EXPECT_NE(app.scene().find(targetId)->name, savedName);
                             PostMessageW(glfwGetWin32Window(h), WM_CLOSE, 0, 0);
                             ++step;
                             break;
                         case 3:
                             if (const auto* u = ImGui::FindWindowByName("Unsaved changes"); u && u->Active)
                             {
                                 EXPECT_TRUE(editor.isDirty() && !app.scene().entities().empty());
                                 cancelDialog = true;
                                 ++step;
                             }
                             break;
                         case 4:
                             if (const auto* u = ImGui::FindWindowByName("Unsaved changes"); !u || !u->Active)
                             {
                                 EXPECT_TRUE(editor.isDirty() && !app.window().shouldClose() && editor.canUndo());
                                 ASSERT_TRUE(editor.save(app, "pending-guard.json"));
                                 EXPECT_FALSE(editor.isDirty());
                                 done = true;
                                 app.requestExit();
                             }
                             break;
                     }
                 },
                 [&](auto& app) {
                     if (cancelDialog) { activateWindowButton("Unsaved changes", "Cancel"); cancelDialog = false; }
                     editor.ui(app);
                 },
                 [&](auto& app) { editor.cancelInteraction(app); },
                 [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
                 [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});
            EXPECT_TRUE(done);
        }
        cleanValidation();
    }

    TEST_F(EditorInteractionTest, EditMenuAndWindowShortcutsUndoRedoWorkWithoutViewportFocus)
    {
        copyShowcaseModels();
        Editor::EditorApp session(options(false, 1280, 800));
        Editor::EditorScene editor;
        WindowHooks hooks{};
        Scene::EntityId targetId;
        glm::vec3 initialPos{};
        int step = 0, totalFrames = 0;
        bool done = false;
        const auto injectCombo = [](ImGuiKey key, bool down, bool shift = false) {
            auto& io = ImGui::GetIO();
            io.AddKeyEvent(ImGuiMod_Ctrl, down);
            io.AddKeyEvent(ImGuiKey_LeftCtrl, down);
            if (shift) { io.AddKeyEvent(ImGuiMod_Shift, down); io.AddKeyEvent(ImGuiKey_LeftShift, down); }
            io.AddKeyEvent(key, down);
        };
        const auto clickPopup = [](const char* label) {
            auto* g = ImGui::GetCurrentContext();
            if (!g || g->OpenPopupStack.empty() || !g->OpenPopupStack.back().Window) return false;
            ImGui::ActivateItemByID(g->OpenPopupStack.back().Window->GetID(label));
            return true;
        };
        const auto menuId = [](ImGuiWindow* bar, const char* label) {
            return ImHashStr("", 0, ImHashStr(label, 0, bar->GetID("##MenuBar")));
        };
        session.run(
            {[&](auto& app) {
                 editor.initialize(app, "assets/scenes/shadows.json");
                 targetId = app.scene().entities().front().id;
                 editor.select(targetId);
                 initialPos = app.scene().find(targetId)->transform.position();
                 hooks = hookWindowCallbacks(static_cast<GLFWwindow*>(app.window().getNativeHandle()));
             },
             [&](auto& app, const auto&) {
                 if (done) return;
                 if (++totalFrames > 800) { done = true; ADD_FAILURE() << "Shortcut test timeout step=" << step; app.requestExit(); return; }
                 auto* h = static_cast<GLFWwindow*>(app.window().getNativeHandle());
                 auto* bar = ImGui::FindWindowByName("##MainMenuBar");
                 auto* props = ImGui::FindWindowByName("Properties");
                 if (!bar || !props) return;
                 const ImGuiID nameId = ImHashStr("##entity-name", 0, props->GetID(std::to_string(targetId.value()).c_str()));
                 switch (step)
                 {
                     case 0: ImGui::ActivateItemByID(menuId(bar, "View")); ++step; break;
                     case 1: if (clickPopup("Scene viewport")) ++step; break;
                     case 2: if (app.sceneExtent().width == 0 && ImGui::GetCurrentContext()->OpenPopupStack.empty()) ++step; break;
                     case 3: ImGui::ActivateItemByID(nameId); ++step; break;
                     case 4: ImGui::GetIO().AddInputCharactersUTF8("X"); ++step; break;
                     case 5: ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); ++step; break;
                     case 6: ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); ++step; break;
                     case 7: ImGui::SetWindowFocus("Objects"); EXPECT_TRUE(editor.canUndo()); injectCombo(ImGuiKey_Z, true); ++step; break;
                     case 8: case 12: case 17: case 22: injectCombo(ImGuiKey_Z, false); ++step; break;
                     case 9: EXPECT_TRUE(!editor.canUndo() && editor.canRedo()); injectCombo(ImGuiKey_Z, true, true); ++step; break;
                     case 10: injectCombo(ImGuiKey_Z, false, true); ++step; break;
                     case 11: EXPECT_TRUE(editor.canUndo() && !editor.canRedo()); injectCombo(ImGuiKey_Z, true); ++step; break;
                     case 13: EXPECT_FALSE(editor.canUndo()); injectCombo(ImGuiKey_Y, true); ++step; break;
                     case 14: injectCombo(ImGuiKey_Y, false); ++step; break;
                     case 15: EXPECT_TRUE(editor.canUndo()); ImGui::ActivateItemByID(nameId); ++step; break;
                     case 16: injectCombo(ImGuiKey_Z, true); ++step; break;
                     case 18: ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true); ++step; break;
                     case 19: ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false); ++step; break;
                     case 20: EXPECT_TRUE(editor.canUndo()); editor.requestOpen(app); ++step; break;
                     case 21: injectCombo(ImGuiKey_Z, true); ++step; break;
                     case 23:
                         EXPECT_TRUE(editor.canUndo());
                         if (auto* u = ImGui::FindWindowByName("Unsaved changes")) ImGui::ActivateItemByID(u->GetID("Cancel"));
                         ++step;
                         break;
                     case 24: injectCombo(ImGuiKey_Z, true); hooks.focus(h, GLFW_FALSE); ++step; break;
                     case 25: hooks.focus(h, GLFW_TRUE); ++step; break;
                     case 26: EXPECT_TRUE(editor.canUndo()); injectCombo(ImGuiKey_Z, false); ++step; break;
                     case 27: case 29: case 31: case 33:
                         if (step == 29) EXPECT_TRUE(!editor.canUndo() && editor.canRedo());
                         if (step == 31) EXPECT_TRUE(editor.canUndo());
                         if (step == 33) EXPECT_TRUE(app.scene().entities().size() == 4u && app.scene().find(targetId)->transform.position() == initialPos);
                         ImGui::ActivateItemByID(menuId(bar, "Edit"));
                         ++step;
                         break;
                     case 28: if (clickPopup("Undo")) ++step; break;
                     case 30: if (clickPopup("Redo")) ++step; break;
                     case 32: if (clickPopup("Duplicate")) ++step; break;
                     case 34: if (clickPopup("Delete")) ++step; break;
                     case 35: EXPECT_EQ(app.scene().entities().size(), 3u); done = true; app.requestExit(); break;
                 }
             },
             [&](auto& app) { editor.ui(app); }, [&](auto& app) { editor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& input) { editor.input(app, extent, input); },
             [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});
        EXPECT_TRUE(done);
        cleanValidation();
    }

    TEST_F(EditorInteractionTest, GizmoDragGesturePriorityUndoRedoOutOfViewportAndCancel)
    {
        copyShowcaseModels();
        Editor::EditorApp session(options(false, 1280, 800));
        Editor::EditorScene editor;
        Scene::EntityId targetId, otherId;
        Scene::Transform initialTransform, postDragTransform;
        Scene::Camera initialCamera;
        glm::vec2 xTipScreen{}, xMidScreen{};
        WindowHooks hooks{};
        int step = 0, totalFrames = 0;
        bool done = false;

        session.run(
            {[&](auto& app) {
                 editor.initialize(app, "assets/scenes/mixed.json");
                 targetId = app.scene().entities()[0].id;
                 otherId = app.scene().entities()[1].id;
                 editor.select(targetId);
                 initialTransform = app.scene().find(targetId)->transform;
                 initialCamera = app.scene().camera();
                 hooks = hookWindowCallbacks(static_cast<GLFWwindow*>(app.window().getNativeHandle()));
             },
             [&](auto& app, const auto&) {
                 if (++totalFrames > 160) { ADD_FAILURE() << "GizmoDrag timeout step=" << step; app.requestExit(); }
             },
             [&](auto& app) {
                 editor.ui(app);
                 if (step == 6) ++step;
                 else if (step == 7)
                 {
                     EXPECT_FALSE(editor.gizmoDragging() || editor.canUndo() || editor.isDirty());
                     EXPECT_EQ(app.scene().find(targetId)->transform.position(), initialTransform.position());
                     hooks.focus(static_cast<GLFWwindow*>(app.window().getNativeHandle()), GLFW_TRUE);
                     ++step;
                 }
             },
             [&](auto& app) { editor.cancelInteraction(app); },
             [&](auto& app, auto extent, const auto& realInput) {
                 const auto& reg = app.viewportRegion();
                 if (extent.width == 0 || extent.height == 0 || reg.size.x <= 0.0 || reg.size.y <= 0.0)
                     { editor.input(app, extent, realInput); return; }
                 const auto send = [&](glm::vec2 p, bool cap, bool pr, bool dn, bool rl, bool foc = false) {
                     editor.input(app, extent, makeViewportInput(reg, p, cap, pr, dn, rl, foc));
                 };
                 switch (step)
                 {
                     case 0: {
                         const auto gizmo = Editor::projectGizmo(app.scene(), targetId, editor.gizmoMode(), extent, reg);
                         ASSERT_TRUE(gizmo.visible);
                         xMidScreen = (gizmo.segments[0].segment.start + gizmo.segments[0].segment.end) * 0.5f;
                         xTipScreen = gizmo.segments[0].segment.end;
                         send({reg.origin.x - 20.0f, xMidScreen.y}, true, true, true, false);
                         EXPECT_FALSE(editor.gizmoDragging());
                         ++step;
                         break;
                     }
                     case 1: send(xMidScreen, false, false, true, false); EXPECT_FALSE(editor.gizmoDragging()); send(xMidScreen, false, false, false, true); ++step; break;
                     case 2: {
                         const auto gizmo = Editor::projectGizmo(app.scene(), targetId, editor.gizmoMode(), extent, reg);
                         ASSERT_TRUE(gizmo.visible);
                         xTipScreen = gizmo.segments[0].segment.end;
                         send(xTipScreen, false, true, true, false); ASSERT_TRUE(editor.gizmoDragging()); editor.setGizmoMode(Editor::GizmoMode::Rotate); EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Translate); ++step; break;
                     }
                     case 3:
                         send({reg.origin.x + reg.size.x + 120.0f, xTipScreen.y}, true, false, true, false, true);
                         EXPECT_TRUE(editor.gizmoDragging() && app.scene().camera().position() == initialCamera.position());
                         EXPECT_GT(app.scene().find(targetId)->transform.position().x, initialTransform.position().x + 0.5f);
                         ++step;
                         break;
                     case 4:
                         send({reg.origin.x + reg.size.x + 120.0f, xTipScreen.y}, true, false, false, true);
                         EXPECT_TRUE(!editor.gizmoDragging() && editor.selected() == targetId);
                         postDragTransform = app.scene().find(targetId)->transform;
                         ASSERT_TRUE(editor.undo(app));
                         EXPECT_TRUE(!editor.canUndo() && !editor.isDirty() && app.scene().find(targetId)->transform.position() == initialTransform.position());
                         ASSERT_TRUE(editor.redo(app));
                         EXPECT_EQ(app.scene().find(targetId)->transform.position(), postDragTransform.position());
                         ASSERT_TRUE(editor.undo(app));
                         ++step;
                         break;
                     case 5: {
                         const auto g = Editor::projectGizmo(app.scene(), targetId, editor.gizmoMode(), extent, reg);
                         xMidScreen = (g.segments[0].segment.start + g.segments[0].segment.end) * 0.5f;
                         send(xMidScreen, false, true, true, false);
                         send(xMidScreen + glm::vec2(60.0f, 0), false, false, true, false);
                         EXPECT_GT(app.scene().find(targetId)->transform.position().x, initialTransform.position().x + 0.1f);
                         hooks.focus(static_cast<GLFWwindow*>(app.window().getNativeHandle()), GLFW_FALSE);
                         ++step;
                         break;
                     }
                     case 6: case 7: break;
                     case 8: {
                         const auto g = Editor::projectGizmo(app.scene(), targetId, editor.gizmoMode(), extent, reg);
                         xMidScreen = (g.segments[0].segment.start + g.segments[0].segment.end) * 0.5f;
                         send(xMidScreen, false, true, true, false);
                         send(xMidScreen + glm::vec2(60.0f, 0), false, false, true, false);
                         editor.select(app, otherId);
                         EXPECT_TRUE(!editor.gizmoDragging() && !editor.canUndo() && editor.selected() == otherId &&
                                     app.scene().find(targetId)->transform.position() == initialTransform.position());
                         ++step;
                         break;
                     }
                     case 9: {
                         const glm::vec2 blankPos(reg.origin.x + 4.0, reg.origin.y + 4.0);
                         send(blankPos, false, true, true, false);
                         EXPECT_FALSE(editor.gizmoDragging());
                         send(blankPos, false, false, false, true);
                         EXPECT_FALSE(editor.selected().valid());
                         done = true; app.requestExit();
                         break;
                     }
                 }
             },
             [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});
        EXPECT_TRUE(done);
        cleanValidation();
    }

    TEST_F(EditorInteractionTest, GizmoOverlayClipOrderColorsHighlightShortcutsAndVisualModes)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(false, 1280, 800));
            Editor::EditorScene editor;
            Scene::EntityId modeledId{}, emptyEntityId{};
            int frame = 0, step = 0, rotateVertexCount = 0, translateVertexCount = 0;
            bool done = false, triggerEmptyDragThisFrame = false;
            glm::vec2 xMidScreen{};
            ImGuiID pendingNavActivateId = 0, pendingItemActivateId = 0;
            float emptyInitialAvgX = 0.0f;

            constexpr ImU32 kBox = IM_COL32(255, 200, 64, 255), kX = IM_COL32(230, 70, 70, 255),
                            kY = IM_COL32(70, 210, 90, 255), kZ = IM_COL32(70, 140, 245, 255),
                            kHi = IM_COL32(255, 240, 100, 255);
            struct Summary { int box = 0, x = 0, y = 0, z = 0, hi = 0, maxBox = -1, minGizmo = -1, maxAxis = -1, minHi = -1; float avgGizmoX = 0; bool clipped = false; };
            const auto summarize = [&](const ImDrawList* dl, const App::ViewportRegion& reg) {
                Summary s{};
                if (!dl) return s;
                double sumX = 0; int cnt = 0;
                for (int i = 0; i < dl->VtxBuffer.Size; ++i)
                {
                    const auto& v = dl->VtxBuffer[i];
                    if (v.col == kBox) { ++s.box; s.maxBox = std::max(s.maxBox, i); }
                    else if (v.col == kX || v.col == kY || v.col == kZ || v.col == kHi)
                    {
                        if (v.col == kX) ++s.x; else if (v.col == kY) ++s.y; else if (v.col == kZ) ++s.z;
                        else { ++s.hi; s.minHi = s.minHi < 0 ? i : std::min(s.minHi, i); }
                        if (v.col != kHi) s.maxAxis = std::max(s.maxAxis, i);
                        s.minGizmo = s.minGizmo < 0 ? i : std::min(s.minGizmo, i);
                        sumX += v.pos.x; ++cnt;
                    }
                }
                if (cnt > 0) s.avgGizmoX = float(sumX / cnt);
                for (int c = 0; c < dl->CmdBuffer.Size; ++c)
                {
                    const auto& cr = dl->CmdBuffer[c].ClipRect;
                    if (std::abs(cr.x - float(reg.origin.x)) < 1 && std::abs(cr.y - float(reg.origin.y)) < 1 &&
                        std::abs(cr.z - float(reg.origin.x + reg.size.x)) < 1 && std::abs(cr.w - float(reg.origin.y + reg.size.y)) < 1)
                        { s.clipped = true; break; }
                }
                return s;
            };

            auto* h = static_cast<GLFWwindow*>(session.window().getNativeHandle());
            session.run(
                {[&](auto& app) {
                     editor.initialize(app);
                     EditorTest::setSharedEditorScene(app, editor, false);
                     modeledId = editor.selected();
                     emptyEntityId = app.scene().createEntity({}, Scene::Transform({0.5f, 0.25f, 0}), true, "Empty");
                     editor.select(app, modeledId);
                     enableGlfwCallbacks(h, false);
                 },
                 [&](auto& app, const auto&) {
                     if (++frame > 100) { ADD_FAILURE() << "GizmoOverlay timeout step=" << step; app.requestExit(); }
                     if (pendingItemActivateId) { ImGui::ActivateItemByID(pendingItemActivateId); pendingItemActivateId = 0; }
                     if (triggerEmptyDragThisFrame)
                     {
                         triggerEmptyDragThisFrame = false;
                         const auto ex = app.sceneExtent(); const auto& reg = app.viewportRegion();
                         const auto g = Editor::projectGizmo(app.scene(), emptyEntityId, Editor::GizmoMode::Translate, ex, reg);
                         const glm::vec2 mid = (g.segments[0].segment.start + g.segments[0].segment.end) * 0.5f;
                         editor.input(app, ex, makeViewportInput(reg, mid, false, true, true, false));
                         editor.input(app, ex, makeViewportInput(reg, mid + glm::vec2(80, 0), false, false, true, false));
                     }
                 },
                 [&](auto& app) {
                     if (pendingNavActivateId) { auto& ctx = *ImGui::GetCurrentContext(); ctx.NavActivateId = ctx.NavActivateDownId = pendingNavActivateId; pendingNavActivateId = 0; }
                     editor.ui(app);
                     if (done) return;
                     const auto ex = app.sceneExtent(); const auto& reg = app.viewportRegion();
                     if (frame < 3 || ex.width == 0 || ex.height == 0 || reg.size.x <= 0 || reg.size.y <= 0) return;
                     const auto s = summarize(app.sceneDrawList(), reg);
                     auto& io = ImGui::GetIO();
                     switch (step)
                     {
                         case 0: {
                             EXPECT_TRUE(s.box > 0 && s.x > 0 && s.y > 0 && s.z > 0 && s.hi == 0 && s.maxBox < s.minGizmo && s.clipped);
                             translateVertexCount = s.x + s.y + s.z;
                             const auto g = Editor::projectGizmo(app.scene(), modeledId, Editor::GizmoMode::Translate, ex, reg);
                             xMidScreen = (g.segments[0].segment.start + g.segments[0].segment.end) * 0.5f;
                             editor.input(app, ex, makeViewportInput(reg, xMidScreen, false, false, false, false));
                             EXPECT_EQ(editor.hoveredGizmoAxis(), Editor::GizmoAxis::X);
                             ++step; break;
                         }
                         case 1:
                             EXPECT_TRUE(s.x == 0 && s.hi > 0 && s.y > 0 && s.z > 0 && s.minHi > s.maxAxis);
                             editor.input(app, ex, makeViewportInput(reg, xMidScreen, false, true, true, false));
                             ASSERT_TRUE(editor.gizmoDragging() && editor.activeGizmoAxis() == Editor::GizmoAxis::X);
                             io.AddKeyEvent(ImGuiKey_E, true); ++step; break;
                         case 2:
                             EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Translate);
                             io.AddKeyEvent(ImGuiKey_E, false);
                             editor.input(app, ex, makeViewportInput(reg, xMidScreen + glm::vec2(70, 0), false, false, false, true));
                             editor.input(app, ex, makeViewportInput(reg, {reg.origin.x + 5, reg.origin.y + 5}, false, false, false, false));
                             EXPECT_FALSE(editor.gizmoDragging()); ++step; break;
                         case 3: EXPECT_TRUE(s.x > 0 && s.hi == 0); pendingNavActivateId = ImGui::FindWindowByName("Properties")->GetID("Rotate (E)"); ++step; break;
                         case 4:
                             EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Rotate);
                             rotateVertexCount = s.x + s.y + s.z;
                             EXPECT_GT(rotateVertexCount, translateVertexCount);
                             pendingNavActivateId = ImGui::FindWindowByName("Properties")->GetID("Scale (R)"); ++step; break;
                         case 5: EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale); io.AddKeyEvent(ImGuiKey_W, true); ++step; break;
                         case 6: EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Translate); io.AddKeyEvent(ImGuiKey_W, false); io.AddKeyEvent(ImGuiKey_R, true); ++step; break;
                         case 7: EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale); io.AddKeyEvent(ImGuiKey_R, false); io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_W, true); ++step; break;
                         case 8:
                             EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale);
                             io.AddKeyEvent(ImGuiKey_W, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
                             pendingItemActivateId = ImHashStr("##entity-name", 0, ImGui::FindWindowByName("Properties")->GetID(std::to_string(modeledId.value()).c_str()));
                             ++step; break;
                         case 9: io.AddKeyEvent(ImGuiKey_W, true); io.AddInputCharacter('w'); ++step; break;
                         case 10: EXPECT_TRUE(io.WantTextInput && editor.gizmoMode() == Editor::GizmoMode::Scale); io.AddKeyEvent(ImGuiKey_W, false); io.AddKeyEvent(ImGuiKey_Enter, true); ++step; break;
                         case 11:
                             io.AddKeyEvent(ImGuiKey_Enter, false); ImGui::ClearActiveID();
                             io.AddMousePosEvent(float(reg.origin.x + 24), float(reg.origin.y + 24));
                             io.AddMouseButtonEvent(ImGuiMouseButton_Right, true); ++step; break;
                         case 12: io.AddKeyEvent(ImGuiKey_W, true); ++step; break;
                         case 13: EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Scale); io.AddKeyEvent(ImGuiKey_W, false); io.AddMouseButtonEvent(ImGuiMouseButton_Right, false); ++step; break;
                         case 14:
                             if (!ImGui::GetCurrentContext()->InputEventsQueue.empty() || ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsKeyDown(ImGuiKey_W)) break;
                             io.AddKeyEvent(ImGuiKey_W, true); ++step; break;
                         case 15: EXPECT_EQ(editor.gizmoMode(), Editor::GizmoMode::Translate); io.AddKeyEvent(ImGuiKey_W, false); editor.select(app, emptyEntityId); ++step; break;
                         case 16: EXPECT_TRUE(s.box == 0 && s.x > 0 && s.y > 0 && s.z > 0); emptyInitialAvgX = s.avgGizmoX; triggerEmptyDragThisFrame = true; ++step; break;
                         case 17:
                             EXPECT_TRUE(app.scene().find(emptyEntityId)->transform.position().x > 0.6f && s.avgGizmoX > emptyInitialAvgX + 10.0f && s.x == 0 && s.hi > 0);
                             editor.input(app, ex, makeViewportInput(reg, xMidScreen, false, false, false, true));
                             ASSERT_TRUE(app.scene().setVisible(emptyEntityId, false)); ++step; break;
                         case 18: EXPECT_TRUE(s.x == 0 && s.y == 0 && s.z == 0); editor.select(app, {}); done = true; app.requestExit(); break;
                     }
                 },
                 {}, {}, [&](auto& app) { editor.layout(app); }, [&](auto& app) { editor.requestClose(app); }});
            EXPECT_TRUE(done);
        }
        cleanValidation();
    }

    TEST_F(EditorInteractionTest, HierarchyTreeReparentGizmoUndoSaveAndRestartUnderValidation)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options(false, 1280, 800));
            Editor::EditorScene editor;
            Scene::EntityId rootId{}, child1Id{}, child2Id{};
            glm::vec3 initialRootPos{};
            int frame = 0, step = 0;
            bool done = false;
            session.run(
                {[&](auto& app) {
                     // 1. 打开 v1 平铺场景 mixed.json，验证所有实体均为根实体。
                     editor.initialize(app, "assets/scenes/mixed.json");
                     ASSERT_EQ(app.scene().entities().size(), 2u);
                     for (const auto& e : app.scene().entities())
                         EXPECT_FALSE(e.parent.valid());

                     // 2. 组装 1 个父对象 + 2 个重名子物件，并挂接。
                     rootId = app.scene().entities()[0].id;
                     initialRootPos = app.scene().find(rootId)->transform.position();
                     child1Id = app.scene().entities()[1].id;
                     editor.selectModel(true);
                     child2Id = editor.createEntity(app);
                     app.scene().setName(rootId, "ParentGroup");
                     app.scene().setName(child1Id, "ChildItem");
                     app.scene().setName(child2Id, "ChildItem");
                     app.scene().setTransform(child1Id, Scene::Transform({0.5f, 0.0f, 0.0f}));
                     app.scene().setTransform(child2Id, Scene::Transform({-0.5f, 0.2f, 0.0f}));
                     ASSERT_TRUE(editor.controller().setParent(app, child1Id, rootId));
                     ASSERT_TRUE(editor.controller().setParent(app, child2Id, rootId));
                     editor.select(app, rootId);
                 },
                 [&](auto& app, const auto&) {
                     if (++frame > 120)
                     {
                         ADD_FAILURE() << "Hierarchy integration test timeout at step=" << step;
                         app.requestExit();
                         return;
                     }
                     auto* objectsWin = ImGui::FindWindowByName("Objects");
                     if (frame < 4 || !objectsWin || objectsWin->DC.ChildWindows.empty())
                         return;

                     switch (step)
                     {
                         case 0: {
                             // 验证对象树折叠按钮不改变当前选中。
                             auto* listChild = objectsWin->DC.ChildWindows[0];
                             const ImGuiID expandId =
                                 ImHashStr("##expand", 0, listChild->GetID(std::to_string(rootId.value()).c_str()));
                             ImGui::ActivateItemByID(expandId);
                             ++step;
                             break;
                         }
                         case 1:
                             EXPECT_EQ(editor.selected(), rootId);
                             // 驱动根对象旋转与平移，验证子物件世界姿态与拾取联动，以及撤销/重做和保存。
                             editor.setGizmoMode(Editor::GizmoMode::Rotate);
                             ASSERT_TRUE(editor.controller().operations().setTransform(
                                 app.scene(), app.clearColor(),
                                 Scene::Transform({1.5f, 0.0f, 0.0f},
                                                  glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 1, 0)),
                                                  {1.0f, 1.0f, 1.0f})));
                             {
                                 const auto w1 = app.scene().worldMatrix(child1Id);
                                 const auto w2 = app.scene().worldMatrix(child2Id);
                                 ASSERT_TRUE(w1 && w2);
                                 EXPECT_NEAR((*w1)[3][0], 1.5f, 1e-4f);
                                 EXPECT_NEAR((*w1)[3][2], -0.5f, 1e-4f);
                                 EXPECT_NEAR((*w2)[3][2], 0.5f, 1e-4f);
                                 const glm::vec3 child1Pos((*w1)[3]);
                                 const auto hit = app.scene().pick(
                                     Math::LineSegment{child1Pos + glm::vec3(0, 0, 2), child1Pos - glm::vec3(0, 0, 2)});
                                 ASSERT_TRUE(hit.has_value());
                                 EXPECT_EQ(hit->id, child1Id);
                             }
                             ASSERT_TRUE(editor.undo(app));
                             EXPECT_EQ(app.scene().find(rootId)->transform.position(), initialRootPos);
                             ASSERT_TRUE(editor.redo(app));
                             EXPECT_NEAR((*app.scene().worldMatrix(child1Id))[3][0], 1.5f, 1e-4f);

                             ASSERT_TRUE(editor.save(app, "hierarchy-saved.json"));
                             // 验证打开非法 v2 场景失败时保留原场景、当前选择与文件关联。
                             {
                                 std::string raw;
                                 {
                                     std::ifstream in("hierarchy-saved.json");
                                     raw.assign(std::istreambuf_iterator<char>(in), {});
                                 }
                                 const auto firstNull = raw.find("\"parent\": null");
                                 ASSERT_NE(firstNull, std::string::npos);
                                 raw.replace(firstNull, 14, "\"parent\": 1");
                                 std::ofstream out("invalid-hierarchy.json", std::ios::binary | std::ios::trunc);
                                 out << raw;
                             }
                             EXPECT_FALSE(editor.load(app, "invalid-hierarchy.json"));
                             EXPECT_EQ(editor.scenePath(), "hierarchy-saved.json");
                             EXPECT_EQ(editor.selected(), rootId);
                             EXPECT_EQ(app.scene().entities().size(), 3u);
                             EXPECT_EQ(app.scene().find(child1Id)->parent, rootId);
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
}  // namespace
