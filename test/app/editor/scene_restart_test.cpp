#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "app/editor/editor_app.h"
#include "app/editor/editor_scene.h"
#include "app/editor/gizmo.h"
#include "runtime/scene/scene_description.h"
#include "test/app/editor/showcase_scene.h"

namespace
{
    std::optional<Incubator::Editor::OverlaySegment> findAxisSegment(const Incubator::Editor::ProjectedGizmo& gizmo,
                                                                     Incubator::Editor::GizmoAxis axis)
    {
        for (const auto& segment : gizmo.segments)
        {
            if (segment.axis == axis && glm::length(segment.segment.end - segment.segment.start) > 1.0f)
                return segment.segment;
        }
        return std::nullopt;
    }

    Incubator::App::UiInput makeViewportInput(const Incubator::App::ViewportRegion& region, glm::vec2 screenPos,
                                              bool down, bool pressed, bool released)
    {
        Incubator::App::UiInput input{};
        input.position = {screenPos.x - region.origin.x, screenPos.y - region.origin.y};
        input.select = {down, pressed, released};
        input.captureMouse = false;
        input.captureKeyboard = false;
        return input;
    }

    bool expectNearTransform(const Incubator::Scene::Transform& actual, const Incubator::Scene::Transform& expected)
    {
        const auto posDiff = glm::length(actual.position() - expected.position());
        const auto scaleDiff = glm::length(actual.scale() - expected.scale());
        const auto qDot = std::abs(glm::dot(actual.rotation(), expected.rotation()));
        return posDiff < 1e-4f && scaleDiff < 1e-4f && qDot > 0.9999f;
    }
}  // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
        return 2;
    const std::string_view mode(argv[1]);
    const std::string expectedPath = std::string(argv[2]) + ".expected.txt";
    int result = 0;
    try
    {
        Incubator::App::SessionOptions options;
        options.window.visible = false;
        options.window.width = 1280;
        options.window.height = 800;
        options.validation = Incubator::Render::RendererValidationMode::Required;
        Incubator::Editor::EditorApp app(options);
        Incubator::Editor::EditorScene demo;
        Incubator::Scene::EntityId tea{};
        int frame = 0;
        bool writeDone = false;

        app.run({[&](auto& session) {
                     demo.initialize(session, mode == "read" ? argv[2] : "assets/scenes/mixed.json");
                     if (mode == "write")
                     {
                         Incubator::EditorTest::setSharedEditorScene(session, demo, false);
                         demo.selectModel(true);
                         tea = demo.createEntity(session);
                         session.scene().setName(tea, "茶壶 ## 一");
                         session.scene().setTransform(
                             tea, Incubator::Scene::Transform({0, 0, 0}, {1, 0, 0, 0}, {-0.1f, 0.2f, 0.1f}));
                         demo.select(session, tea);
                     }
                     else if (mode == "read")
                     {
                         if (demo.selected().valid() || demo.isDirty() || demo.canUndo() || demo.canRedo() ||
                             demo.gizmoMode() != Incubator::Editor::GizmoMode::Translate)
                         {
                             std::cerr << "Editor session state should reset on fresh restart\n";
                             result = 1;
                         }
                         Incubator::Scene::Transform expectedTea;
                         glm::vec3 p{}, s{};
                         glm::quat r{};
                         {
                             std::ifstream expectedIn(expectedPath);
                             if (!(expectedIn >> p.x >> p.y >> p.z >> r.w >> r.x >> r.y >> r.z >> s.x >> s.y >> s.z))
                             {
                                 std::cerr << "Missing expected transform file: " << expectedPath << "\n";
                                 result = 1;
                             }
                             expectedTea.setPosition(p);
                             expectedTea.setRotation(r);
                             expectedTea.setScale(s);
                         }
                         const auto d = Incubator::Scene::captureScene(session.scene(), session.clearColor());
                         const Incubator::Model::PrepareOptions expected{
                             Incubator::Render::TextureMipMode::FullChain,
                             {Incubator::Render::TextureFilter::Linear, Incubator::Render::TextureFilter::Linear,
                              Incubator::Render::MipFilter::Linear},
                             Incubator::Render::ShadingMode::Lambert};
                         if (d.entities.size() != 7 || !d.entities[0].model || !d.entities[2].model ||
                             !d.entities[3].model || d.entities[2].name != "茶壶 ## 一" ||
                             d.entities[3].name != "茶壶 ## 一 副本" || d.entities[4].name != "空 ### 对象" ||
                             d.entities[4].model || d.entities[4].visible || !d.entities[5].model ||
                             !d.entities[6].model ||
                             session.scene().entities()[2].model != session.scene().entities()[3].model ||
                             !expectNearTransform(d.entities[2].transform, expectedTea) ||
                             !expectNearTransform(d.entities[3].transform, expectedTea) ||
                             d.entities[2].transform.position().x <= 0.15f ||
                             std::abs(d.entities[2].transform.rotation().w) >= 0.999f ||
                             std::abs(d.entities[2].transform.scale().x - (-0.1f)) <= 0.01f ||
                             d.entities[5].model->path() != "models/restart-import/sculpture.obj" ||
                             d.entities[5].model != d.entities[6].model ||
                             session.scene().entities()[5].model != session.scene().entities()[6].model ||
                             d.entities[5].model->options() != expected ||
                             d.entities[0].model->path() != "models/avocado/gltf/avocado.gltf" ||
                             d.entities[2].model->path() != "models/utah_teapot/utah_teapot.obj" ||
                             d.entities[0].model->options() != expected || d.entities[2].model->options() != expected ||
                             d.camera.position() != glm::vec3(1, 2, 8) || d.camera.perspective().verticalFov != 0.9f ||
                             d.camera.perspective().nearPlane != 0.2f || d.camera.perspective().farPlane != 90 ||
                             d.lighting.direction != glm::vec3(0, -2, -3) ||
                             d.lighting.color != glm::vec3(0.2f, 0.6f, 0.8f) || d.lighting.intensity != 2 ||
                             d.lighting.ambient != 0.3f || d.clearColor != std::array<float, 4>{0.2f, 0.3f, 0.4f, 0.5f})
                         {
                             std::cerr << "Restored scene verification failed\n";
                             result = 1;
                         }
                         session.requestExit();
                     }
                     else
                     {
                         result = 2;
                         session.requestExit();
                     }
                 },
                 [&](auto& session, const auto&) {
                     if (mode != "write" || writeDone)
                         return;
                     ++frame;
                     const auto extent = session.sceneExtent();
                     const auto& region = session.viewportRegion();
                     if (frame < 4 || extent.width == 0 || extent.height == 0 || region.size.x <= 0 ||
                         region.size.y <= 0 || region.size.x > 1000)
                     {
                         if (frame > 200)
                         {
                             std::cerr << "Write pass timed out waiting for viewport: frame=" << frame
                                       << " extent=" << extent.width << "x" << extent.height
                                       << " region=" << region.size.x << "x" << region.size.y << "\n";
                             result = 1;
                             writeDone = true;
                             session.requestExit();
                         }
                         return;
                     }

                     // 1. 视口 Gizmo 平移（X 轴）
                     demo.setGizmoMode(Incubator::Editor::GizmoMode::Translate);
                     const auto transGizmo =
                         Incubator::Editor::projectGizmo(session.scene(), tea, demo.gizmoMode(), extent, region);
                     const auto xSeg = findAxisSegment(transGizmo, Incubator::Editor::GizmoAxis::X);
                     if (!xSeg)
                     {
                         std::cerr << "Missing translate X gizmo axis\n";
                         result = 1;
                         writeDone = true;
                         session.requestExit();
                         return;
                     }
                     const glm::vec2 xMid = (xSeg->start + xSeg->end) * 0.5f;
                     demo.input(session, extent, makeViewportInput(region, xMid, true, true, false));
                     demo.input(session, extent,
                                makeViewportInput(region, xMid + glm::vec2(80.0f, 0.0f), true, false, false));
                     demo.input(session, extent,
                                makeViewportInput(region, xMid + glm::vec2(80.0f, 0.0f), false, false, true));
                     const auto posAfterTranslate = session.scene().find(tea)->transform.position();
                     if (posAfterTranslate.x <= 0.15f)
                     {
                         std::cerr << "Gizmo translate did not move entity\n";
                         result = 1;
                     }

                     // 2. 视口 Gizmo 旋转（Z 轴圆环）并验证 Undo / Redo 闭环
                     demo.setGizmoMode(Incubator::Editor::GizmoMode::Rotate);
                     const auto rotGizmo =
                         Incubator::Editor::projectGizmo(session.scene(), tea, demo.gizmoMode(), extent, region);
                     const auto zRingSeg = findAxisSegment(rotGizmo, Incubator::Editor::GizmoAxis::Z);
                     if (!zRingSeg)
                     {
                         std::cerr << "Missing rotate Z gizmo ring\n";
                         result = 1;
                         writeDone = true;
                         session.requestExit();
                         return;
                     }
                     const glm::vec2 zRingPt = (zRingSeg->start + zRingSeg->end) * 0.5f;
                     const glm::vec2 zRingTangent = glm::normalize(zRingSeg->end - zRingSeg->start);
                     demo.input(session, extent, makeViewportInput(region, zRingPt, true, true, false));
                     demo.input(session, extent,
                                makeViewportInput(region, zRingPt + zRingTangent * 60.0f, true, false, false));
                     demo.input(session, extent,
                                makeViewportInput(region, zRingPt + zRingTangent * 60.0f, false, false, true));
                     const auto rotAfterDrag = session.scene().find(tea)->transform.rotation();
                     if (std::abs(rotAfterDrag.w) >= 0.999f || !demo.undo(session) ||
                         std::abs(session.scene().find(tea)->transform.rotation().w) < 0.999f ||
                         !demo.redo(session) ||
                         std::abs(glm::dot(session.scene().find(tea)->transform.rotation(), rotAfterDrag)) < 0.9999f)
                     {
                         std::cerr << "Gizmo rotate undo/redo roundtrip failed\n";
                         result = 1;
                     }

                     // 3. 视口 Gizmo 缩放（局部 X 轴）
                     demo.setGizmoMode(Incubator::Editor::GizmoMode::Scale);
                     const auto scaleGizmo =
                         Incubator::Editor::projectGizmo(session.scene(), tea, demo.gizmoMode(), extent, region);
                     const auto scaleXSeg = findAxisSegment(scaleGizmo, Incubator::Editor::GizmoAxis::X);
                     if (!scaleXSeg)
                     {
                         std::cerr << "Missing scale X gizmo axis\n";
                         result = 1;
                         writeDone = true;
                         session.requestExit();
                         return;
                     }
                     const glm::vec2 scaleXMid = (scaleXSeg->start + scaleXSeg->end) * 0.5f;
                     const glm::vec2 scaleXDir = glm::normalize(scaleXSeg->end - scaleXSeg->start);
                     demo.input(session, extent, makeViewportInput(region, scaleXMid, true, true, false));
                     demo.input(session, extent,
                                makeViewportInput(region, scaleXMid + scaleXDir * 70.0f, true, false, false));
                     demo.input(session, extent,
                                makeViewportInput(region, scaleXMid + scaleXDir * 70.0f, false, false, true));
                     const auto finalTeaTransform = session.scene().find(tea)->transform;
                     if (std::abs(finalTeaTransform.scale().x - (-0.1f)) <= 0.01f)
                     {
                         std::cerr << "Gizmo scale did not update entity scale\n";
                         result = 1;
                     }

                     // 4. 复制、临时删除与 Undo/Redo、空实体及隔离目录新模型导入共享
                     demo.select(session, tea);
                     const auto teaCopy = demo.duplicateSelected(session);
                     if (!teaCopy.valid())
                     {
                         std::cerr << "Duplicate failed\n";
                         result = 1;
                     }
                     const auto temp = demo.createEntity(session);
                     demo.select(session, temp);
                     demo.deleteSelected(session);
                     if (!demo.undo(session) || !session.scene().find(temp) || !demo.redo(session) ||
                         session.scene().find(temp))
                     {
                         std::cerr << "Delete undo/redo failed\n";
                         result = 1;
                     }
                     session.scene().createEntity({}, Incubator::Scene::Transform({5, 6, 7}), false, "空 ### 对象");
                     std::filesystem::create_directories("assets/models/restart-import");
                     std::filesystem::copy_file("assets/models/utah_teapot/utah_teapot.obj",
                                                "assets/models/restart-import/sculpture.obj",
                                                std::filesystem::copy_options::overwrite_existing);
                     if (!demo.addModel(session, "models/restart-import/sculpture.obj") ||
                         !demo.addModel(session, "models/restart-import/sculpture.obj"))
                     {
                         std::cerr << demo.message();
                         result = 1;
                     }
                     session.scene().setCamera(Incubator::Scene::Camera({1, 2, 8}, {1, 0, 0, 0}, {0.9f, 0.2f, 90}));
                     session.scene().setLighting({{0, -2, -3}, {0.2f, 0.6f, 0.8f}, 2, 0.3f});
                     session.clearColor() = {0.2f, 0.3f, 0.4f, 0.5f};

                     {
                         std::ofstream expectedOut(expectedPath);
                         expectedOut << std::setprecision(9) << finalTeaTransform.position().x << ' '
                                     << finalTeaTransform.position().y << ' ' << finalTeaTransform.position().z << ' '
                                     << finalTeaTransform.rotation().w << ' ' << finalTeaTransform.rotation().x << ' '
                                     << finalTeaTransform.rotation().y << ' ' << finalTeaTransform.rotation().z << ' '
                                     << finalTeaTransform.scale().x << ' ' << finalTeaTransform.scale().y << ' '
                                     << finalTeaTransform.scale().z << '\n';
                     }
                     if (!demo.save(session, argv[2]) || demo.isDirty())
                     {
                         std::cerr << demo.message();
                         result = 1;
                     }
                     else
                     {
                         std::ifstream savedIn(argv[2]);
                         const std::string json((std::istreambuf_iterator<char>(savedIn)),
                                                std::istreambuf_iterator<char>());
                         if (json.find("\"version\"") == std::string::npos ||
                             json.find("\"dirty\"") != std::string::npos ||
                             json.find("\"undo") != std::string::npos || json.find("\"redo") != std::string::npos ||
                             json.find("\"gizmo") != std::string::npos)
                         {
                             std::cerr << "Saved JSON contaminated with editor state\n";
                             result = 1;
                         }
                     }
                     writeDone = true;
                     session.requestExit();
                 },
                 [&](auto& session) { demo.ui(session); },
                 [&](auto& session) { demo.cancelInteraction(session); },
                 [&](auto& session, auto extent, const auto& input) { demo.input(session, extent, input); },
                 [&](auto& session) { demo.layout(session); },
                 [&](auto& session) { demo.requestClose(session); }});

        if (mode == "write" && !writeDone)
        {
            std::cerr << "Write pass did not complete\n";
            result = 1;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what();
        return 1;
    }
    std::cout << mode << (result == 0 ? " succeeded\n" : " failed\n");
    return result;
}
