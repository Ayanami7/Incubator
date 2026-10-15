# 纹理加载阶段完成后复核清单

记录日期：2026-10-15。
复核时机：Task 01–06 全部完成后，与 Task 06 的阶段验收报告一并核对。
当前状态：第 1 项已完成精简并通过测试；第 2 项待最终复核。本记录不表示已发现功能 bug。

复核遵循 stop-that-shit 原则：以当前契约和实际行为为依据，复用仍适用于最终实现的证据。仅在实现变化、测试失败或存在未解决问题时补充或重跑必要检查。

## 1. CPU 输入长度检查与测试是否存在冗余

**修复前观察**

- TextureLoader 将 PNG 输入限制为最多 64 MiB，同时检查输入长度是否超出 `int` 和 `size_t` 的表示范围。
- 在当前支持的 Windows x64 环境下，64 MiB 上限已经保证这两种转换安全；后面的表示范围检查不会独立触发。
- 输入长度测试中的 `INT_MAX + 1` 和 `UINT64_MAX` 同样先被 64 MiB 上限拒绝，与 `64 MiB + 1` 用例覆盖相同的拒绝条件，不能作为独立转换边界已验证的证据。

**依据位置**

- [TextureLoader::readInput](../../../../src/runtime/resource/loader/texture_loader.cpp)。
- [TextureInputLimitTest / InputBoundaries](../../../../test/runtime/texture_loader_test.cpp)。
- [Task 03 输入限制与完成条件](./task-03-texture-loading.md)。

**最终核查要求**

1.1 核对最终输入上限、支持平台与类型表示范围，确认上述判断是否仍成立。

1.2 若仍采用当前 64 MiB 上限，可精简被该上限完全覆盖的表示范围检查及对应测试参数；保留空输入、超过实际输入上限且在分配和读取前失败的行为覆盖。

1.3 核对最终测试与文档表述，避免将命中同一上限的多个大数用例记为独立边界覆盖。本项属于可精简内容，不应扩展成新的边界测试矩阵。

**核查结果（2026-10-15）：** 已保留 `0 < size <= 64 MiB` 运行时约束，用编译期断言保证上限可由 `int` 和 `size_t` 表示，移除重复的表示范围比较。`InputBoundaries` 只保留空输入与 `64 MiB + 1` 两个参数，继续断言在读取前失败；`INT_MAX + 1`、`UINT64_MAX` 不再作为独立边界用例。TextureLoader 接口说明和 Task 03 转换约定已同步。本轮 Debug all 构建成功，完整 CTest 391/391 通过（含 TextureLoader 相关 50 项）；见 [最终回归日志](../../../../build/review/final-fix-tests.log)。

## 2. 区分 GPU 离屏颜色回读与实际交换链显示证据

**当前观察**

- `TextureGpuReview::capture` 使用生产 Shader 和立方体 +Z 面网格，在独立的离屏颜色图像上绘制并回读像素；该图像采用当前 Surface 实际选出的 UNORM 格式。
- 此回读能够核对 GPU 输出的方向、通道、opaque alpha、中灰以及 ImGui 编码空间混合结果，但没有直接读取实际交换链图像。
- 另有 Required Validation 测试覆盖 Renderer 的实际交换链绘制、缩放和关闭。颜色数值回读与这些运行结果各有观测边界，最终报告需要准确说明。

**依据位置**

- [TextureGpuReview::capture](../../../../test/runtime/support/texture_gpu_review.h)。
- [ActualShaderAndCubeFaceMatchReferenceAndUiGrayOnGpu / RequiredValidationDrawResizeAndShutdown](../../../../test/runtime/vulkan_review_test.cpp)。
- [Task 04 完成条件与完成记录](./task-04-renderer-texture.md)。
- [Task 06 显示结果、实际 Surface 组合及 Validation 证据要求](./task-06-verification.md)。

**最终核查要求**

2.1 在 Task 06 报告中明确标注颜色数值证据来自离屏附件，记录其格式、所用 Shader 和网格，以及已核对的像素结果。

2.2 按 Task 05–06 已有要求，核对两张正式样本通过 Demo / Renderer 实际交换链呈现的显示记录，覆盖方向、通道、中灰和 opaque 显示；记录实际设备、format、colorSpace 与启动方式。

2.3 分别关联显示结果、颜色数值回读和 Required Validation 日志。仅通过受控格式列表验证的 RGBA 回退或拒绝行为，应保留其受控测试身份；未经实际运行的呈现路径不记为 GPU 显示验收通过。

2.4 复用足以满足上述要求的已有证据，不因本清单额外引入 Renderer 截图接口、通用回读框架或重复 GPU 测试。

**核查结果：** 待填写实际显示记录、数值回读与 Validation 日志的证据位置，以及未解决项。
