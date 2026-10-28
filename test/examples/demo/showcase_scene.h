#pragma once

#include "examples/demo/model_scene.h"

namespace Incubator::DemoTest
{
    /// @brief 为共享实例、场景替换及持久化测试构造双实例场景。
    inline void setSharedModelScene(Demo::DemoApp& app, Demo::ModelScene& demo, bool useObj)
    {
        demo.selectModel(useObj);
        const auto model = app.scene().find(demo.createEntity(app))->model;
        auto scene = std::make_unique<Scene::Scene>();
        scene->setCamera(Scene::Camera({0, 0, 6}));
        scene->setLighting({{-0.6f, -0.8f, -1}, {1, 0.95f, 0.9f}, 1, 0.15f});
        const float offset = useObj ? 1.65f : 1.2f;
        const auto transform = [useObj](float x) {
            return Scene::Transform({x, useObj ? 0.0f : -0.78f, 0}, {1, 0, 0, 0}, glm::vec3(useObj ? 0.075f : 24.0f));
        };
        const auto first = scene->createEntity(model, transform(-offset));
        scene->createEntity(model, transform(offset));
        app.setScene(std::move(scene));
        demo.select(first);
    }
}  // namespace Incubator::DemoTest
