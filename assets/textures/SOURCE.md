# 原创纹理样本

`reference_rgba.png` 与 `direction_rgba.png` 为本项目生成的原创像素样本，无外部素材。
本目录这两张图片、生成脚本及其像素基准以 **CC0-1.0** 提供：
https://creativecommons.org/publicdomain/zero/1.0/

生成方式：在项目根执行 `python test/runtime/fixtures/textures/generate_fixtures.py`。
Python 3 标准库构造 PNG 签名、IHDR、filter=0 的三行像素、单 IDAT 与 IEND；
zlib level=0，非隔行，8-bit RGBA。生成器同时产生必要的测试格式样本，字节变体由测试内构造。

- 尺寸：4×3，左上原点，行从上到下，行内从左到右。
- 文件：119 字节，SHA-256 `a609d42b2ac0a5fab2e9ffac52387d4c479018e729e46d9536598e213baa10b3`。
- RGB 为 sRGB 编码字节，alpha 为线性 straight alpha；透明像素的 RGB 保留。
- CMake 的 `demo_textures` 将它部署到 `<build>/assets/textures/reference_rgba.png`，
  Demo 和 GPU 复核目标均依赖此目标；修改源图后构建会更新目标图，不依赖重新链接。

每格是 RGBA 字节；左上坐标 `(0,0)`：

| y / x | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| 0 | (255,0,0,255) | (0,255,0,255) | (0,0,255,255) | (255,255,0,255) |
| 1 | (0,255,255,255) | (255,0,255,255) | (128,128,128,255) | (255,255,255,255) |
| 2 | (255,128,0,255) | (128,0,255,128) | (17,34,51,0) | (0,0,0,255) |

该图非方形，三个色序不同，用于识别翻转、镜像、RGBA 顺序与中灰转换。
当前场景保持不透明：透明或半透明格子的 RGB 仍会显示，alpha 保存由 CPU 测试检查。

## 第二张正式样本：direction_rgba.png

在项目根执行 `python assets/textures/generate_direction.py` 可复现；仅使用 Python 3 标准库。
PNG 为 3×4、8-bit RGBA、非隔行、filter=0、单 IDAT、zlib level=0，120 字节。
SHA-256：`b4decf515a27c9e66160cd18a2e0a6e2b11fc102dfe26c358ebed49ceddf4cf7`。
RGB/alpha 语义与首图相同，以下为完整 RGBA 字节：

| y / x | 0 | 1 | 2 |
|---|---|---|---|
| 0 | (255,255,255,255) | (255,128,0,255) | (0,0,0,255) |
| 1 | (0,0,255,255) | (128,128,128,255) | (255,0,0,255) |
| 2 | (0,255,0,255) | (255,0,255,128) | (0,255,255,255) |
| 3 | (17,34,51,0) | (255,255,0,255) | (128,0,255,255) |

+Z 面顶部为白→橙→黑，下一行蓝→中灰→红，底部暗色→黄→紫；与首图尺寸、色序均不同。
中灰应为 128（GPU 数字检查允许 ±1），半透明品红和全透明暗色仍按不透明材质显示 RGB。
两张图均由 `demo_textures` 部署到 `<build>/assets/textures/`，无需手工复制或重新链接。
Demo 无参数使用首图，传入 `textures/direction_rgba.png` 后重启显示第二图。
