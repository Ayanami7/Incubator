# ResourceManager 门面模块设计

## 目标

资源层的**门面（Facade）**：Loader 路由 + 同步加载 + 生命周期驱动。存储层委托 `ResourceRegistry`，Loader 路由表在本类持有。全局单例，线程安全由调用方保证（与 Registry 约定一致）。

## 接口

```cpp
class ResourceManager
{
public:
    static ResourceManager& instance();

    void registerLoader(ResourceType type, std::unique_ptr<LoaderBase> loader);
    bool hasLoader(ResourceType type) const;
    bool loadersEmpty() const;                    // 整个路由表为空，不枚举特定类型
    void reset();                                 // 清除全部 Loader（测试隔离/引擎关闭）

    ResourceId registerResource(ResourceType type, std::string_view virtualPath);  // 委托 Registry（幂等）

    ResourceObject* load(ResourceId id);          // 同步加载
    template <typename T> T* get(ResourceId id);  // load + dynamic_cast；类型不符返回 nullptr

    void acquire(ResourceId id);                  // 额外持有者 +1
    void release(ResourceId id);                  // 归零时卸载对象（登记保留）
};
```

## 引用计数所有权模型

- `registerResource` 首次登记即持有 refCount = 1；重复登记**幂等，不再 +1**
- 额外的持有者必须显式调用 `acquire()`；`load()`/`get()` **不改变引用计数**
- `release()` 递减计数，归零时由本类卸载对象（`detachObject`），登记保留
- `load()`/`get()` 返回的裸指针**仅在对应持有期（登记持有 + acquire）内有效**，持有期满后不得再访问
- 已有 refCount=0 的登记条目重新使用前需 `acquire()`；重复登记不会恢复持有。
- `reset()` 只移除 Loader，不释放 CPU 对象、不清空 Registry，也不使已挂载缓存失效。
  调用方须在正常或异常结束时先 release 本次持有，再移除借用 FileIO 的 Loader，最后销毁 FileIO；
  具体资源的 GPU 使用期由 Render 另行管理。

[DemoApp 示例](../../examples/demo.md) 在首次注册前通过 loadersEmpty() 确认整个路由表为空，
取得独占权后注册 Shader/Texture/Model Loader；已有任意路由即拒绝接管并保留原路由。
DemoApp 运行期间路由及 FileIO 保持有效，CPU release、Scene clear/switch 不调用 reset，关闭时才清理已接管路由。

## 同步加载语义（load）

1. 已挂载对象 → 直接返回（对象缓存复用，Loader 不会二次调用）
2. 未注册 → error 日志 + `nullptr`
3. 无对应类型 Loader → error 日志 + `nullptr`
4. 调 Loader 构建；成功 → `setState(READY)` + `attachObject`，返回对象；Loader 返回 `nullptr` → `nullptr`
5. **异常边界**：Loader 抛出的任何异常（`catch (const std::exception&)` 后 `catch (...)` 兜底）一律视为加载失败 → error 日志 + `nullptr`。因此 `load()` **保证不向外抛异常**（"预期失败走返回值"由门面唯一实施）

## 防护与加固

- `registerLoader` 拒绝空 loader（`if (!loader) return;`），杜绝后续 `load()` 解引用空指针
- 同类型重复注册**覆盖**（键唯一，后注册替代先注册）
- Loader 与对象 on failure 均不留任何挂载残留（`getObject == nullptr`）

## 与异步/未来状态的关系

同步路径产物恒为 `READY`；LOADING/ERROR 为异步预留。异步落地时需复核 `load()` 快路径对非 READY
对象的处理（当前不变式：只有 load 挂载，且挂载即 READY）。
