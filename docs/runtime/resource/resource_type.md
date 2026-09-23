# ResourceType 资源类型模块设计

## 目标

定义资源的**封闭类型集**。`ResourceType` 同时充当类型 tag（区分不同资源种）与 Loader 路由键（Manager 按类型分发加载器）。

## 定义

```cpp
enum class ResourceType : Utils::Hash
{
    TEXT = Utils::constHash("TEXT"),  // 文本资源（配置/JSON/着色器源码）
    TEXTURE = Utils::constHash("TEXTURE"),
    MODEL = Utils::constHash("MODEL"),
    MATERIAL = Utils::constHash("MATERIAL"),
    SHADER = Utils::constHash("SHADER"),
    INVALID = 0
};
```

- 底层为 `Utils::Hash`（uint64），显式值 = 字符串 `constHash`，跨会话稳定
- `INVALID = 0` 为无效类型哨兵，任何入口（registerResource / fromPath）遇之拒绝
- 枚举值使用 **UPPER_CASE**，与 `Log::Level` 约定一致

## TEXT 新值说明

Task 4 落地首个真实 Loader 时引入 `TEXT`。文本资源是配置/JSON/着色器源码的通用载荷，
不引入解码库，作为第一个 Loader 路由键（TextLoader）。

## 扩展注意

1. **封闭枚举**：改动需**重新编译全部依赖方**；预期演进方式是每次增量新增一个显式 `constHash` 值
2. **值即哈希**：新增枚举值必须使用 `Utils::constHash("...")` 而非手写序数，否则破坏跨会话 ID 稳定
3. 枚举值参与 ResourceId 计算（`combine(域种子, 类型tag, hash(规范化键))`），改动某类型值会改变该类型全部已有 ID——应在无存量数据时进行