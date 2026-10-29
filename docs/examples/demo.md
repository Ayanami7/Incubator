# Demo 静态场景展示

Demo 位于 `examples/demo/`，是独立的 Runtime 示例消费者。正式编辑工具见 [Editor](../app/editor.md)。
Demo 启动时读取一次场景，提供观察和渲染参数控制；对象编辑及文件工作流由 Editor 提供。

## 构建与启动

按 [开发工作流](../../WORKFLOW.md) 使用 UTF-8 Debug/all 构建，或单独构建 `demo`，随后从构建目录运行：

```powershell
cd E:/Project/VScode/Incubator/build
./examples/demo/demo.exe
```

目标链接 IncubatorRuntime 与 imgui，显式依赖 `incubator_assets`。共用 Shader 在 `cmake/shaders.cmake` 编译，
模型、场景、字体和配置在 `cmake/assets.cmake` 单向部署到 build。可执行文件的位置不代替 cwd；
模型从 `<cwd>/assets/` 读取，运行时不回写仓库源资产。

## 启动场景和控制

`app/common/startup_config.h` 通过 FileIO 读取 cwd 的 `incubator.json`，保持已有 window 与 startupScene 字段语义。
配置缺失或字段省略使用默认值，默认场景是 `assets/scenes/shadows.json`；相对配置和场景路径按 cwd 解析。
配置、场景、模型或依赖无效会结束启动；不提交部分结果。
默认阴影场景包含 Avocado、Utah Teapot 和接收地面，模型按 Runtime ModelManager 复用。

| 操作 | 行为 |
|---|---|
| 右键拖动 | 在视口内开始轨道旋转，跨出视口后保持手势 |
| 中键拖动 | 在视口内开始平移 |
| 滚轮 | 在视口中缩放观察距离 |
| Reset camera | 恢复启动场景中的相机位姿和透视参数 |
| Lighting | 展开编辑方向光角度、线性颜色、强度和环境项 |
| Shadows | 启停本会话阴影预览，默认开启 |
| Shadow bias | 展开调整接收深度偏移，默认 0.003 |

主视口随窗口尺寸变化；最小化、失焦及无效尺寸取消当前手势，恢复后不重放旧增量。
控制面板可收起 Lighting 与 Shadow bias，让场景保持主体。相机和 UI 编辑参与当前帧提取；
灯光及阴影预览不写回源场景。Demo 不保存停靠布局，也不读取或覆盖 Editor 的 `incubator-editor-layout.ini`。

## 实现与持有期

`DemoApp` 在示例中具体组织 Window、Renderer、FileIO、Loader 和 Scene；`Showcase` 只负责静态场景准备、
相机与展示控件。共享输入消费、轨道相机、逻辑视口换算、光照控件及启动配置位于 `app/common/`，
双方没有公共 Application 基类或 Runtime 应用框架。

布局先确定图像区域与像素 extent，再消费 ImGui 已处理的输入批次、构建控件、提取 SceneFrame 并提交。
CPU 持有在准备后释放，PreparedModel 拥有 GPU 资源组；展示缓存及当前 Scene 保留模型至结束，
在途对象由 FrameSlot/Fence 保护。回调和 Scene 在 Renderer 前释放，Renderer 先关闭时外部模型晚析构仍安全。
模型 CPU 加载失败由资源门面记录一次，main 不重复记录 Model::LoadError；其余失败由处理边界记录。

## 测试

`test/examples/demo/demo_test.cpp` 验证默认场景、相机和渲染控制、最小化/尺寸恢复、布局隔离及失败清理。
启动 cwd 是 build，窗口/GPU 用例串行、逐例超时 120 秒；正式验收使用 CTest。
共用输入与配置测试位于 `test/app/common/`，编辑行为测试和独立进程场景恢复入口位于 `test/app/editor/`。
