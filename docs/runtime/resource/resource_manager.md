# ResourceManager 门面模块设计

## 目标

资源层的**门面（Facade）**：Loader 路由 + 同步加载 + 生命周期驱动。存储层委托 `ResourceRegistry`，Loader 路由表在本类持有。全局单例，线程安全由调用方保证（与 Registry 约定一致）。

## 接口

```cpp
class ResourceManager
{
public:
    static ResourceManager& instance();
    void bindAssetSource(AssetSource* source);    // 空指针解除绑定
    AssetSource* assetSource() const noexcept;   // 当前借用源

    void registerLoader(ResourceType type, std::unique_ptr<LoaderBase> loader);
    bool hasLoader(ResourceType type) const;
    bool loadersEmpty() const;                    // 整个路由表为空，不枚举特定类型
    void reset();                                 // 清除全部 Loader（测试隔离/引擎关闭）

    ResourceId registerResource(ResourceType type, std::string_view virtualPath);  // 委托 Registry（幂等）

    ResourceObject* load(ResourceId id);          // 同步加载
    template <typename T> T* get(ResourceId id);  // load + dynamic_cast；类型不符返回 nullptr

    std::vector<AssetEntry> listDirectory(std::string_view directory = {}) const;
    std::optional<ResourceInfo> query(ResourceType type, std::string_view path) const;
    template <class T = ResourceObject> CpuResource hold(ResourceType type, std::string_view path);

    void acquire(ResourceId id);                  // 额外持有者 +1
    void release(ResourceId id);                  // 归零时卸载对象（登记保留）
};
```

## 引用计数所有权模型

普通消费者优先使用 `hold<T>`，无需直接查询 Registry 或手工配对引用。

- `registerResource` 首次登记即持有 refCount = 1；重复登记**幂等，不再 +1**
- 额外的持有者必须显式调用 `acquire()`；`load()`/`get()` **不改变引用计数**
- `release()` 递减计数，归零时由本类卸载对象（`detachObject`），登记保留
- `load()`/`get()` 返回的裸指针**仅在对应持有期（登记持有 + acquire）内有效**，持有期满后不得再访问
- 已有 refCount=0 的登记条目重新使用前需 `acquire()`；重复登记不会恢复持有。
- `reset()` 只移除 Loader，不释放 CPU 对象、不清空 Registry，也不使已挂载缓存失效。
  调用方须在正常或异常结束时先 release 本次持有，再移除借用 AssetSource 的 Loader、解除源绑定，最后销毁 AssetSource 和 FileIO；
  具体资源的 GPU 使用期由 Render 另行管理。

[EditorApp](../../app/editor.md) 在首次注册前通过 loadersEmpty() 确认整个路由表为空，
同时确认没有既有资产源绑定，再绑定会话源并注册 Shader/Texture/Model Loader；已有任意路由或绑定即拒绝接管并保留原状态。
EditorApp 运行期间路由及 FileIO 保持有效，CPU release、Scene clear/switch 不调用 reset，关闭时才清理已接管路由。

## 资产源绑定

bindAssetSource 借用单个 AssetSource，同一指针重复绑定无操作。更换或解除绑定前须先清空 Loader 路由，
并释放 Registry 中全部 CPU 持有及挂载对象，否则抛 CONFIG，原绑定与载荷不变。
仅有零持有且无载荷的身份登记时可以解绑或换绑；新根中同路径沿用原 ResourceId，但重新加载载荷。
源自身借用 FileIO，二者须存活至解除绑定。reset 仍只清除路由，不能代替 release 或解除源绑定。

## 目录发现与状态查询

`listDirectory()` 或 `listDirectory(".")` 列根，传子目录虚拟路径列直接子项；返回 `AssetEntry{name, path, kind}`。
name 保留实际 UTF-8 名称，path 为规范化的小写 ASCII 虚拟路径，结果按 path 排序。每次调用都读取当前目录，
因此手动刷新可以看到新增或删除的文件；空目录返回空列表，未绑定源抛 CONFIG，访问错误抛 IO。
词法越根被拒绝，目录链接和目录重解析点不列出也不可进入；根仍为受信任目录，不承诺文件链接的物理隔离。

`AssetKind` 包括 Directory、Model（OBJ/glTF）、Texture（PNG）、Dependency（bin/mtl）及 File。
扩展名分类不解析内容、不代表可以成功导入；依赖及普通文件不能直接作为场景模型。

`query(type, path)` 仅返回已有登记的 `ResourceInfo{id, type, path, references, loaded}`，不存在返回空 optional。
loaded 表示当前挂载 CPU 对象，与文件存在或 GPU 准备无关。列表和查询均不登记、不取得引用、不调用 Loader，
也不准备模型或上传 GPU。

## CPU 持有与失败结果

`CpuResource` 禁止复制，支持 noexcept 移动；移动转移一次持有，源不再持有，移动赋值先释放目标原有持有。
首次登记的初始引用属于这次 hold；已有登记（包括零引用条目）额外 acquire。reset 或析构 release，
最后引用归零时卸载 CPU 对象，登记保留。裸指针仅在持有期间借用；`get<T>()` 不加载，类型不符返回空指针。
必须在会话解绑及 FileIO 销毁前释放持有，不能用 Manager 的 reset 代替。

```cpp
auto model = manager.hold<ModelResource>(ResourceType::MODEL, "models/example/model.gltf");
if (model)
{
    const auto* data = model.get<ModelResource>();  // 仅在 model 持有期间借用
    // 使用 CPU 数据完成同步准备。
}
else
{
    const ResourceFailure& error = *model.failure();  // 自有虚拟路径及具体原因，可供界面显示
}
```

加载、缺失依赖、解析或类型错误返回空持有，失败不遗留本次引用。`failure()` 返回本次调用拥有的
`optional<ResourceFailure>{path, reason}`，后续调用不覆盖；reset 保留失败信息。
错误由原负责边界记录一次，展示或补充模型上下文不重复记错。旧 load/get 仍返回裸指针或 nullptr，
Loader 异常仍在 Manager 转成一次错误日志；正常 Exists 探测不经过该失败入口。

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
