#pragma once

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


namespace Incubator::Test
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

    inline WindowHooks hookWindowCallbacks(GLFWwindow* handle, bool setFocus = true)
    {
        WindowHooks hooks{glfwSetMouseButtonCallback(handle, nullptr), glfwSetCursorPosCallback(handle, nullptr),
                          glfwSetWindowFocusCallback(handle, nullptr)};
        glfwSetMouseButtonCallback(handle, hooks.mouse);
        glfwSetCursorPosCallback(handle, hooks.cursor);
        glfwSetWindowFocusCallback(handle, hooks.focus);
        if (setFocus && hooks.focus) hooks.focus(handle, GLFW_TRUE);
        return hooks;
    }

    inline void enableGlfwCallbacks(GLFWwindow* handle, bool cursorEnter = true)
    {
        if (cursorEnter)
            if (const auto enter = glfwSetCursorEnterCallback(handle, nullptr))
                { glfwSetCursorEnterCallback(handle, enter); enter(handle, GLFW_TRUE); }
        if (const auto focus = glfwSetWindowFocusCallback(handle, nullptr))
            { glfwSetWindowFocusCallback(handle, focus); focus(handle, GLFW_TRUE); }
        glfwFocusWindow(handle);
    }

    inline void activateWindowButton(const char* windowName, const char* buttonLabel)
    {
        auto* window = ImGui::FindWindowByName(windowName);
        ASSERT_NE(window, nullptr) << windowName;
        auto& ctx = *ImGui::GetCurrentContext();
        ctx.NavActivateId = ctx.NavActivateDownId = window->GetID(buttonLabel);
    }

    inline App::UiInput makeViewportInput(const App::ViewportRegion& region, glm::vec2 screenPos, bool captureMouse,
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
            for (const char* file : {"mixed.json", "shadows.json", "pbr-showcase.json", "lighting-showcase.json"})
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

    inline ImGuiID assetRowAction(ImGuiWindow& panel, std::string_view path, const char* action)
    {
        const auto row = ImHashStr(path.data(), path.size(), panel.GetID("##assets"));
        return ImHashStr(action, 0, row);
    }
}  // namespace Incubator::Test
