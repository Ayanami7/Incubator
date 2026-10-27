#include <iostream>
#include <string_view>

#include "examples/demo/demo_app.h"
#include "examples/demo/model_scene.h"
#include "runtime/scene/scene_description.h"

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    const std::string_view mode(argv[1]);
    int result = 0;
    try
    {
        Incubator::Demo::DemoOptions options;
        options.window.visible = false;
        options.validation = Incubator::Render::RendererValidationMode::Required;
        Incubator::Demo::DemoApp app(options);
        app.run({[&](auto& session) {
                     Incubator::Demo::ModelScene demo;
                     demo.initialize(session);
                     if (mode == "write")
                     {
                         demo.update(session, {});
                         demo.update(session, {});
                         demo.selectModel(true);
                         const auto tea = demo.createEntity(session);
                         session.scene().setTransform(
                             tea, Incubator::Scene::Transform({1, 0, 0}, {1, 0, 0, 0}, {-0.1f, 0.2f, 0}));
                         session.scene().createEntity({}, Incubator::Scene::Transform({5, 6, 7}), false);
                         session.scene().setCamera(Incubator::Scene::Camera({1, 2, 8}, {1, 0, 0, 0}, {0.9f, 0.2f, 90}));
                         session.scene().setLighting({{0, -2, -3}, {0.2f, 0.6f, 0.8f}, 2, 0.3f});
                         session.clearColor() = {0.2f, 0.3f, 0.4f, 0.5f};
                         if (!demo.save(session, argv[2]))
                         {
                             std::cerr << demo.message();
                             result = 1;
                         }
                     }
                     else if (mode == "read")
                     {
                         if (!demo.load(session, argv[2]))
                         {
                             std::cerr << demo.message();
                             result = 1;
                         }
                         else
                         {
                             const auto d = Incubator::Scene::captureScene(session.scene(), session.clearColor());
                             const Incubator::Model::PrepareOptions expected{
                                 Incubator::Render::TextureMipMode::FullChain,
                                 {Incubator::Render::TextureFilter::Linear, Incubator::Render::TextureFilter::Linear,
                                  Incubator::Render::MipFilter::Linear},
                                 Incubator::Render::ShadingMode::Lambert};
                             if (d.entities.size() != 4 || !d.entities[0].model || !d.entities[2].model ||
                                 d.entities[3].model || d.entities[3].visible ||
                                 d.entities[0].model->path() != "models/avocado/gltf/avocado.gltf" ||
                                 d.entities[2].model->path() != "models/utah_teapot/utah_teapot.obj" ||
                                 d.entities[0].model->options() != expected || d.entities[2].model->options() != expected ||
                                 d.entities[2].transform.scale() != glm::vec3(-0.1f, 0.2f, 0) ||
                                 d.camera.position() != glm::vec3(1, 2, 8) ||
                                 d.camera.perspective().verticalFov != 0.9f ||
                                 d.camera.perspective().nearPlane != 0.2f || d.camera.perspective().farPlane != 90 ||
                                 d.lighting.direction != glm::vec3(0, -2, -3) ||
                                 d.lighting.color != glm::vec3(0.2f, 0.6f, 0.8f) || d.lighting.intensity != 2 ||
                                 d.lighting.ambient != 0.3f ||
                                 d.clearColor != std::array<float, 4>{0.2f, 0.3f, 0.4f, 0.5f})
                                 result = 1;
                         }
                     }
                     else
                         result = 2;
                     session.requestExit();
                 },
                 {},
                 {}});
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what();
        return 1;
    }
    std::cout << mode << (result == 0 ? " succeeded\n" : " failed\n");
    return result;
}
