# Incubator

Incubator 是基于 C++20 和 CMake 的图形渲染器/游戏引擎项目。Runtime 当前包含 Core、Platform、Resource 和 Render 模块。

## 当前 Demo

`examples/demo` 展示旋转的纹理立方体与可交互 ImGui 界面。Renderer 管理 Vulkan 设备、交换链、帧同步和绘制；Demo 通过 ResourceManager 加载构建时编译的 SPIR-V CPU 资源。Shader 资源路径为 `shaders/cube.vert.spv` 和 `shaders/cube.frag.spv`，构建产物位于运行目录下的 `assets/shaders/`。

## 开发文档

- [架构设计](./ARCHITECTURE.md)
- [CMake 结构](./docs/cmake.md)
- [开发工作流](./WORKFLOW.md)
- [Render 模块](./docs/runtime/render.md)
- [Resource 模块](./docs/runtime/resource/resource.md)

