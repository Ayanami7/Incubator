# 项目约束与规范

## 项目概述

Incubator 是一个 C++ 图形渲染器/游戏引擎项目。当前以 Windows、Vulkan 1.3、单窗口和主线程同步调用为基础，核心能力集中在 `IncubatorRuntime` 静态库，`examples/demo/` 是当前的示例消费者。

## 技术栈

- **语言**：C++20
- **构建系统**：CMake 3.22+
- **渲染后端**：Vulkan 1.3
- **测试**：GoogleTest；TestMate 用于编辑器快速反馈，CTest 用于正式复核与阶段验收

## 项目总体结构

```text
Incubator/
├── 3rdparty/           # 第三方依赖库
├── assets/             # 项目资产源文件
├── cmake/              # 构建辅助与资产部署规则
├── bin/                # 二进制输出预留目录，实际位置以 CMake 为准
├── build/              # 构建产物、部署资产与测试输出
├── src/
│   └── runtime/        # Core、Platform、Resource、Model、Scene、Render 逻辑模块
├── examples/
│   └── demo/           # 引擎能力展示程序
├── test/               # 单元测试、集成测试与精确 fixture
├── docs/               # 构建、模块接口与使用说明
├── .agents/specs/      # 可选方向、当前任务与任务归档
├── ARCHITECTURE.md     # 当前架构、职责边界与所有权
├── WORKFLOW.md         # 构建、测试与提交规范
├── incubator.json      # 默认应用启动配置，构建时部署到运行目录
├── CMakeLists.txt      # 构建入口
└── README.md           # 项目说明
```

`apps/incubator/` 是正式应用的候选目录，目前尚未创建；规划见 [TO_DO.md](./.agents/specs/TO_DO.md)，构建约定见 [CMake 结构](./docs/cmake.md#正式应用目标apps-下规划尚未实施)。不要将规划目录或接口当作已实现能力。

## 工作约定

- 无明确要求时，不读写 `archived/` 目录下的文件。
- 构建与测试命令遵循 [WORKFLOW.md](./WORKFLOW.md)，配置和构建统一使用 UTF-8 代码页。
- 测试入口分工遵循 [WORKFLOW.md](./WORKFLOW.md#6-运行测试)：编辑器使用 TestMate 快速反馈，正式复核与阶段验收使用 CTest，原有任务验收条件继续有效；CTest 默认串行，cwd、串行约束与超时以 CMake 注册属性为准。
- Demo 的运行 cwd 必须为对应构建目录（本工作区为 `E:/Project/VScode/Incubator/build`），资产从 `<cwd>/assets/` 读取；可执行文件所在目录与 cwd 分别约定。
- `.agents/` 下的文件一律不允许跟踪进入 Git。没有明确要求时，不修改 `README.md`。
- 遵循**不要过度设计和防御性编程**：以实际需求和现有职责为依据，复用现有实现，保留必要校验，不提前引入通用框架或额外机制。
- 日志遵循[调用约定](./docs/runtime/log.md#调用约定)：记录有诊断价值的事件，错误在承担处理责任的边界记录，带上必要上下文；稳定帧和高频操作不刷日志，不以日志数量作为覆盖指标。
- 项目自有文件中的说明性注释统一使用简体中文，覆盖 C++、Shader、CMake、脚本和配置示例；新增或修改注释时使用完整的中文说明，避免整句英文或不必要的中英文混杂。标识符、API 名称、标准技术术语、规范引用及 `namespace` 等结构标记保留原文；第三方代码和自动生成文件保持原样。
- C++ 接口说明采用 Doxygen 格式，具体写法遵循[注释规范](./.agents/rules/code_style.md#注释规范)。日常修改同步整理涉及范围内的注释，全仓历史注释另行集中处理。

## 文档职责

| 文档 | 内容 |
|---|---|
| [ARCHITECTURE.md](./ARCHITECTURE.md) | 当前架构、模块依赖、数据流、所有权与设计原则；不维护演进规划或完成清单 |
| [当前任务](./.agents/specs/tasks/README.md) | 已明确的阶段范围、子任务、验收条件与完成简记 |
| [CMake 结构](./docs/cmake.md) | 构建目标、依赖、资产部署与运行 cwd；规划内容明确标注 |
| [模块文档](./docs/runtime/)与 [Demo 说明](./docs/examples/demo.md) | 公开接口、具体支持范围与使用方式，不存放阶段验收报告 |
| [WORKFLOW.md](./WORKFLOW.md)与[代码风格](./.agents/rules/code_style.md) | 构建、测试、提交及编码规范 |
| [TO_DO.md](./.agents/specs/TO_DO.md) | 宽泛的可选开发方向，不区分近期与远期，不记录完成状态 |

文档与源码存在冲突时，对照当前公开接口及实现确认行为，并同步修正负责该约定的文档。任务完成情况以任务记录和实际验证为准，不将候选方向视为实施承诺。

## 推荐阅读顺序

首次接手时，按以下顺序建立上下文，随后按任务范围补读：

1. 阅读本文件、[开发工作流](./WORKFLOW.md)和[代码风格](./.agents/rules/code_style.md)，明确本机开发约束。
2. 阅读 [ARCHITECTURE.md](./ARCHITECTURE.md)，了解当前分层与所有权；涉及构建时查阅 [CMake 结构](./docs/cmake.md)。
3. 阅读[当前任务](./.agents/specs/tasks/README.md)，确定本次工作的范围和验收要求；讨论后续方向时查阅 [TO_DO.md](./.agents/specs/TO_DO.md)。
4. 阅读 [Demo 使用说明](./docs/examples/demo.md)、[main.cpp](./examples/demo/main.cpp)和 [DemoApp](./examples/demo/demo_app.h)，串起会话初始化、更新、交互、帧提取与关闭。
5. 根据任务查阅 [Resource](./docs/runtime/resource/resource.md)、[Model](./docs/runtime/model.md)、[Scene](./docs/runtime/scene.md)和 [Render](./docs/runtime/render.md)，对照对应公开头文件确认资源持有、实体状态及帧数据借用期。
6. 补读受影响的实现与测试，复用已有入口和 fixture，按修改范围执行必要验证。


