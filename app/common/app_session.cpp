#include "app_session.h"

#include <imgui.h>

#include <chrono>
#include <thread>
#include <utility>

#include "app/common/ui_input.h"
#include "app/common/viewport.h"
#include "runtime/core/error/error.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/resource/loader/model_loader.h"
#include "runtime/resource/loader/float_texture_resource.h"
#include "runtime/resource/loader/shader_loader.h"
#include "runtime/resource/loader/shader_resource.h"
#include "runtime/resource/loader/texture_loader.h"
#include "runtime/resource/resource_manager.h"
#include "runtime/scene/scene_frame.h"

namespace Incubator::App
{
    AppSession::AppSession(std::string name, const SessionOptions& options, std::unique_ptr<FileIO::FileIO> fileIO)
        : name_(std::move(name)),
          fileIO_(std::move(fileIO)),
          clearColor_(options.clearColor),
          fxaaFragmentShader_(options.fxaaFragmentShader),
          iblIrradiancePath_(options.iblIrradiance),
          iblPrefilteredPath_(options.iblPrefiltered),
          iblBrdfLutPath_(options.iblBrdfLut)
    {
        shadowSettings_.depthBias = 0.003f;
        auto& manager = Resource::ResourceManager::instance();
        // 注册任何路由或启用路由清理前，先检查完整路由表。
        if (!manager.loadersEmpty() || manager.assetSource())
            throw Error::Exception(Error::Code::CONFIG, name_ + " requires an empty Loader route table");
        if (!fileIO_)
            throw Error::Exception(Error::Code::CONFIG, name_ + " requires FileIO");
        assetSource_ = std::make_unique<Resource::AssetSource>(*fileIO_);
        manager.bindAssetSource(assetSource_.get());
        ownsRoutes_ = true;
        try
        {
            manager.registerLoader(Resource::ResourceType::SHADER,
                                   std::make_unique<Resource::ShaderLoader>(*assetSource_));
            manager.registerLoader(Resource::ResourceType::TEXTURE,
                                   std::make_unique<Resource::TextureLoader>(*assetSource_));
            manager.registerLoader(Resource::ResourceType::MODEL,
                                   std::make_unique<Resource::ModelLoader>(*assetSource_));
            const auto vertex =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, options.vertexShader);
            const auto fragment =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, options.fragmentShader);
            const auto displayVertex =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, options.displayVertexShader);
            const auto displayFragment =
                manager.hold<Resource::ShaderResource>(Resource::ResourceType::SHADER, options.displayFragmentShader);
            const auto* vertexData = vertex.get<Resource::ShaderResource>();
            const auto* fragmentData = fragment.get<Resource::ShaderResource>();
            const auto* displayVertexData = displayVertex.get<Resource::ShaderResource>();
            const auto* displayFragmentData = displayFragment.get<Resource::ShaderResource>();
            if (!vertexData || !fragmentData || !displayVertexData || !displayFragmentData)
                throw Error::Exception(Error::Code::RENDER, "Cannot load application scene shaders");
            window_ = Window::createPlatformWindow(options.window);
            renderer_ = std::make_unique<Render::Renderer>(*window_, options.validation);
            renderer_->prepareSceneResources(*vertexData, *fragmentData);
            renderer_->prepareDisplayResources(*displayVertexData, *displayFragmentData);
            scene_ = std::make_unique<Scene::Scene>();
        }
        catch (...)
        {
            close();
            throw;
        }
    }

    AppSession::~AppSession()
    {
        close();
    }

    bool AppSession::isOpen() const
    {
        return renderer_ != nullptr;
    }

    void AppSession::requireOpen() const
    {
        if (!isOpen())
            throw Error::Exception(Error::Code::CONFIG, name_ + " is closed");
    }

    Window::Window& AppSession::window()
    {
        requireOpen();
        return *window_;
    }

    Render::Renderer& AppSession::renderer()
    {
        requireOpen();
        return *renderer_;
    }

    FileIO::FileIO& AppSession::fileIO()
    {
        requireOpen();
        return *fileIO_;
    }

    Resource::AssetSource& AppSession::assetSource()
    {
        requireOpen();
        return *assetSource_;
    }

    Scene::Scene& AppSession::scene()
    {
        requireOpen();
        return *scene_;
    }

    void AppSession::setScene(std::unique_ptr<Scene::Scene> scene)
    {
        requireOpen();
        if (!scene)
            throw Error::Exception(Error::Code::CONFIG, name_ + " requires a Scene");
        scene_ = std::move(scene);
        sceneChanged_ = true;
    }

    std::array<float, 4>& AppSession::clearColor()
    {
        requireOpen();
        return clearColor_;
    }

    Render::ShadowSettings& AppSession::shadowSettings()
    {
        requireOpen();
        return shadowSettings_;
    }

    bool& AppSession::fxaaEnabled()
    {
        requireOpen();
        return fxaaEnabled_;
    }

    bool AppSession::fxaaEnabled() const
    {
        requireOpen();
        return fxaaEnabled_;
    }

    bool& AppSession::iblEnabled()
    {
        requireOpen();
        return iblEnabled_;
    }

    bool AppSession::iblEnabled() const
    {
        requireOpen();
        return iblEnabled_;
    }

    const Render::SceneOutput& AppSession::sceneOutput() const
    {
        requireOpen();
        return sceneOutput_;
    }

    void AppSession::drawSceneImage()
    {
        const auto size = ImGui::GetContentRegionAvail();
        int w, h, fw, fh;
        window_->getWindowSize(w, h);
        window_->getFramebufferSize(fw, fh);
        sceneExtent_ = App::viewportExtent(size.x, size.y, w > 0 ? double(fw) / w : 0, h > 0 ? double(fh) / h : 0);
        if (sceneExtent_.width && sceneExtent_.height)
        {
            if (fxaaEnabled_ && !fxaaResourcesPrepared_)
            {
                const auto fxaa = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
                    Resource::ResourceType::SHADER, fxaaFragmentShader_);
                const auto* shader = fxaa.get<Resource::ShaderResource>();
                if (!shader)
                    throw Error::Exception(Error::Code::RENDER, "Cannot load application FXAA shader");
                renderer_->prepareFxaaResources(*shader);
                fxaaResourcesPrepared_ = true;
            }
            sceneOutput_ = renderer_->prepareSceneTarget(sceneExtent_, fxaaEnabled_);
            const auto origin = ImGui::GetCursorScreenPos();
            ImGui::Image(static_cast<ImTextureID>(renderer_->sceneImage(sceneOutput_)), size);
            const bool hovered = ImGui::IsItemHovered();
            if (hovered && (ImGui::IsMouseClicked(0) || ImGui::IsMouseClicked(1) || ImGui::IsMouseClicked(2)))
                ImGui::SetWindowFocus();
            viewportRegion_ = {{origin.x, origin.y}, {size.x, size.y}, hovered, ImGui::IsWindowFocused()};
            sceneDrawList_ = ImGui::GetWindowDrawList();
        }
    }

    void AppSession::requestExit()
    {
        exitRequested_ = true;
    }

    void AppSession::run(FrameCallbacks& callbacks)
    {
        requireOpen();
        try
        {
            if (!exitRequested_ && !window_->shouldClose() && callbacks.initialize)
                callbacks.initialize();
            auto previous = Clock::now();
            App::UiInputAdapter inputAdapter;
            while (!exitRequested_ && !window_->shouldClose())
            {
                window_->beginInputFrame();
                window_->pollEvents();
                if (!exitRequested_ && window_->shouldClose() && callbacks.closeRequested)
                    callbacks.closeRequested();
                if (exitRequested_ || window_->shouldClose())
                    break;
                const auto now = Clock::now();
                const FrameTime time{now, now - previous};
                previous = now;  // 每轮循环都推进时间，包括跳过渲染的循环。
                lastFrameTime_ = time;
                if (callbacks.update)
                    callbacks.update(time);
                if (exitRequested_ || window_->shouldClose())
                    break;
                const auto extent = renderer_->beginUiFrame();
                const bool cancelInput = !extent || !window_->input().focused() || window_->input().focusLost() ||
                                         (callbacks.layout && sceneChanged_);
                sceneChanged_ = false;
                if (cancelInput)
                {
                    inputAdapter.cancel();
                    if (callbacks.cancelInteraction)
                        callbacks.cancelInteraction();
                }
                if (!extent)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                sceneExtent_ = {};
                sceneOutput_ = {};
                viewportRegion_ = {};
                sceneDrawList_ = nullptr;
                if (callbacks.layout)
                    callbacks.layout();
                const auto renderExtent = callbacks.layout ? sceneExtent_ : *extent;
                if (callbacks.layout && (!renderExtent.width || !renderExtent.height))
                {
                    inputAdapter.cancel();
                    if (callbacks.cancelInteraction)
                        callbacks.cancelInteraction();
                }
                auto input = inputAdapter.sample(window_->input());
                if (callbacks.layout)
                    input = App::viewportInput(input, viewportRegion_, ImGui::GetIO().WantTextInput);
                if (!cancelInput && callbacks.input && renderExtent.width && renderExtent.height)
                    callbacks.input(renderExtent, input);
                if (callbacks.ui)
                    callbacks.ui();
                if (exitRequested_ || window_->shouldClose())
                {
                    renderer_->imgui().endFrame();
                    break;
                }
                const auto frame = Scene::extractSceneFrame(*scene_, renderExtent, true, shadowSettings_, iblEnabled_);
                renderer_->imgui().endFrame();
                if (frame && frame->iblEnabled && !iblResourcesPrepared_)
                {
                    auto& manager = Resource::ResourceManager::instance();
                    const auto irrHold =
                        manager.hold<Resource::FloatTextureResource>(Resource::ResourceType::TEXTURE, iblIrradiancePath_);
                    const auto preHold =
                        manager.hold<Resource::FloatTextureResource>(Resource::ResourceType::TEXTURE, iblPrefilteredPath_);
                    const auto lutHold =
                        manager.hold<Resource::FloatTextureResource>(Resource::ResourceType::TEXTURE, iblBrdfLutPath_);
                    const auto* irrData = irrHold.get<Resource::FloatTextureResource>();
                    const auto* preData = preHold.get<Resource::FloatTextureResource>();
                    const auto* lutData = lutHold.get<Resource::FloatTextureResource>();
                    if (!irrData || !preData || !lutData)
                        throw Error::Exception(Error::Code::RENDER, "Cannot load application IBL environment assets");
                    const auto irr = renderer_->createTexture(*irrData);
                    const auto pre = renderer_->createTexture(*preData);
                    const auto lut = renderer_->createTexture(*lutData);
                    renderer_->prepareIblResources(irr, pre, lut);
                    iblResourcesPrepared_ = true;
                }
                if (callbacks.layout && frame && frame->shadow && !shadowResourcesPrepared_)
                {
                    const auto depth = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
                        Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
                    const auto* shader = depth.get<Resource::ShaderResource>();
                    const auto maskVertex = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
                        Resource::ResourceType::SHADER, "shaders/shadow_mask.vert.spv");
                    const auto maskFragment = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
                        Resource::ResourceType::SHADER, "shaders/shadow_mask.frag.spv");
                    if (!shader || !maskVertex || !maskFragment)
                        throw Error::Exception(Error::Code::RENDER, "Cannot load application shadow shader");
                    renderer_->prepareShadowResources(*shader, *maskVertex.get<Resource::ShaderResource>(),
                                                     *maskFragment.get<Resource::ShaderResource>());
                    shadowResourcesPrepared_ = true;
                }
                auto* data = renderer_->imgui().drawData();
                const auto record = [&](void* commands) { renderer_->imgui().recordDrawData(data, commands); };
                if (callbacks.layout && frame)
                    renderer_->drawSceneTarget(sceneOutput_, frame->input(), clearColor_, record);
                else if (callbacks.layout)
                    renderer_->drawFrame({*extent, {}, {}}, clearColor_, record);
                else
                    renderer_->drawFrame(frame->input(), clearColor_, record);
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

    void AppSession::close() noexcept
    {
        scene_.reset();
        if (ownsRoutes_)
        {
            Resource::ResourceManager::instance().reset();
            Resource::ResourceManager::instance().bindAssetSource(nullptr);
            ownsRoutes_ = false;
        }
        renderer_.reset();
        window_.reset();
        assetSource_.reset();
        fileIO_.reset();
    }
}  // namespace Incubator::App
