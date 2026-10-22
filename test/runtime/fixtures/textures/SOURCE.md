# PNG 测试纹理

本目录保存纹理加载和渲染测试使用的固定输入。文件与生成脚本由本项目编写，按 **CC0-1.0** 提供：
https://creativecommons.org/publicdomain/zero/1.0/

## 精确渲染样本

`reference_rgba.png` 与 `direction_rgba.png` 用于检查像素通道、纹理方向和 GPU 输出。两者均为 8-bit RGBA、
非隔行、filter=0、zlib level=0；RGB 是 sRGB 编码字节，alpha 是 straight alpha。当前材质不透明，
透明像素仍按其 RGB 显示。

`reference_rgba.png` 为 4×3、119 字节，SHA-256 为
`a609d42b2ac0a5fab2e9ffac52387d4c479018e729e46d9536598e213baa10b3`：

| y / x | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| 0 | (255,0,0,255) | (0,255,0,255) | (0,0,255,255) | (255,255,0,255) |
| 1 | (0,255,255,255) | (255,0,255,255) | (128,128,128,255) | (255,255,255,255) |
| 2 | (255,128,0,255) | (128,0,255,128) | (17,34,51,0) | (0,0,0,255) |

`direction_rgba.png` 为 3×4、120 字节，SHA-256 为
`b4decf515a27c9e66160cd18a2e0a6e2b11fc102dfe26c358ebed49ceddf4cf7`：

| y / x | 0 | 1 | 2 |
|---|---|---|---|
| 0 | (255,255,255,255) | (255,128,0,255) | (0,0,0,255) |
| 1 | (0,0,255,255) | (128,128,128,255) | (255,0,0,255) |
| 2 | (0,255,0,255) | (255,0,255,128) | (0,255,255,255) |
| 3 | (17,34,51,0) | (255,255,0,255) | (128,0,255,255) |

`generate_fixtures.py` 生成 reference 样本及 PNG 解码输入，`generate_direction.py` 生成 direction 样本；
两者都只使用 Python 3 标准库，并只写入本目录。运行构建不会执行生成器。CMake 将这两张样本和最小模型
部署到独立的 `<build>/test-fixtures/assets/`，不会部署到 Demo 的 `<build>/assets/`。

## PNG 解码器输入

`generate_fixtures.py` 还生成用于单元测试的编码样本和 `manifest.json`。它们覆盖 Gray/RGB/Gray+Alpha、
调色板、RGB/RGBA16、Adam7 与 CgBI；其他字节变体在测试内构造，不在构建时运行生成器。

- RGB/Gray tRNS 由测试向基准 Gray/RGB 图插入 chunk；gray2/gray4 由有效的单黑像素 gray1 图修改位深并重算 IHDR CRC。
- gAMA/cHRM/sRGB chunk 在测试内插入；iCCP 保留包含压缩 profile 的样本。测试 profile 是合成字节，
  不表示引擎支持 ICC 色彩配置。
- palette8、gray1/2/4、RGB/RGBA16、1×1 Adam7 与 CgBI 输入用于验证头部和声明范围拒绝行为，
  不以解码成功作为支持依据。
- 截断输入、IDAT 压缩头变体、CRC/IEND 变化、尾随数据、异常尺寸及非法头部由测试在内存中构造。
- `max_width` 和 `max_height` 是 4096×1 / 1×4096 的有效 Gray8 图，验证尺寸边界而不分配 64 MiB 像素。
