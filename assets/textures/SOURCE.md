# 原创纹理样本

`reference_rgba.png` 为本项目生成的原创像素样本，无外部素材。
本目录该图片及其像素基准以 **CC0-1.0** 提供：
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
