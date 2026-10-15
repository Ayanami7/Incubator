# Task 03：完成 CPU 纹理资源与同步加载

状态：✅ 已完成（2026-10-14）。
前置依赖：[Task 01](./task-01-design-contract.md)、[Task 02](./task-02-asset-path.md)。
总体约束：[architecture.md](./architecture.md)。

## 目标

通过现有 ResourceManager 加载外部图片，得到身份明确、拥有解码像素的 TextureResource，形成可供 Render 消费的 CPU 资源链路。

## 范围

- 按 Task 01 固定的提交或版本接入 stb_image 和 PNG 输入范围。依赖放在 `3rdparty/stb/`，保留上游头文件与许可证，新增唯一实现编译入口和独立 CMake 静态库目标；在 `3rdparty/CMakeLists.txt` 注册，由 IncubatorRuntime PRIVATE 链接。
- 由依赖目标集中配置 `STBI_ONLY_PNG`、`STBI_NO_STDIO`；实现宏不出现在公共头文件或 TextureLoader 中，不重复编译 stb 实现，第三方告警处理不扩散到 Runtime。
- 定义 TextureResource 的尺寸、RGBA8 像素载荷和必要的颜色语义，不包含 GPU 对象。只暴露引擎自己的数据；解码临时内存按 stb_image 约定释放，包括载荷转存失败等异常路径，Resource 不保存依赖解码器存活的裸像素指针。
- 实现 TextureLoader，复用 FileIO、已确定的资源定位约定和现有 Loader 路由，通过内存解码入口调用 stb_image，不调用其文件读取接口。
- 在读取边界按 FileHandle 的实际读取量取得完整输入；正常短读继续读取，未取得完整输入就遇到 EOF 或读取异常时加载失败，不把部分输入交给解码器当作完整文件。
- 在读取和解码边界落实 Task 01 确定的格式范围、解码器输入长度限制、有效尺寸及像素字节数检查；利用解码器已有的尺寸检查能力，在大块分配前防止尺寸计算溢出或超过支持范围，不依赖分配失败作为唯一检查。
- 提供有已知尺寸、关键像素和方向的最小 PNG 样本、来源许可及预期结果，供本任务测试和 Task 04 使用。
- 建立最小样本的 CMake 部署目标，明确源文件、目标文件及增量更新依赖，并接入 Demo 与 vulkan_review_test 的构建依赖；按 Task 02 约定使其与构建生成的 SPIR-V 同时可定位。仅修改图片也要触发更新，不仅依赖 Demo 链接后的 POST_BUILD；此项在 Task 04 前完成，Task 05 负责扩展正式样本和 Demo 运行说明。
- 沿用 ResourceManager 的缓存、状态设置、异常处理与引用计数语义。
- 接入 Runtime CMake 和必要的行为测试，更新对应模块设计文档。

## 交付

TextureResource、TextureLoader、stb_image 独立静态库接入、最小样本及其构建部署目标、来源许可与预期结果，以及完整同步加载链路的测试和文档。

## 完成条件

- 已知非对称、非方形样本加载得到正确尺寸、关键 RGBA 像素、行顺序与颜色语义，Manager 返回 READY 对象；覆盖选定格式范围内必要的通道转换。
- 中文路径可加载；重复加载复用同一挂载对象。
- 引用归零卸载 CPU 对象且保留登记；重新 acquire 后加载能够重建对象。重复 registerResource 不增加引用，测试不得将其当作重新持有。
- 缺失文件、未读满即 EOF、读取异常、PNG 识别或解码失败、不支持的 PNG 输入变体及超出限制的输入按约定失败，不挂载半成品对象。按 Task 01 指定位置或方式制作截断及损坏反例；仅 CRC 修改及解码器可容忍的尾部变动不自动要求失败，不将解码成功称为完整性校验通过。
- 行为测试区分能读完的多次短读与提前 EOF，覆盖解码长度及尺寸计算边界；不为边界验证实际分配巨量内存。
- 加载失败后可使用修正的文件重新尝试，诊断包含 UTF-8 路径和实际读取/解码原因，同一异常不在多层重复记录；记录固定版本对仅 CRC 修改反例的行为及检查范围。
- 独立静态库的实现仅编译一次，Runtime 私有链接，公共资源接口不包含 stb 头文件或类型；Resource 模块不依赖 Vulkan。相关资源测试与既有文本、Shader 测试通过。
- 无需手工复制、无需 Task 05，即可通过构建得到与 Shader 同根可读取的最小测试纹理；仅更新源样本后的构建也能更新目标样本。

## Task 01 已冻结的实施输入

- 固定 stb 提交 `013ac3beddff3dbffafd5177e7972067cd2b5083`（stb_image v2.30），采用 MIT；源码与 LICENSE 指纹及独立核对见 [契约复核记录](./task-01-contract-review.md)。独立静态目标定义 PNG-only、无 stdio、STBI_MAX_DIMENSIONS=4096，Runtime PRIVATE 链接。
- 只支持非隔行 8-bit Gray/RGB/Gray+Alpha/RGBA，RGBA8 输出；Gray 复制 RGB、合法 tRNS 展开，straight alpha，左上原点、top-to-bottom，不启用翻转或 gamma/ICC 转换。输入固定按 sRGB 解释，细则见 architecture.md §5.5.1。
- 输入最多 64 MiB，宽/高 1–4096，RGBA 输出最多 64 MiB；运行时检查输入与输出范围，编译期断言保证输入上限可由 size_t/int 表示后再转换。不靠分配巨量内存测试边界。必要检查是签名和固定 IHDR 范围，不扩展完整 PNG 验证器。
- 采用 §5.5.2 的固定 7/20/72 字节截断、IDAT `00 00` 压缩头损坏及仅 CRC 修改反例。空 decoder reason 使用带 UTF-8 路径的明确兜底描述；CRC 修改及缺 IEND CRC/追加尾字节不要求失败。
- 最小样本为 §5.5.6 的原创 4×3 精确像素图，附 CC0-1.0 来源说明；source→output 依赖及增量复制同时接入 Demo 和 GPU 复核目标，与 SPIR-V 同处 build/assets。
- 完成条件增加对灰度复制、无 alpha→255、tRNS、透明 RGB 保留和元数据不改变字节的断言；不支持的真实 PNG 变体由头范围拒绝，不能仅证明 stb 已解码。

## 规则与边界

遵循总体架构第 4 节：不要过度设计和防御性编程。继续使用现有 ResourceManager 和 Registry，不增加独立 Cache、纹理插件体系、异步状态机或 GPU 句柄。

下一任务：[Task 04](./task-04-renderer-texture.md)。

## 完成情况（2026-10-14）

- 已实现 TextureResource、TextureLoader、固定版本 stb_image 私有静态库接入，以及原创样本和 CMake 增量部署。测试保留 12 个必要格式样本，其余字节变体在内存中生成，RGBA 基准复用 Demo 资产。
- 工作流 Debug 构建成功；244 项相关回归全部通过，其中纹理测试 52 项，覆盖中文路径、通道转换、短读、失败重试及引用生命周期。
- 仅修改源图片即可更新构建副本，无需重新链接。仅 CRC 修改及约定的尾部变动可成功且像素不变，不表示完整性校验通过。GPU 消费与 Validation 由后续任务完成。
