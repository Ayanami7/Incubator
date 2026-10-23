#include "demo_app.h"
#include "model_scene.h"

#include <chrono>
#include <thread>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/texture_loader.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/resource/resource_registry.h"
#include "runtime/scene/scene_frame.h"

namespace Incubator::Demo
{
    namespace
    {
        struct ShaderHold
        {
            explicit ShaderHold(std::string_view path)
            {
                auto& manager = Resource::ResourceManager::instance();
                id = Resource::ResourceRegistry::instance().lookup(Resource::ResourceType::SHADER, path);
                if (id.valid())
                    manager.acquire(id);
                else
                    id = manager.registerResource(Resource::ResourceType::SHADER, path);
            }

            ~ShaderHold()
            {
                if (id.valid())
                    Resource::ResourceManager::instance().release(id);
            }

            ShaderHold(const ShaderHold&) = delete;
            ShaderHold& operator=(const ShaderHold&) = delete;
            Resource::ResourceId id;
        };
    }  // namespace

    DemoApp::DemoApp(DemoOptions options, std::unique_ptr<FileIO::FileIO> fileIO)
        : fileIO_(std::move(fileIO)), clearColor_(options.clearColor)
    {
        auto& manager = Resource::ResourceManager::instance();
        // 注册任何路由或启用路由清理前，先检查完整路由表。
        if (!manager.loadersEmpty())
            throw Error::Exception(Error::Code::CONFIG, "DemoApp requires an empty Loader route table");
        if (!fileIO_)
            throw Error::Exception(Error::Code::CONFIG, "DemoApp requires FileIO");
        ownsRoutes_ = true;
        try
        {
            manager.registerLoader(Resource::ResourceType::SHADER, std::make_unique<Resource::ShaderLoader>(*fileIO_));
            manager.registerLoader(Resource::ResourceType::TEXTURE,
                                   std::make_unique<Resource::TextureLoader>(*fileIO_));
            manager.registerLoader(Resource::ResourceType::MODEL, std::make_unique<Resource::ModelLoader>(*fileIO_));
            const ShaderHold vertex(options.vertexShader);
            const ShaderHold fragment(options.fragmentShader);
            const auto* vertexData = manager.get<Resource::ShaderResource>(vertex.id);
            const auto* fragmentData = manager.get<Resource::ShaderResource>(fragment.id);
            if (!vertexData || !fragmentData)
                throw Error::Exception(Error::Code::RENDER, "Cannot load application scene shaders");
            window_ = Window::createPlatformWindow(options.window);
            renderer_ = std::make_unique<Render::Renderer>(*window_, options.validation);
            renderer_->prepareSceneResources(*vertexData, *fragmentData);
            scene_ = std::make_unique<Scene::Scene>();
        }
        catch (...)
        {
            close();
            throw;
        }
    }

    DemoApp::~DemoApp()
    {
        close();
    }

    bool DemoApp::isOpen() const
    {
        return renderer_ != nullptr;
    }

    void DemoApp::requireOpen() const
    {
        if (!isOpen())
            throw Error::Exception(Error::Code::CONFIG, "DemoApp is closed");
    }

    Window::Window& DemoApp::window()
    {
        requireOpen();
        return *window_;
    }

    Render::Renderer& DemoApp::renderer()
    {
        requireOpen();
        return *renderer_;
    }

    Scene::Scene& DemoApp::scene()
    {
        requireOpen();
        return *scene_;
    }

    void DemoApp::setScene(std::unique_ptr<Scene::Scene> scene)
    {
        requireOpen();
        if (!scene)
            throw Error::Exception(Error::Code::CONFIG, "DemoApp requires a Scene");
        scene_ = std::move(scene);
    }

    std::array<float, 4>& DemoApp::clearColor()
    {
        requireOpen();
        return clearColor_;
    }

    void DemoApp::requestExit()
    {
        exitRequested_ = true;
    }

    void DemoApp::run()
    {
        run(makeCallbacks());
    }

    void DemoApp::run(Callbacks callbacks)
    {
        requireOpen();
        try
        {
            if (!exitRequested_ && !window_->shouldClose() && callbacks.initialize)
                callbacks.initialize(*this);
            auto previous = Clock::now();
            UiInputAdapter inputAdapter;
            while (!exitRequested_ && !window_->shouldClose())
            {
                window_->beginInputFrame();
                window_->pollEvents();
                if (exitRequested_ || window_->shouldClose())
                    break;
                const auto now = Clock::now();
                const FrameTime time{now, now - previous};
                previous = now;  // 每轮循环都推进时间，包括跳过渲染的循环。
                if (callbacks.update)
                    callbacks.update(*this, time);
                if (exitRequested_ || window_->shouldClose())
                    break;
                const auto extent = renderer_->beginUiFrame();
                const bool cancelInput = !extent || !window_->input().focused() || window_->input().focusLost();
                if (cancelInput)
                {
                    inputAdapter.cancel();
                    if (callbacks.cancelInteraction)
                        callbacks.cancelInteraction(*this);
                }
                if (!extent)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                const auto input = inputAdapter.sample(window_->input());
                if (!cancelInput && callbacks.input)
                    callbacks.input(*this, *extent, input);
                if (callbacks.ui)
                    callbacks.ui(*this);
                if (exitRequested_ || window_->shouldClose())
                {
                    renderer_->imgui().endFrame();
                    break;
                }
                const auto frame = Scene::extractSceneFrame(*scene_, *extent);
                renderer_->imgui().endFrame();
                auto* data = renderer_->imgui().drawData();
                renderer_->drawFrame(frame->input(), clearColor_,
                                     [&](void* commands) { renderer_->imgui().recordDrawData(data, commands); });
            }
            callbacks = {};
            close();
        }
        catch (...)
        {
            callbacks = {};
            close();
            throw;
        }
    }

    void DemoApp::close() noexcept
    {
        scene_.reset();
        if (ownsRoutes_)
        {
            Resource::ResourceManager::instance().reset();
            ownsRoutes_ = false;
        }
        renderer_.reset();
        window_.reset();
        fileIO_.reset();
    }
}  // namespace Incubator::Demo
