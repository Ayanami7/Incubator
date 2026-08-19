# UTF-8 编码约定与文本语义方案

## 目标

统一全引擎字符串编码约定，消除文件系统边界的中文乱码与资源 ID 不稳定问题；为将来需要"文本语义"（大小写折叠、规范化、排序、多语言）的场景预留分层方案。

## 核心约定

> **全引擎 `std::string` / `std::string_view` 均为 UTF-8 字节序列。**

- 源文件通过 `/utf-8` 编译选项保证字面量正确
- fmt / spdlog / gtest / glm 生态原生支持 UTF-8，无需转换
- 资源键、路径、日志、模块名等一律按 UTF-8 字节处理

## 边界守则（必须遵守）

### 唯一入口

`std::filesystem::path` 与 UTF-8 字符串互转**只允许**走 `Utils::Utf8`（`src/runtime/core/utils/utf8.h`）：

| 场景 | 正确写法 | 禁止写法 |
|---|---|---|
| `std::string` → `filesystem::path` | `Utils::Utf8::toFsPath(s)` | `std::filesystem::path(s)` |
| `filesystem::path` → `std::string` | `Utils::Utf8::toUtf8String(p)` | `p.string()` |
| 校验外部输入是否合法 UTF-8 | `Utils::Utf8::isValid(s)` | 直接信任字节 |

### 原因（一句话）

Windows 上 `path.string()` 与 `path(const char*)` 走的是系统 ANSI 代码页（中文系统 = GBK），而项目约定是 UTF-8，桥接处必然乱码。`toUtf8String` / `toFsPath` 内部使用 `u8string()` / `char8_t` 构造，保证 UTF-8 ↔ UTF-16 直连。

### 硬性红线

1. 禁止 `path.string()`
2. 禁止 `std::filesystem::path(某std::string)`
3. 禁止将 `std::string::data()` 未经 `Utils::Utf8` 直接传给 Win32 W 系列 / 文件系统 / 序列化 API
4. 新代码不要自行编写 `reinterpret_cast<const char8_t*>` 之类的手工桥，需要时扩展 `utf8.cpp`

## 使用规则

### 整段搬运（安全，无需处理）

拼接、比较、排序、哈希、存储、拷贝、完整子串查找 —— 这些操作对 UTF-8 字节无感知，正常使用。

### 按字符拆（需注意）

`std::string::size()` 是**字节数**，`substr` 按字节切。以下操作可能破坏多字节字符：

- `substr` 切到字符中间 → 产生非法 UTF-8
- 按 `char` 遍历 / 反转
- 把 `size()` 当显示长度 / 截断限制
- 大小写转换（`std::tolower` 只处理 ASCII）

**判断法**：操作是"整段搬"还是"按字符拆"？按字符拆的场景当前无消费方，出现时按下方"文本语义方案"处理。

## 当前依赖

- **utfcpp**（`3rdparty/utfcpp/`，仅头文件）：校验（`is_valid`）与码点迭代
- **`Utils::Utf8`**：项目自有边界封装，基于 utfcpp 与标准库

## 文本语义优化方案（将来，ICU）

### 触发条件

- 运行时出现按码点操作消费方（资源名模糊匹配、文本解析器）
- 文本排版 / 渲染（字体、字形、合字）
- 编辑器 / 多语言（i18n）落地

### 方案：借道 ICU，不换字符串

ICU 提供大量直接处理 UTF-8 的 `*_UTF8` 入口（如 `u_strFoldCase_UTF8`、`unorm2_normalize_UTF8`、`ucol_strcollUTF8`），`std::string` 无需替换：

```
全引擎 std::string = UTF-8（恒定不变）
      ↓ 仅在需要文本语义时
Utils 层 Text 助手（薄包 ICU *_UTF8 入口）
      ↓ 结果仍为 UTF-8
std::string 返回
```

个别复杂 API 需要 `UnicodeString`（UTF-16）时，在函数局部转换、用完即弃，不渗透到系统。

### 分层影响

- **编码边界不变**：`Utils::Utf8` 是唯一桥，无论 ICU 进哪个打包单元都走这里
- **依赖归属**：ICU 体积大（数 MB），只引入渲染 / Tool 层所在打包单元；若在意 runtime 库体积，在渲染落地时考虑将其抽为独立库（如 `IncubatorRender`）
- **不动的**：现有 `std::string` 用法、资源键（哈希 UTF-8 字节）、日志 —— 零改动

### i18n

多语言文案 = 消息目录（key → 文案）+ 复数规则 + fmt 格式化。与字符串类型无关，属上层独立模块，延后（YAGNI）。

## 测试要点

- `utf8_test`：非法 UTF-8（截断 / 超长 / surrogate / 孤立续字节）拒绝、中文路径 `toFsPath` / `toUtf8String` 往返
- 涉及中文路径的新功能，顺手补中文往返用例
- 自查：`rg "\.string\(\)" src/` 应为空

## 文件结构

```
src/runtime/core/utils/utf8.h/.cpp   # Utils::Utf8 边界助手
test/runtime/utf8_test.cpp           # 边界与校验测试
3rdparty/utfcpp/                     # 仅头文件 UTF-8 库
```
