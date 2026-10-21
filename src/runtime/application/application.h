#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "runtime/core/clock/clock.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/renderer.h"
#include "runtime/scene/scene.h"

namespace Incubator::Application
{
    struct SessionOptions
    {
        Window::WindowDesc window;
        Render::RendererValidationMode validation = Render::RendererValidationMode::Disabled;
        std::string vertexShader = "shaders/cube.vert.spv";
        std::string fragmentShader = "shaders/cube.frag.spv";
        std::array<float, 4> clearColor{0.12f, 0.16f, 0.22f, 1};
    };

    struct FrameTime
    {
        Clock::TimePoint now;
        Clock::Duration delta;

        double deltaSeconds() const
        {
            return std::chrono::duration<double>(delta).count();
        }
    };

    class Session;

    struct Callbacks
    {
        std::function<void(Session&)> initialize;
        std::function<void(Session&, const FrameTime&)> update;
        std::function<void(Session&)> ui;
    };

    // Single-window/main-thread application session, exclusively owning all Loader routes.
    class Session
    {
    public:
        explicit Session(SessionOptions options = {},
                         std::unique_ptr<FileIO::FileIO> fileIO = FileIO::createPlatformFileIO());
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&) = delete;
        Session& operator=(Session&&) = delete;

        // Run once, without reentry. Both return and callback failure close this session;
        // errors propagate to the consumer for logging. Callback values are released first.
        void run(Callbacks callbacks);
        void requestExit();
        bool isOpen() const;

        // Borrow only while open. Do not retain Scene references across setScene.
        Window::Window& window();
        Render::Renderer& renderer();
        Scene::Scene& scene();
        void setScene(std::unique_ptr<Scene::Scene> scene);
        std::array<float, 4>& clearColor();

    private:
        void requireOpen() const;
        void close() noexcept;

        std::unique_ptr<FileIO::FileIO> fileIO_;
        std::unique_ptr<Window::Window> window_;
        std::unique_ptr<Render::Renderer> renderer_;
        std::unique_ptr<Scene::Scene> scene_;
        std::array<float, 4> clearColor_;
        bool ownsRoutes_ = false;
        bool exitRequested_ = false;
    };
}  // namespace Incubator::Application
