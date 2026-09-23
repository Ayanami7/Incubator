# ResourceRegistry 资源注册表模块设计

## 目标

全局**户口本**：资源去重、哈希碰撞检测、引用计数，并作为运行时对象的**唯一挂载点**。
与 Manager 的分工：Registry 是底层存储（只认 id/path/refCount/object），Manager 在其上做 Loader 路由与加载语义。

## Entry（X3 结构）

```cpp
struct Entry
{
    AssetInfo info;                          // 身份信息（id/type/path/contentHash）
    uint32_t refCount = 1;                   // 引用计数（注册即持有 1）
    std::unique_ptr<ResourceObject> object;  // 运行时对象（null = 未加载）
};
```

## 内部结构

- `pathToId_`：`(类型, 规范化路径) → id`，支持按路径去重
- `idToEntry_`：`id → Entry`，支持按 ID 反查与对象挂载

## 接口

```cpp
class ResourceRegistry
{
public:
    static ResourceRegistry& instance();                              // 全局单例

    void clear();                                                     // 清空全部条目（测试隔离/引擎关闭）

    ResourceId registerResource(ResourceType type, std::string_view virtualPath);
    bool injectEntry(ResourceType type, const std::string& key, ResourceId id);  // 低层注入/仅反序列化与构造碰撞用

    void acquire(ResourceId id);                                      // refCount +1（未注册记日志）
    void release(ResourceId id);                                      // refCount -1（不会减到负数）
    uint32_t refCount(ResourceId id) const;                           // 未注册返回 0

    bool lookup(ResourceId id, ResourceType& type, std::string& virtualPath) const;
    ResourceId lookup(ResourceType type, std::string_view virtualPath) const;

    bool attachObject(ResourceId id, std::unique_ptr<ResourceObject> object);
    ResourceObject* getObject(ResourceId id);
    const ResourceObject* getObject(ResourceId id) const;
    void detachObject(ResourceId id);
};
```

## 语义要点

- **registerResource 幂等**：先查 `pathToId_`，命中直接返回既有 ID，不重复登记、不 +1
- **injectEntry 双向校验**：ID 已被不同 (type, key) 占用（碰撞）拒绝；(type, key) 已被不同 ID 占用拒绝；key 必须已规范化
- **attachObject 边界**：未注册/object 为空/条目已挂载/id 或 type 与登记信息不一致 → `false`（身份/类型一致性是写入边界的核心不变量）
- **引用计数从 1 起步**（注册即持有），不会减到负数（0 时再 release 只记日志）
- **refCount == 0 的纯存储态**：Registry 允许计数归零后仍暂持有 object，**不做自动卸载**；卸载由调用方（ResourceManager::release 归零 → detachObject）触发，登记保留
- 错误路径统一经 `Log::Registry::get("resource")` 记录，不抛异常
- 线程安全由调用方保证（与全局单例共享一致）

## 与同步加载的关系

同步加载下 `load()` 失败**不挂载任何对象**（直接返回 nullptr），因此不会产生 ERROR 状态的对象；
条目保持 `object == nullptr` 的未加载态，可随时再次尝试加载（`text_loader_test` 的 `ReloadAfterUnloadRebuildsObject` 覆盖该路径）。