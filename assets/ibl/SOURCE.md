# 固定棚拍 IBL 环境资产说明

## 资产来源与许可

- **原始环境**：[Poly Haven — Studio Small 09](https://polyhaven.com/a/studio_small_09)（`studio_small_09_1k.hdr`，1024×512 线性等距柱状 HDR）
- **许可证**：`CC0 1.0 Universal`（Public Domain，可自由商用与修改）

## 离线预处理产物与方法

构建期不依赖外网或外部工具，以下三份 `KTX1`（`GL_RGBA16F` / `VK_FORMAT_R16G16B16A16_SFLOAT`，小端序、`glType = 0x140B`、`glFormat = 0x1908`、`glInternalFormat = 0x881A`）产物已离线预生成并随仓库部署至 `<build>/assets/ibl/`：

| 文件 | 维度 | 尺寸 / Mip | 预处理方式 |
|---|---|---|---|
| `studio_irradiance.ktx` | Cubemap（6 面） | 32×32，1 级 mip | 余弦加权半球重要性采样（2048 samples/texel），预积分 $\frac{1}{\pi}\int_\Omega L_i(\omega_i)(n\cdot\omega_i)\,d\omega_i$ |
| `studio_prefiltered.ktx` | Cubemap（6 面） | 128×128，8 级 mip（`128..1`） | 参考 Khronos glTF-IBL-Sampler 与 Karis 2013 Split-Sum，按 `roughness = mip / 7` 执行 GGX 重要性采样（1024 samples/texel，并按采样立体角读取源 HDR mip 链抑制火点） |
| `studio_brdf_lut.ktx` | 2D | 128×128，1 级 mip | Karis 2013 Split-Sum BRDF 查找表（Schlick-GGX $k = r^2 / 2$，1024 samples/texel）；R 通道存 `scale`，G 通道存 `bias`，B=0、A=1；按 Vulkan 顶行原点存储：`u = NdotV ∈ [0, 1]`、`v = roughness ∈ [0, 1]`（顶行 `y=0` 对应 `roughness ≈ 0`） |

## 立方体朝向约定

立方体贴图严格按 Vulkan 1.3 Cube Map 规范的 6 个数组层顺序存储，每个面内 `s = 2(x + 0.5)/W - 1 ∈ [-1, 1]`（自左向右）、`t = 2(y + 0.5)/H - 1 ∈ [-1, 1]`（自顶向下）：

- Layer 0 (`+X`): `normalize(+1, -t, -s)`
- Layer 1 (`-X`): `normalize(-1, -t, +s)`
- Layer 2 (`+Y`): `normalize(+s, +1, +t)`
- Layer 3 (`-Y`): `normalize(+s, -1, -t)`
- Layer 4 (`+Z`): `normalize(+s, -t, +1)`
- Layer 5 (`-Z`): `normalize(-s, -t, -1)`
