#include <imgui.h>

#include <chrono>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/model_instance.h"
#include "runtime/platform/window/window.h"
#include "runtime/render/integration/imgui_renderer.h"
#include "runtime/render/renderer.h"
#include "runtime/resource/resource_registry.h"
#include "scene_assets.h"
#include "scene_controls.h"
#include "scene_mesh.h"
#include "validation_mode.h"

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
#else
int main(int argc, char* argv[])
#endif
{
    Incubator::Log::Registry::init(Incubator::Log::Level::DEBUG);
    auto log = Incubator::Log::Registry::get("demo");
    log.info("Engine starting");

    try
    {
        const auto argument = [&](int index) -> std::string {
#ifdef _WIN32
            // Windows resource paths enter the engine as UTF-8, including model dependencies.
            return Incubator::Utils::Utf8::toUtf8String(std::filesystem::path(argv[index]));
#else
            return argv[index];
#endif
        };
        const bool modelMode = argc >= 2 && argument(1) == "--model";
        if (argc > 3 || (modelMode && argc != 3))
        {
            log.error("Usage: demo.exe [texture virtual path] [both|cube|plane]");
            log.error("       demo.exe --model <model virtual path (.gltf|.obj)>");
            return 1;
        }
        Incubator::Demo::SceneAssetPaths paths;
        const std::string assetPath = modelMode ? argument(2) : argc >= 2 ? argument(1) : std::string(paths.texture);
        if (modelMode)
            paths.model = assetPath;
        else
            paths.texture = assetPath;
        std::string meshName = "both";
        if (argc == 3 && !modelMode)
        {
            meshName = argument(2);
            if (meshName != "both" && meshName != "cube" && meshName != "plane")
                throw Incubator::Error::Exception(Incubator::Error::Code::CONFIG,
                                                  "Expected initial visibility 'both', 'cube' or 'plane'");
        }
        log.info("Scene {}: {}", modelMode ? "model" : "texture", assetPath);
        Incubator::Demo::SceneAssets assets(paths);

        Incubator::Window::WindowDesc windowDesc;
        windowDesc.title = "Incubator Engine";
        windowDesc.width = 1280;
        windowDesc.height = 720;
        auto window = Incubator::Window::createPlatformWindow(windowDesc);

        Incubator::Render::Renderer renderer(*window, Incubator::Demo::VALIDATION_MODE);
        renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
        std::shared_ptr<const Incubator::Model::PreparedModel> model;
        Incubator::Render::Mesh cube, plane;
        Incubator::Render::Material cubeMaterial, planeMaterial;
        if (modelMode)
            model = Incubator::Model::prepareModel(renderer, paths.model);
        else
        {
            const auto texture = renderer.createTexture(*assets.texture);
            cubeMaterial = renderer.createMaterial({texture});
            planeMaterial = renderer.createMaterial({texture, {0.5f, 1.0f, 0.5f}});
            const auto data = Incubator::Demo::makeCubeMesh();
            cube = renderer.createMesh(data.vertices, data.indices);
            const auto planeData = Incubator::Demo::makePlaneMesh();
            plane = renderer.createMesh(planeData.vertices, planeData.indices);
        }
        auto scene = modelMode ? Incubator::Demo::makeModelSceneControls(assetPath) : Incubator::Demo::SceneControls{};
        scene.cube.visible = meshName != "plane";
        scene.plane.visible = meshName != "cube";
        log.info("Initial scene visibility: {}", meshName);
        const auto textureId = assets.texture ? assets.texture->id() : Incubator::Resource::ResourceId{};
        assets.releaseCpuResources();
        if (textureId.valid() && Incubator::Resource::ResourceRegistry::instance().getObject(textureId))
            throw Incubator::Error::Exception(Incubator::Error::Code::RENDER, "CPU scene texture was not unloaded");

        while (!window->shouldClose())
        {
            window->pollEvents();
            const auto extent = renderer.beginUiFrame();
            if (!extent)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            Incubator::Demo::drawSceneControls(scene, assetPath, modelMode);
            std::vector<Incubator::Render::DrawItem> draws;
            if (modelMode)
            {
                std::vector<Incubator::Model::ModelInstance> instances;
                for (const auto* object : {&scene.cube, &scene.plane})
                    if (object->visible)
                        instances.push_back({model, Incubator::Demo::makeSceneModel(object->transform)});
                draws = Incubator::Model::makeModelDraws(instances);
            }
            else
                draws = Incubator::Demo::makeSceneDraws(scene, cube, plane, cubeMaterial, planeMaterial);
            const auto frame = Incubator::Demo::makeSceneFrame(*extent, draws, scene.camera);
            renderer.imgui().endFrame();

            void* uiDrawData = renderer.imgui().drawData();
            renderer.drawFrame(frame, scene.clearColor, [&renderer, uiDrawData](void* commandBuffer) {
                renderer.imgui().recordDrawData(uiDrawData, commandBuffer);
            });
        }
    }
    catch (const Incubator::Error::Exception& error)
    {
        log.error("Engine error: {}", error.what());
        return 1;
    }
    catch (const std::exception& error)
    {
        log.error("Unexpected error: {}", error.what());
        return 1;
    }

    log.info("Engine shutdown complete");
    return 0;
}
