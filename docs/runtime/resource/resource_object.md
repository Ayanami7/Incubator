# ResourceObject 运行时对象模块设计

## 目标

运行时资源对象的**薄基类**。只保存身份与类型，**不保存载荷**；载荷由派生类持有，并由对应 Loader 创建。

## 定义

```cpp
class ResourceObject
{
public:
    virtual ~ResourceObject() = default;

    ResourceId id() const noexcept;
    ResourceType type() const noexcept;

    ResourceObject(const ResourceObject&) = delete;
    ResourceObject& operator=(const ResourceObject&) = delete;

protected:
    ResourceObject(ResourceId id, ResourceType type);
};
```

## 设计取舍

- **不存 path**：路径唯一真源在 `AssetInfo`（成就单一真源原则）；对象不需要重复备份
- **不存预留异步状态**：同步加载成功即挂载可用，失败不挂载半成品对象，无需 `ResourceState` 状态机字段
- **不设虚 `load()/unload()`**：加载/卸载职责全部在 Loader 与 ResourceManager（Loader 是唯一桥），避免把 IO 逻辑耦合进对象层
- **禁拷贝**：运行时对象是共享单实体（引用计数共享），不按值复制

## 派生约定

具体载荷派生类（如 `ShaderResource`、`TextureResource`、`ModelResource`）由对应 Loader 构造并返回，Manager 校验后直接挂载。
派生类建议声明 `final`（引擎对具体载荷不做进一步分发）。