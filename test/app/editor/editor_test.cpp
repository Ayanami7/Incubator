#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <spdlog/sinks/ostream_sink.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <glm/ext/matrix_transform.hpp>
#include <iostream>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "app/common/lighting_controls.h"
#include "app/common/startup_config.h"
#include "app/common/ui_input.h"
#include "app/editor/editor_app.h"
#include "app/editor/editor_scene.h"
#include "app/editor/selection_overlay.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/prepared_model.h"
#include "runtime/render/detail/renderer_review.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/resource/loader/float_texture_resource.h"
#include "runtime/resource/loader/texture_resource.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/scene/scene_file.h"
#include "runtime/scene/scene_frame.h"
#include "support/texture_gpu_review.h"
#include "support/window_capture.h"
#include "test/app/editor/showcase_scene.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#ifdef ERROR
#undef ERROR
#endif

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;
    namespace fs = std::filesystem;
    namespace NativeIO = Incubator::FileIO;

    constexpr const char* GLTF = "models/minimal_gltf/model.gltf";
    constexpr const char* OBJ = "models/minimal_obj/model.obj";
    constexpr const char* PNG = "textures/direction_rgba.png";
    constexpr const char* DEMO_GLTF = "models/avocado/glTF/Avocado.gltf";
    constexpr const char* DEMO_OBJ = "models/utah_teapot/utah_teapot.obj";

    struct Lifetime
    {
        bool ioDestroyed = false, routesEmptyAtIoClose = false, gpuAliveAtIoClose = false, windowAliveAtIoClose = false,
             callbackReleased = true, callbackReleasedAtIoClose = false, modelAliveAtIoClose = false;
        size_t opens = 0;
        HWND window = nullptr;
        Render::Mesh mesh;
        std::weak_ptr<const Model::PreparedModel> model;
    };

    class TrackedFileIO final : public NativeIO::FileIO
    {
    public:
        explicit TrackedFileIO(std::shared_ptr<Lifetime> state)
            : state_(std::move(state)), native_(NativeIO::createPlatformFileIO()) {}
        ~TrackedFileIO() override
        {
            state_->ioDestroyed = true;
            state_->routesEmptyAtIoClose = ResourceManager::instance().loadersEmpty();
            state_->gpuAliveAtIoClose = state_->mesh.valid();
            state_->windowAliveAtIoClose = state_->window && IsWindow(state_->window);
            state_->callbackReleasedAtIoClose = state_->callbackReleased;
            state_->modelAliveAtIoClose = !state_->model.expired();
        }
        bool exists(const fs::path& p) override { return native_->exists(p); }
        uint64_t getFileSize(const fs::path& p) override { return native_->getFileSize(p); }
        bool remove(const fs::path& p) override { return native_->remove(p); }
        std::unique_ptr<NativeIO::FileHandle> open(const fs::path& p, NativeIO::OpenMode m, NativeIO::ShareMode s) override
        { ++state_->opens; return native_->open(p, m, s); }
        std::vector<NativeIO::DirectoryEntry> listDirectory(const fs::path& p) override { return native_->listDirectory(p); }
        bool directoryExists(const fs::path& p) override { return native_->directoryExists(p); }
        bool createDirectory(const fs::path& p) override { return native_->createDirectory(p); }
        bool removeDirectory(const fs::path& p) override { return native_->removeDirectory(p); }

    private:
        std::shared_ptr<Lifetime> state_;
        std::unique_ptr<NativeIO::FileIO> native_;
    };

    struct OriginalResource final : public ResourceObject
    {
        OriginalResource(ResourceId id, ResourceType type) : ResourceObject(id, type) {}
    };

    class OriginalLoader final : public LoaderBase
    {
    public:
        OriginalLoader(ResourceType type, std::shared_ptr<bool> destroyed)
            : type_(type), destroyed_(std::move(destroyed)) {}
        ~OriginalLoader() override { *destroyed_ = true; }
        std::unique_ptr<ResourceObject> load(const ResourceId& id, std::string_view) override
        { return std::make_unique<OriginalResource>(id, type_); }

    private:
        ResourceType type_;
        std::shared_ptr<bool> destroyed_;
    };

    struct CallbackHold
    {
        explicit CallbackHold(std::shared_ptr<Lifetime> state) : state(std::move(state))
        { this->state->callbackReleased = false; }
        ~CallbackHold()
        {
            cpu.reset();
            EXPECT_TRUE(ResourceManager::instance().hasLoader(ResourceType::MODEL));
            EXPECT_TRUE(state->mesh.valid());
            state->callbackReleased = true;
        }
        std::shared_ptr<Lifetime> state;
        CpuResource cpu;
    };

    struct WindowHooks
    {
        GLFWmousebuttonfun mouse = nullptr;
        GLFWcursorposfun cursor = nullptr;
        GLFWwindowfocusfun focus = nullptr;
    };

    WindowHooks hookWindowCallbacks(GLFWwindow* handle, bool setFocus = true)
    {
        WindowHooks hooks{glfwSetMouseButtonCallback(handle, nullptr), glfwSetCursorPosCallback(handle, nullptr),
                          glfwSetWindowFocusCallback(handle, nullptr)};
        glfwSetMouseButtonCallback(handle, hooks.mouse);
        glfwSetCursorPosCallback(handle, hooks.cursor);
        glfwSetWindowFocusCallback(handle, hooks.focus);
        if (setFocus && hooks.focus) hooks.focus(handle, GLFW_TRUE);
        return hooks;
    }

    void enableGlfwCallbacks(GLFWwindow* handle, bool cursorEnter = true)
    {
        if (cursorEnter)
            if (const auto enter = glfwSetCursorEnterCallback(handle, nullptr))
                { glfwSetCursorEnterCallback(handle, enter); enter(handle, GLFW_TRUE); }
        if (const auto focus = glfwSetWindowFocusCallback(handle, nullptr))
            { glfwSetWindowFocusCallback(handle, focus); focus(handle, GLFW_TRUE); }
        glfwFocusWindow(handle);
    }

    void activateWindowButton(const char* windowName, const char* buttonLabel)
    {
        auto* window = ImGui::FindWindowByName(windowName);
        ASSERT_NE(window, nullptr) << windowName;
        auto& ctx = *ImGui::GetCurrentContext();
        ctx.NavActivateId = ctx.NavActivateDownId = window->GetID(buttonLabel);
    }

    App::UiInput makeViewportInput(const App::ViewportRegion& region, glm::vec2 screenPos, bool captureMouse,
                                   bool pressed, bool down, bool released, bool focus = false)
    {
        App::UiInput ui{};
        ui.position = {screenPos.x - region.origin.x, screenPos.y - region.origin.y};
        ui.captureMouse = captureMouse;
        ui.select = {down, pressed, released};
        ui.focusPressed = focus;
        return ui;
    }

    class EditorAppTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            previous_ = fs::current_path();
            sandbox_ = previous_ / "review/application" / ::testing::UnitTest::GetInstance()->current_test_info()->name();
            fs::remove_all(sandbox_);
            fs::create_directories(sandbox_ / "assets/models");
            const auto fixtureRoot = Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR);
            for (const char* dir : {"shaders", "fonts", "textures"})
                fs::copy(fixtureRoot / dir, sandbox_ / "assets" / dir, fs::copy_options::recursive);
            fs::copy(fixtureRoot / "models/minimal_gltf", sandbox_ / "assets/models/minimal_gltf",
                     fs::copy_options::recursive);
            fs::current_path(sandbox_);
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(renderLog_);
            sink->set_pattern("%l|%v");
            for (const char* name : {"render", "render.vulkan"})
            {
                Log::Registry::get(name).addSink(sink);
                Log::Registry::get(name).setLevel(Log::Level::DEBUG);
            }
        }
        void TearDown() override
        {
            ResourceManager::instance().reset();
            ResourceRegistry::instance().clear();
            Log::Registry::reset();
            fs::current_path(previous_);
        }
        void copyMinimalObj()
        {
            fs::copy(Utils::Utf8::toFsPath(RUNTIME_TEST_ASSET_DIR) / "models/minimal_obj",
                     sandbox_ / "assets/models/minimal_obj",
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
        void copyShowcaseModels()
        {
            copyMinimalObj();
            for (const char* dir : {"avocado", "utah_teapot", "ground"})
                fs::copy(previous_ / "assets/models" / dir, sandbox_ / "assets/models" / dir,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            fs::copy(previous_ / "assets/ibl", sandbox_ / "assets/ibl",
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            fs::create_directories(sandbox_ / "assets/scenes");
            for (const char* file : {"mixed.json", "shadows.json", "pbr-showcase.json"})
                fs::copy_file(previous_ / "assets/scenes" / file, sandbox_ / "assets/scenes" / file,
                              fs::copy_options::overwrite_existing);
        }
        App::SessionOptions options(bool visible = false, int width = 640, int height = 480)
        {
            App::SessionOptions r;
            r.window.title = "Incubator application session";
            r.window.width = width;
            r.window.height = height;
            r.window.visible = visible;
            r.validation = Render::RendererValidationMode::Required;
            r.clearColor = {0.2f, 0.24f, 0.28f, 1};
            return r;
        }
        void track(Editor::EditorApp& session, const std::shared_ptr<const Model::PreparedModel>& model,
                   const std::shared_ptr<Lifetime>& state)
        {
            state->window = glfwGetWin32Window(static_cast<GLFWwindow*>(session.window().getNativeHandle()));
            state->model = model;
            state->mesh = model->meshes()[0];
        }
        void closed(const std::shared_ptr<Lifetime>& state)
        {
            EXPECT_TRUE(state->ioDestroyed && state->routesEmptyAtIoClose && !state->gpuAliveAtIoClose &&
                        !state->windowAliveAtIoClose && state->callbackReleasedAtIoClose && !state->modelAliveAtIoClose);
            EXPECT_TRUE(ResourceManager::instance().loadersEmpty());
        }
        void unloaded(ResourceType type, const char* path)
        {
            auto& reg = ResourceRegistry::instance();
            const auto id = reg.lookup(type, path);
            ASSERT_TRUE(id.valid());
            EXPECT_EQ(reg.refCount(id), 0u);
            EXPECT_EQ(reg.getObject(id), nullptr);
        }
        void routes()
        {
            const auto& m = ResourceManager::instance();
            EXPECT_TRUE(m.hasLoader(ResourceType::SHADER) && m.hasLoader(ResourceType::TEXTURE) && m.hasLoader(ResourceType::MODEL));
        }
        void cleanValidation()
        {
            const auto text = renderLog_.str();
            EXPECT_NE(text.find("validation: enabled"), std::string::npos);
            EXPECT_TRUE(text.find("warning|") == std::string::npos && text.find("error|") == std::string::npos) << text;
        }
        size_t count(std::string_view value)
        {
            const auto text = renderLog_.str();
            size_t n = 0;
            for (auto pos = text.find(value); pos != std::string::npos; pos = text.find(value, pos + value.size()))
                ++n;
            return n;
        }
        fs::path previous_, sandbox_;
        std::ostringstream renderLog_;
    };

    TEST_F(EditorAppTest, ReloadUpdatesSharedEntitiesUndoRedoAndRejectsIncompatibleOverrides)
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

    TEST_F(EditorAppTest, NewVirtualModelsShareAndFailuresPreserveEditingStateUntilRetry)
    {
        copyShowcaseModels();
        fs::copy("assets/models/minimal_gltf", "assets/models/imported_gltf", fs::copy_options::recursive);
        fs::copy("assets/models/minimal_obj", "assets/models/imported_obj", fs::copy_options::recursive);
        Editor::EditorApp session(options());
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
                 app.requestExit();
             },
             {}, {}});
        cleanValidation();
    }

    TEST_F(EditorAppTest, NameFieldEditsSelectedEntityWithoutPreparingAnotherModel)
    {
        copyShowcaseModels();
        auto state = std::make_shared<Lifetime>();
        Editor::EditorApp session(options(true, 1280, 800), std::make_unique<TrackedFileIO>(state));
        auto editor = std::make_shared<Editor::EditorScene>();
        Scene::EntityId selected, first;
        std::shared_ptr<const Model::PreparedModel> model;
        size_t opens = 0;
        int frame = 0;
        ImVec2 click{}, firstRow{}, thirdRow{};
        session.run({[&](auto& app) {
                         editor->initialize(app);
                         first = app.scene().entities()[0].id;
                         selected = editor->createEntity(app);
                         ASSERT_TRUE(selected.valid());
                         EXPECT_EQ(app.scene().find(selected)->name, "Avocado");
                         model = app.scene().find(selected)->model;
                         opens = state->opens;
                         enableGlfwCallbacks(static_cast<GLFWwindow*>(app.window().getNativeHandle()));
                     },
                     [&](auto& app, const auto&) {
                         auto& io = ImGui::GetIO();
                         if (frame == 3) { io.AddMousePosEvent(click.x, click.y); io.AddMouseButtonEvent(0, true); }
                         if (frame == 4 || frame == 11 || frame == 14) io.AddMouseButtonEvent(0, false);
                         if (frame == 6) io.AddInputCharactersUTF8(" 兔子##");
                         if (frame == 8) { ASSERT_NE(app.scene().find(selected)->name.find("兔子##"), std::string::npos); app.scene().setName(first, app.scene().find(selected)->name); ImGui::SetWindowFocus("Objects"); }
                         if (frame == 10 || frame == 13) { const auto pos = frame == 10 ? firstRow : thirdRow; io.AddMousePosEvent(pos.x, pos.y); io.AddMouseButtonEvent(0, true); }
                         if (frame == 12) EXPECT_EQ(editor->selected(), first);
                         if (frame == 17)
                         {
                             EXPECT_TRUE(app.scene().find(selected)->name.find("兔子##") != std::string::npos &&
                                         editor->selected() == selected && app.scene().find(selected)->model == model && state->opens == opens);
                             app.requestExit();
                         }
                     },
                     [&](auto& app) {
                         editor->ui(app);
                         const auto* panel = ImGui::FindWindowByName("Properties");
                         ASSERT_NE(panel, nullptr);
                         click = {panel->Pos.x + panel->Size.x - ImGui::GetFontSize() * 2,
                                  panel->DC.CursorStartPos.y + 2 * ImGui::GetFrameHeightWithSpacing() + ImGui::GetFrameHeight() * 0.5f};
                         if (frame >= 9)
                         {
                             const auto* objects = ImGui::FindWindowByName("Objects");
                             ASSERT_TRUE(objects && objects->DC.ChildWindows.Size == 1);
                             const auto* child = objects->DC.ChildWindows[0];
                             const float entityX = child->DC.CursorStartPos.x + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + 20.0f;
                             firstRow = {entityX, child->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f};
                             thirdRow = {entityX, firstRow.y + 2 * ImGui::GetFrameHeightWithSpacing()};
                         }
                         ++frame;
                     },
                     {}, {}, [editor](auto& app) { editor->layout(app); }});
        model.reset();
        cleanValidation();
    }

    TEST_F(EditorAppTest, ViewportClickSelectsSharedInstancesAndPopupBlocksSelection)
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
                             if (++stage == 8) app.requestExit();
                         }
                     },
                     [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(stage, 8);
        cleanValidation();
    }

    TEST_F(EditorAppTest, ClickCandidatesCancelAndUseLogicalCoordinatesAcrossExtents)
    {
        copyShowcaseModels();
        Editor::EditorApp session(options(false, 1280, 800));
        Editor::EditorScene editor;
        int frames = 0, updates = 0;
        session.run({[&](auto& app) {
                         editor.initialize(app);
                         EditorTest::setSharedEditorScene(app, editor, false);
                         enableGlfwCallbacks(static_cast<GLFWwindow*>(app.window().getNativeHandle()), false);
                     },
                     [&](auto& app, const auto&) {
                         if (++updates > 150)
                         {
                             ADD_FAILURE() << "未收到有效视口输入";
                             app.requestExit();
                         }
                     },
                     [&](auto& app) { editor.ui(app); }, {},
                     [&](auto& app, auto extent, const auto&) {
                         if (++frames != 3)
                             return;
                         const auto first = app.scene().entities()[0].id, second = app.scene().entities()[1].id;
                         const auto region = app.viewportRegion();
                         glm::vec2 point{};
                         bool found = false;
                         for (int y = 1; y < 30 && !found; ++y)
                             for (int x = 1; x < 30 && !found; ++x)
                                 if (const auto hit = app.scene().pick(extent, {x / 30.0f, y / 30.0f});
                                     hit && hit->id == second)
                                 {
                                     point = {x / 30.0f, y / 30.0f};
                                     found = true;
                                 }
                         ASSERT_TRUE(found);
                         const glm::vec2 screenPos{region.origin.x + point.x * region.size.x,
                                                   region.origin.y + point.y * region.size.y};
                         const auto press = makeViewportInput(region, screenPos, false, true, true, false);
                         const auto held = makeViewportInput(region, screenPos, false, false, true, false);
                         const auto release = makeViewportInput(region, screenPos, false, false, false, true);
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
                     },
                     [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(frames, 3);
        cleanValidation();
    }

    TEST_F(EditorAppTest, SelectionOverlayFollowsSameFrameEditsOrderAndSavesReviewCaptures)
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

    TEST_F(EditorAppTest, MultiInstanceClickRenameDuplicateDeleteSaveAndRestore)
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

    TEST_F(EditorAppTest, UnsavedChangesDialogGuardsDirtyStateOpenClearExitAndWindowClose)
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

    TEST_F(EditorAppTest, HistoryStackFramesAndShadowCastersProtectCollectUnusedUntilLoad)
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

    TEST_F(EditorAppTest, ShadowPreviewUsesCurrentFrameAndSurvivesFilesAndWindowRecovery)
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

    TEST_F(EditorAppTest, StartupFailuresRouteGuardsAndShaderErrorCleanup)
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

    TEST_F(EditorAppTest, WorkspacePersistsDockingResetAndStartupCustomScene)
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

    TEST_F(EditorAppTest, UiInputAdapterAndOrbitCameraBatches)
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

    TEST_F(EditorAppTest, SceneViewportLayoutOrbitDragPanAndFocus)
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

    TEST_F(EditorAppTest, MixedSceneSaveLoadSameUiFrameAndShowcaseModels)
    {
        copyShowcaseModels();
        {
            Editor::EditorApp session(options());
            auto demo = std::make_shared<Editor::EditorScene>();
            bool verified = false;
            session.run({[demo](auto& app) {
                             demo->initialize(app);
                             EditorTest::setSharedEditorScene(app, *demo, false);
                         },
                         {},
                         [&](auto& app) {
                             demo->ui(app);
                             if (verified)
                                 return;
                             verified = true;

                             // 独家边界 #1：在 ui() 中改名/编辑并 save/load/clear 后，同帧 extractSceneFrame 必须立即看到新状态。
                             const auto original = app.scene().entities()[0].id;
                             const auto avocado = app.scene().find(original)->model;
                             const Scene::Transform edited({2, 1, 0}, {1, 0, 0, 0}, {-2, 0, 3});
                             ASSERT_TRUE(app.scene().setTransform(original, edited));
                             ASSERT_TRUE(demo->save(app, "same-frame.json"));
                             app.scene().setTransform(original, Scene::Transform{});
                             ASSERT_TRUE(demo->load(app, "same-frame.json"));
                             EXPECT_TRUE(app.scene().find(original) == nullptr && !demo->selected().valid());
                             const auto sameFrame = Scene::extractSceneFrame(app.scene(), {640, 480}, false);
                             ASSERT_TRUE(sameFrame);
                             EXPECT_EQ(sameFrame->draws[0].model, edited.matrix() * app.scene().entities()[0].model->draws()[0].model);

                             demo->selectModel(true);
                             const auto tea = demo->createEntity(app);
                             const auto teapot = app.scene().find(tea)->model;
                             ASSERT_NE(avocado, teapot);
                             ASSERT_TRUE(app.scene().setTransform(tea, Scene::Transform({1.6f, 0, 0}, {1, 0, 0, 0}, {0.075f, 0.075f, 0.075f})));
                             app.scene().createEntity({}, Scene::Transform({5, 1, 2}, {1, 0, 0, 1}, {-1, 0, 2}), false);
                             app.scene().setLighting({{0.2f, -1, -2}, {0.7f, 0.8f, 1}, 1.2f, 0.2f});
                             app.clearColor() = {0.1f, 0.2f, 0.3f, 1};
                             const auto pixels = TextureGpuReview::captureFrame(app.renderer(), Scene::extractSceneFrame(app.scene(), {384, 256})->input());
                             const auto uploads = count("Uploaded mesh:");
                             ASSERT_TRUE(demo->save(app, "mixed-save.json"));

                             auto invalidModel = Scene::captureScene(app.scene(), app.clearColor());
                             invalidModel.entities[2].model = Model::ModelReference("models/missing.gltf");
                             Scene::writeScene(app.fileIO(), "missing-model.json", invalidModel);
                             EXPECT_TRUE(!demo->load(app, "missing-model.json") && demo->scenePath() == "mixed-save.json" &&
                                         app.scene().find(tea) != nullptr);

                             ASSERT_TRUE(demo->load(app, "mixed-save.json"));
                             ASSERT_EQ(app.scene().entities().size(), 4u);
                             EXPECT_TRUE(app.scene().entities()[0].model == avocado && app.scene().entities()[2].model == teapot &&
                                         !app.scene().entities()[3].visible);
                             const auto restored = TextureGpuReview::captureFrame(app.renderer(), Scene::extractSceneFrame(app.scene(), {384, 256})->input());
                             ASSERT_EQ(pixels.size(), restored.size());
                             for (size_t i = 0; i < pixels.size(); ++i) EXPECT_LE(std::abs(int(pixels[i]) - int(restored[i])), 2);
                             EXPECT_EQ(count("Uploaded mesh:"), uploads);

                             demo->clear(app);
                             const auto cleared = Scene::extractSceneFrame(app.scene(), {640, 480});
                             ASSERT_TRUE(cleared && cleared->draws.empty());
                             app.requestExit();
                         }});
            EXPECT_TRUE(verified);
        }
        cleanValidation();
    }

    TEST_F(EditorAppTest, LightingWidgetsAndLiveRoutesPrepareModels)
    {
        copyMinimalObj();
        {
            auto state = std::make_shared<Lifetime>();
            Editor::EditorApp session(options(true), std::make_unique<TrackedFileIO>(state));
            CpuResource otherHold;
            const TextureResource* otherPixels = nullptr;
            size_t opens = 0;
            session.run({[&](auto& app) {
                             routes();
                             unloaded(ResourceType::SHADER, "shaders/cube.vert.spv");
                             const auto m = Model::prepareModel(app.renderer(), GLTF);
                             track(app, m, state);
                             app.scene().createEntity(m);
                             unloaded(ResourceType::MODEL, GLTF);
                             unloaded(ResourceType::TEXTURE, PNG);
                             otherHold = ResourceManager::instance().hold<TextureResource>(ResourceType::TEXTURE, PNG);
                             otherPixels = otherHold.get<TextureResource>();
                             ASSERT_NE(otherPixels, nullptr);
                             opens = state->opens;
                         },
                         [&](auto& app, const auto&) {
                             routes();
                             EXPECT_EQ(state->opens, opens);
                             const auto m = Model::prepareModel(app.renderer(), OBJ);
                             app.scene().createEntity(m);
                             EXPECT_EQ(ResourceRegistry::instance().refCount(otherHold.id()), 1u);
                             EXPECT_EQ(ResourceRegistry::instance().getObject(otherHold.id()), otherPixels);
                             track(app, m, state);
                             otherHold.reset();
                             unloaded(ResourceType::MODEL, OBJ);
                             unloaded(ResourceType::TEXTURE, PNG);
                             app.requestExit();
                         },
                         {}});
            closed(state);
        }
        {
            const auto baselineUploads = count("Uploaded mesh:");
            auto state = std::make_shared<Lifetime>();
            Editor::EditorApp session(options(true, 640, 1000), std::make_unique<TrackedFileIO>(state));
            const Render::Lighting initial{{1, 2, -3}, {0.5f, 0.75f, 1}, 0.7f, 0.2f};
            const Render::Lighting external{{2, 1, -3}, {0.2f, 0.4f, 0.8f}, 0.4f, 0.3f};
            const Render::Lighting replacement{{-2, -3, -4}, {0.8f, 0.1f, 1}, 0.5f, 0.4f};
            ImVec2 intensityClick{}, yawClick{};
            int frames = 0;
            Scene::SceneFrame retained;
            session.run({[&](auto& app) { app.scene().setLighting(initial); enableGlfwCallbacks(static_cast<GLFWwindow*>(app.window().getNativeHandle())); },
                         [&](auto& app, const auto&) {
                             auto& io = ImGui::GetIO();
                             if (frames == 3 || frames == 5) { const auto p = frames == 3 ? intensityClick : yawClick; io.AddMousePosEvent(p.x, p.y); io.AddMouseButtonEvent(0, true); }
                             if (frames == 4 || frames == 6) io.AddMouseButtonEvent(0, false);
                             if (frames == 7) app.scene().setLighting(external);
                             if (frames == 8) { auto s = std::make_unique<Scene::Scene>(); s->setLighting(replacement); app.setScene(std::move(s)); }
                             if (frames >= 9) app.requestExit();
                         },
                         [&](auto& app) {
                             ImGui::SetNextWindowPos({12, 12}, ImGuiCond_Always);
                             const auto row = ImGui::GetFrameHeightWithSpacing();
                             ImGui::SetNextWindowSize({600, 5 * row + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().WindowPadding.y}, ImGuiCond_Always);
                             ImGui::Begin("Lighting controls");
                             const float availWidth = ImGui::GetContentRegionAvail().x;
                             const float fontSize = ImGui::GetFontSize();
                             const float labelWidth = std::clamp(availWidth * 0.34f, fontSize * 5.5f, fontSize * 8.0f);
                             const float controlWidth = std::max(60.0f, availWidth - labelWidth);
                             const auto origin = ImGui::GetCursorScreenPos();
                             yawClick = {origin.x + labelWidth + controlWidth * 0.25f, origin.y + ImGui::GetFrameHeight() * 0.5f};
                             intensityClick = {origin.x + labelWidth + controlWidth * 0.8f, yawClick.y + 3 * row};
                             App::lightingControls(app.scene());
                             ImGui::End();
                             const auto frame = Scene::extractSceneFrame(app.scene(), {800, 600});
                             ASSERT_TRUE(frame);
                             EXPECT_EQ(frame->input().lighting, app.scene().lighting());
                             if (frames == 3) { EXPECT_GT(frame->lighting.intensity, 2.0f); retained = *frame; }
                             if (frames == 5) EXPECT_NE(frame->lighting.direction, initial.direction);
                             if (frames == 7) EXPECT_EQ(frame->lighting, external);
                             if (frames == 8) EXPECT_EQ(frame->lighting, replacement);
                             ++frames;
                         }});
            EXPECT_TRUE(frames == 9 && count("Uploaded mesh:") == baselineUploads);
            closed(state);
        }
        cleanValidation();
    }

    TEST_F(EditorAppTest, InitializeUpdateUiCallbacksMinimizedAndExitLifecycle)
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

    TEST_F(EditorAppTest, ContinuousPropertyEditsColorPickersAndPendingUnsavedGuard)
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
                if (ImGui::GetHoveredID() == target) { foundX = ImGui::GetIO().MousePos.x; foundY = ImGui::GetIO().MousePos.y; scanY = 0.0f; phase = nextPhase; return; }
                const float minY = props->InnerRect.Min.y + 8.0f, maxY = props->InnerRect.Max.y - 8.0f;
                scanY = (scanY < minY) ? minY : scanY + 12.0f;
                if (scanY > maxY) { scanY = minY; props->Scroll.y = props->Scroll.y >= props->ScrollMax.y ? 0.0f : std::min(props->Scroll.y + 120.0f, props->ScrollMax.y); }
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
                             props->Scroll.y = props->ScrollMax.y; ++phase; break;
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

    TEST_F(EditorAppTest, EditMenuAndWindowShortcutsUndoRedoWorkWithoutViewportFocus)
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

    TEST_F(EditorAppTest, GizmoDragGesturePriorityUndoRedoOutOfViewportAndCancel)
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

    TEST_F(EditorAppTest, GizmoOverlayClipOrderColorsHighlightShortcutsAndVisualModes)
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

    ImGuiID assetRowAction(ImGuiWindow& panel, std::string_view path, const char* action)
    {
        const auto row = ImHashStr(path.data(), path.size(), panel.GetID("##assets"));
        return ImHashStr(action, 0, row);
    }

    TEST_F(EditorAppTest, AssetsImportCancelRefreshAddSaveAndReopen)
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
                             case 5:
                                 ASSERT_EQ(app.scene().entities().size(), 1u) << editor.message();
                                 EXPECT_EQ(app.scene().entities().front().model->source().path(), imported);
                                 EXPECT_TRUE(editor.isDirty());
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
                         ++step;
                     },
                     [&](auto& app) { editor.ui(app); },
                     {}, {}, [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(step, 6);
        cleanValidation();
    }

    TEST_F(EditorAppTest, AssetsReloadButtonKeepsNewDiskModelAfterUndo)
    {
        Editor::EditorApp session(options(true, 1280, 800));
        Editor::EditorScene editor;
        Scene::EntityId id;
        std::shared_ptr<const Model::PreparedModel> oldModel;
        const glm::vec3 edited{2, 3, 4};
        int step = 0;
        int frames = 0;
        session.run({[&](auto& app) {
                         editor.initialize(app, "");
                         ASSERT_TRUE(editor.browseDirectory("models/minimal_gltf"));
                         ASSERT_TRUE(editor.addModel(app, GLTF));
                         id = editor.selected();
                         oldModel = app.scene().find(id)->model;
                         ASSERT_TRUE(editor.save(app, "reload-baseline.json"));
                         ASSERT_TRUE(editor.controller().operations().setTransform(
                             app.scene(), app.clearColor(), Scene::Transform(edited)));
                         const auto file = fs::path("assets") / GLTF;
                         std::ifstream input(file, std::ios::binary);
                         std::string text{std::istreambuf_iterator<char>(input), {}};
                         input.close();
                         const auto translation = text.find("0.25");
                         ASSERT_NE(translation, std::string::npos);
                         text.replace(translation, 4, "0.75");
                         std::ofstream(file, std::ios::binary) << text;
                     },
                     [&](auto& app, const auto&) {
                         if (++frames > 120)
                         {
                             ADD_FAILURE() << "Assets reload timed out at step " << step;
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
                         }
                         else if (step == 1 && !panel->Hidden)
                         {
                             ImGui::ActivateItemByID(assetRowAction(*panel, GLTF, "Reload"));
                             ++step;
                         }
                         else if (step == 2)
                         {
                             const auto* entity = app.scene().find(id);
                             ASSERT_NE(entity, nullptr);
                             const auto reloaded = entity->model;
                             ASSERT_NE(reloaded, oldModel) << editor.message();
                             EXPECT_NE(reloaded->draws().front().model, oldModel->draws().front().model);
                             EXPECT_EQ(entity->transform.position(), edited);
                             EXPECT_EQ(editor.cachedModels(), 1u);
                             EXPECT_TRUE(editor.isDirty());
                             EXPECT_FALSE(editor.controller().messageError());
                             ASSERT_TRUE(editor.undo(app));
                             EXPECT_EQ(app.scene().find(id)->transform.position(), glm::vec3(0));
                             EXPECT_EQ(app.scene().find(id)->model, reloaded);
                             EXPECT_FALSE(editor.isDirty());
                             ++step;
                             app.requestExit();
                         }
                     },
                     [&](auto& app) { editor.ui(app); },
                     {}, {}, [&](auto& app) { editor.layout(app); }});
        EXPECT_EQ(step, 3);
        cleanValidation();
    }

    TEST_F(EditorAppTest, ToolbarBreadcrumbsTransformResetAndSceneFileBrowserUnderValidation)
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

    TEST_F(EditorAppTest, EntityShadingComboSwitchPbrUndoRedoFailureCacheAndScreenshotUnderValidation)
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

    TEST_F(EditorAppTest, DisplayControlsHdrComparisonUndoRedoSaveAndRestoreUnderValidation)
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

    TEST_F(EditorAppTest, HierarchyTreeReparentGizmoUndoSaveAndRestartUnderValidation)
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

    TEST_F(EditorAppTest, FxaaViewCheckboxNextFrameOutputResizeOverlayAndCleanDocumentUnderValidation)
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

    TEST_F(EditorAppTest, ViewStatsPanelCpuGpuAndDrawCountsTrackShadowToggle)
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

    TEST_F(EditorAppTest, BuiltinPbrShowcaseOpensViaFileBrowserMatchesPbrBaselineAndExportsComparison)
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

    TEST_F(EditorAppTest, StudioIblViewCheckboxDefaultOffLazyLoadSameFrameOutputAndCleanDocumentUnderValidation)
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

    TEST_F(EditorAppTest, MaterialPanelSlotSelectionContinuousTransactionUndoRedoResetAndSaveReopen12UnderValidation)
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

    TEST_F(EditorAppTest, GlbImportMaterialOverridesPreservedAcrossSaveReopenAndReloadUnderValidation)
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
