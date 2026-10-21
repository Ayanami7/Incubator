#include "runtime/application/application.h"

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

namespace Incubator::Application
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

    Session::Session(SessionOptions options, std::unique_ptr<FileIO::FileIO> fileIO)
        : fileIO_(std::move(fileIO)), clearColor_(options.clearColor)
    {
        auto& manager = Resource::ResourceManager::instance();
        // Check the entire table before registering anything or enabling route cleanup.
        if (!manager.loadersEmpty())
            throw Error::Exception(Error::Code::CONFIG, "Application session requires an empty Loader route table");
        if (!fileIO_)
            throw Error::Exception(Error::Code::CONFIG, "Application session requires FileIO");
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

    Session::~Session()
    {
        close();
    }

    bool Session::isOpen() const
    {
        return renderer_ != nullptr;
    }

    void Session::requireOpen() const
    {
        if (!isOpen())
            throw Error::Exception(Error::Code::CONFIG, "Application session is closed");
    }

    Window::Window& Session::window()
    {
        requireOpen();
        return *window_;
    }

    Render::Renderer& Session::renderer()
    {
        requireOpen();
        return *renderer_;
    }

    Scene::Scene& Session::scene()
    {
        requireOpen();
        return *scene_;
    }

    void Session::setScene(std::unique_ptr<Scene::Scene> scene)
    {
        requireOpen();
        if (!scene)
            throw Error::Exception(Error::Code::CONFIG, "Application session requires a Scene");
        scene_ = std::move(scene);
    }

    std::array<float, 4>& Session::clearColor()
    {
        requireOpen();
        return clearColor_;
    }

    void Session::requestExit()
    {
        exitRequested_ = true;
    }

    void Session::run(Callbacks callbacks)
    {
        requireOpen();
        try
        {
            if (!exitRequested_ && !window_->shouldClose() && callbacks.initialize)
                callbacks.initialize(*this);
            auto previous = Clock::now();
            while (!exitRequested_ && !window_->shouldClose())
            {
                window_->pollEvents();
                if (exitRequested_ || window_->shouldClose())
                    break;
                const auto now = Clock::now();
                const FrameTime time{now, now - previous};
                previous = now;  // Advance on every loop, including skipped rendering loops.
                if (callbacks.update)
                    callbacks.update(*this, time);
                if (exitRequested_ || window_->shouldClose())
                    break;
                const auto extent = renderer_->beginUiFrame();
                if (!extent)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
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

    void Session::close() noexcept
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
}  // namespace Incubator::Application
