# TextureResource 与 TextureLoader

## CPU 载荷

TextureResource 拥有宽、高及 `std::vector<uint8_t>` 像素；`width()`、`height()` 和
`pixels()` 只读访问。格式固定为紧密 RGBA8，无行 padding，左上原点，行从上到下。
RGB 为 sRGB 编码字节，alpha 为线性 straight alpha，透明 RGB 保留；不做 gamma、ICC、
预乘或垂直翻转。公共接口不包含 stb 或 Vulkan 类型，也不持有 GPU 对象。

宽、高各在 1–4096，有效 RGBA 载荷最多 64 MiB；构造边界检查尺寸和载荷长度一致性。
资源状态、挂载与引用计数由 Manager/Registry 控制，Loader 不设置 READY。

## 路径与读取

TextureLoader 借用 AssetSource，经固定目录根打开虚拟路径；默认根由构造时 cwd/assets 解析，
也支持指定根。资源身份规范化继续由 Registry 负责，源检查只读文件路径的词法边界。
Loader 只使用已打开 FileHandle 的 `size()`，检查 `0 < size <= 64 MiB` 后分配输入。
编译期断言保证该上限可由 `int` 与 `size_t` 表示，不重复检查已被上限覆盖的范围。
循环累积短读；未读满遇到 read=0 就报告 expected/actual，
读取异常保留原因，不将部分输入送入解码器。

## PNG 解码范围

解码前检查 PNG 签名、完整的首个 13-byte IHDR，以及尺寸、输出乘法和范围。
只接收非隔行 8-bit Gray、RGB、Gray+Alpha、RGBA，compression/filter method 均为 0。
调色板、低位深、16-bit、Adam7 与 CgBI 输入明确失败。灰度复制 RGB，缺省 alpha 为 255；
合法 Gray/RGB tRNS 按解码器语义展开。后缀不用于判断格式。

stb_image v2.30 固定提交、MIT 许可及指纹保存在 `3rdparty/stb/SOURCE.md`。
唯一实现文件由独立 `stb_image` 静态目标编译，集中定义 `STBI_ONLY_PNG`、
`STBI_NO_STDIO`、`STBI_MAX_DIMENSIONS=4096`，Runtime PRIVATE 链接。
TextureLoader 从内存解码，要求 4 通道；临时像素由 RAII 调用 `stbi_image_free`，
核对返回尺寸后复制到引擎 vector，转存或构造异常时也释放临时结果。

成功解码不意味着完整 PNG 验证：此版本不校验 CRC，可能容忍尾部变化；不额外引入
CRC/chunk 完整性检查或色彩管理。gAMA/cHRM/iCCP/sRGB 元数据均不触发颜色转换，
资产 RGB 按约定解释为 sRGB。

## 失败与生命周期

文件缺失、输入范围不支持、读取或解码失败均抛出带 UTF-8 物理路径和原因的 IO 异常。
空 decoder reason 使用 `PNG decode failed (decoder supplied no reason)`。
Loader 不记录同一异常；经 Manager 调用时，Manager 记录一次并返回 nullptr，失败不挂载对象。
保持有效持有时，修正文件后可重试。挂载对象重复加载会复用；release 到零卸载像素，
登记保留，重新 acquire 后才能重新持有并加载。覆盖 Loader 或 reset 不使旧缓存失效。

`Renderer::createTexture` 借用 TextureResource 并同步上传为 SRGB GPU Image，返回类型化
Texture 引用，不保留 CPU 像素或资源指针；调用方可在下一次绘制前 release 到零以卸载 CPU 对象。交换链重建只复用
GPU 纹理。设备尺寸和用途限制由上传边界检查，具体颜色与 GPU 生命周期见
[Render 模块](../../render.md)。
