# Application 应用会话

完整交互示例见 [Scene Runtime 示例](../examples/scene_demo.md)：初始化 glTF，后续更新准备 OBJ，
UI 直接操作当前 Scene 的实体及 Camera，Session 负责提取、提交与关闭。

`runtime/application/application.h` 提供 `Incubator::Application::Session`、SessionOptions、Callbacks 和 FrameTime，
归入既有 IncubatorRuntime。Session 拥有单 Window、Renderer、FileIO、当前 Scene 及整个 Loader 路由表的独占持有期，
通过直接回调驱动 SceneFrame 提取和提交；不提供继承式应用基类、系统调度器或多会话并发。

## 最小用法

```cpp
#include <imgui.h>
#include "runtime/application/application.h"
#include "runtime/model/prepared_model.h"

using Incubator::Application::Session;
Incubator::Application::SessionOptions options;
options.window.title = "Scene application";
options.validation = Incubator::Render::RendererValidationMode::Optional;
Session session(options);
session.run({
    [](Session& app) {
        auto model = Incubator::Model::prepareModel(app.renderer(), "models/minimal_gltf/model.gltf");
        app.scene().setCamera(Incubator::Scene::Camera({0, 0, 6}));
        app.scene().createEntity(model, Incubator::Scene::Transform({-1, 0, 0}));
        app.scene().createEntity(model, Incubator::Scene::Transform({1, 0, 0}));
    },
    [](Session& app, const Incubator::Application::FrameTime& time) {
        const double dt = time.deltaSeconds();
        // Apply this loop's input/logic to app.scene(), using dt as needed.
    },
    [](Session& app) {
        if (ImGui::Button("Clear scene")) app.scene().clear();
        if (ImGui::Button("Exit")) app.requestExit();
    }});
// session.isOpen() is now false; borrowed Window/Renderer/Scene references have expired.
```

工作目录为所用构建目录，Shader/模型及依赖按既有 assets 流程部署。SessionOptions 保存 WindowDesc、
validation（默认 Disabled）、两个 Shader 虚拟路径及清屏颜色；默认 Shader 为 shaders/cube.vert.spv 与
shaders/cube.frag.spv，可由消费者替换。clearColor() 返回当前清屏参数引用，RGB 使用 Render 既有 sRGB 约定。
构造参数可转入 FileIO 所有权；默认创建平台 FileIO，传空抛 CONFIG。

## 初始化与资源服务

构造时先检查 `ResourceManager::loadersEmpty()`，覆盖整个表，包括 Session 不使用的 TEXT 或其他路由。
任一路由已存在即在任何注册前抛 CONFIG，原路由不覆盖、不清除；此失败路径没有取得独占权，不能 reset。

空表确认后记录持有状态，再注册 Shader/Texture/Model Loader。使用 register/acquire/release 取得两份 Shader
的本次 CPU 持有，创建 Window、Renderer 并显式 prepareSceneResources 一次，创建默认 Scene；成功或失败均释放
本次 Shader 持有。部分初始化失败只清理已取得的所有权，Loader 在 FileIO 之前销毁。

FileIO 和全部 Loader 持续存活至会话关闭。初始化回调及后续更新可显式调用已有
`Model::prepareModel(app.renderer(), path)`，不另建资源门面；首次 CPU 模型/像素卸载、Scene clear 或 setScene
不会 reset 路由。实体修改和普通提取不加载文件、上传或创建材质；再次显式准备依旧遵循每次准备内部共享的约定。
会话期间其他调用方不得注册、覆盖或清空 Loader 路由，沿用现有单线程调用规则。

本次 CPU 持有与其他资源持有者独立，关闭不调用 Registry.clear；其他持有者继续负责自己的 release。
删除实体、切换 Scene 或 PreparedModel 最后一个逻辑引用结束，不等于即时释放 GPU；GPU 仍由 Renderer 持有至关闭。

## 驱动与回调顺序

Session 不可复制或移动。`run(Callbacks)` 执行一次且不重入；各回调可为空。

1. initialize(Session&) 执行一次，用于模型准备和初始 Scene 构造；已请求退出/关窗时不再调用。
2. 每轮轮询事件并检查关闭，再计算 FrameTime，调用 update(Session&, const FrameTime&)。
3. 调用 Renderer::beginUiFrame 取得实际 extent/UI 帧；无 extent 时跳过后续绘制阶段并等待 10ms，继续下一轮事件/更新。
4. 调用 ui(Session&)，随后提取当前 Scene；UI 修改参与当次提取。
5. 结束 ImGui 帧，将 SceneFrame::input() 和 UI 一起交给 Renderer。提交返回后本轮快照销毁。

FrameTime 保存本轮单调 Clock 时间点 now 和与上一轮时间点之差 delta，deltaSeconds() 返回 double 秒数。
初始化结束后开始循环计时，每轮都先推进上一时间点，包括最小化/零尺寸循环；恢复时不再次累计多个跳帧的总时长。
FrameTime 引用只借用于更新回调，需长期保存时复制其值。首版没有固定步长或补帧。

requestExit() 或 Window::setShouldClose(true) 停止后续阶段/循环；从 update 请求退出不再进入 UI，
从 UI 请求退出则结束已开始的 ImGui 帧而不提取/提交；停止后不再调用后续更新。
Renderer 仍负责 extent 变化跳帧、在途资源持有及 Fence/呈现等待，Session 不添加第二套 GPU 同步。

## Scene 替换与访问

window()/renderer()/scene()/clearColor() 仅在会话开放期间借用，关闭后访问抛 CONFIG。
setScene(unique_ptr<Scene>) 接管非空场景，传空抛 CONFIG，旧 Scene 及其实体引用立即失效。
在初始化、更新或 UI 阶段、下一次提取之前调用；下一次提取只读取新 Scene 的 Camera 与实体，旧 EntityId 不在新 Scene 生效。
Scene 不可移动，以 unique_ptr 转移所有权；不引入场景序列化或 SceneManager。

## 正常与异常关闭

run 正常结束或发生回调/模型准备异常时，先释放本次回调值，再按以下顺序关闭：

1. 清理当前 Scene 及其模型引用；本次回调值独占的 RAII CPU 持有已随其结束，外部副本仍由调用方负责。
2. 仅在已取得路由独占权时移除本会话 Loader。
3. 销毁 Renderer，由其等待在途提交与呈现后释放 GPU。
4. 销毁 Window，最后结束 FileIO 持有。

run 返回或重新抛出前资源清理已完成，isOpen() 返回 false；未运行就析构也使用相同关闭顺序。
外部自行保留的 PreparedModel 不延长 Renderer 寿命，其 GPU 引用随 Renderer 关闭失效。
run 只清理并透传异常，不重复记录错误；消费者负责应用错误边界，Loader/Manager 延用原有诊断约定。

原 Demo 的程序网格及直接 Renderer 路径继续可用；Session 的实际示例入口集成独立于本模块接口。
