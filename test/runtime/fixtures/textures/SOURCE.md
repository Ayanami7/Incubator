# PNG 行为测试输入

全部 PNG 由本目录 `generate_fixtures.py` 原创生成，无外部素材；图片、生成器、
预期字节及 manifest 以 **CC0-1.0** 提供：
https://creativecommons.org/publicdomain/zero/1.0/
使用 Python 3 标准库即可重建。保留 12 个必要的格式、压缩与尺寸样本；测试读取这些文件，
其余字节变体在内存中构造，不在构建时运行生成器。

RGBA8 基准直接复用 `assets/textures/reference_rgba.png`，不保存重复图片；预期 4×3 的完整像素
见资产来源说明及 `texture_loader_test.cpp`。`manifest.json` 记录保留样本的长度和指纹。

- Gray/RGB/Gray+Alpha、调色板、RGB/RGBA16、Adam7 与 CgBI 保留独立编码样本。
  RGB/Gray tRNS 由测试向基准 Gray/RGB 图插入 chunk；gray2/gray4 由有效的单黑像素 gray1 图
  修改位深并重算 IHDR CRC，三种位深下像素数据均合法。
- gAMA/cHRM/sRGB chunk 在测试内插入；iCCP 保留包含压缩 profile 的样本。
  它们用于验证字节不随元数据转换；iCCP 的测试 profile
  是不解析的合成字节，不是经过验证的 ICC 色彩配置，不用于宣称 ICC 支持。
- palette8、gray1/2/4、RGB/RGBA16、1×1 Adam7 与 CgBI 包含对应变体的头及压缩数据，
  Loader 根据首个 IHDR 和声明范围拒绝，不以解码成功作为支持依据。
- 7/20/72 字节截断与 IDAT 压缩头 `00 00`（重算 CRC）由测试修改 RGBA8 基准字节，验证不同失败原因。
- `crc_only` 仅改变 IDAT CRC 的第一个字节；`missing_iend_crc` 删除最后 4 字节；
  `trailing_bytes` 追加尾部数据。均在测试内构造；固定解码器允许它们成功并保留像素，这不表示完整性校验。
- `missing_iend` 删除最后 12 字节，用于检查没有 decoder reason 时的明确诊断。
- 0、4097、UINT32_MAX 尺寸及非法 method/首 chunk/头长度在测试内构造为受控头部变异，
  用于验证解码前拒绝；不把这些输入称作有效 PNG。
- `max_width` 和 `max_height` 是 4096×1 / 1×4096 的有效 Gray8 图，
  验证可接受的尺寸边界而不分配 64 MiB 像素。
