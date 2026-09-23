# ResourceObject 运行时对象模块设计

## 目标

运行时资源对象的**薄基类**。只保存身份、类型与加载状态，**不保存载荷**；载荷由派生类持有，并由对应 Loader 创建。

## 定义

```cpp
class ResourceObject
{
public:
    virtual ~ResourceObject() = default;

    ResourceId id() const noexcept;
    ResourceType type() const noexcept;
    ResourceState state() const noexcept;
    void setState(ResourceState state) noexcept;   // 由 ResourceManager 加载流程调用

    ResourceObject(const ResourceObject&) = delete;
    ResourceObject& operator=(const ResourceObject&) = delete;

protected:
    ResourceObject(ResourceId id, ResourceType type);
};
```

## 设计取舍

- **不存 path**：路径唯一真源在 `AssetInfo`（成就单一真源原则）；对象不需要重复备份
- **不设虚 `load()/unload()`**：加载/卸载职责全部在 Loader 与 ResourceManager（Loader 是唯一桥），避免把 IO 逻辑耦合进对象层
- **禁拷贝**：运行时对象是共享单实体（引用计数共享），不按值复制

## ResourceState

```cpp
enum class ResourceState : uint8_t
{
    UNLOADED = 0,  // 条目已登记但尚未挂载运行时对象
    LOADING,       // 正在加载（为异步加载预留）
    READY,         // 已加载，运行时对象可访问
    ERROR          // 加载失败（为异步加载预留）
};
```

**同步语义**：同步加载下 LOADING 为瞬时状态；同步失败**直接返回 nullptr**，不会为失败结果挂载 ERROR 状态的对象。
LOADING 与 ERROR 为异步加载预留（异步落地前禁止产生这两种状态的对象）。

## 派生约定

具体载荷派生类（如 `TextResource`）由对应 Loader 构造并返回，Manager 挂载时将对象 state 置为 READY。
派生类建议声明 `final`（引擎对具体载荷不做进一步分发）。