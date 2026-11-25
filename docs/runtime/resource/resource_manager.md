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

    std::vector<AssetEntry> listDirectory(std::string_view directory = {}) const;
    std::optional<ResourceInfo> query(ResourceType type, std::string_view path) const;
    template <class T = ResourceObject> CpuResource hold(ResourceType type, std::string_view path);
};
```

## 引用计数所有权模型

所有消费者统一通过只可移动的 `CpuResource`（`hold<T>(type, path)`）管理单次 CPU 资源持有，不再对外暴露裸指针 `load`/`get` 或手动 `registerResource`/`acquire`/`release`：

- 首次 `hold` 自动完成登记与加载，并取得一次引用计数；已有挂载对象时复用缓存并递增引用计数
- `CpuResource` 析构或调用 `reset()` 时由本类内部递减引用计数，归零时卸载对象（`detachObject`），登记保留
- 通过 `CpuResource::get<T>()` 借用的裸指针**仅在对应持有存活期内有效**，持有释放后不得再访问
- `reset()` 只移除 Loader，不释放 CPU 对象、不清空 Registry，也不使已挂载缓存失效。
  调用方须在正常或异常结束时先释放全部 `CpuResource` 持有，再移除借用 AssetSource 的 Loader、解除源绑定，最后销毁 AssetSource 和 FileIO；
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

`AssetKind` 包括 Directory、Model（OBJ/glTF/GLB）、Texture（PNG/JPEG）、Dependency（bin/mtl）及 File。
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
错误由原负责边界记录一次，展示或补充模型上下文不重复记错；正常 Exists 探测不经过该失败入口。

## 同步加载语义（hold）

1. 已挂载对象 → 直接复用并返回 `CpuResource`（Loader 不会二次调用）
2. 无对应类型 Loader → 记录一次 error 日志，返回带 `failure()` 的空 `CpuResource`
3. 调 Loader 构建；成功 → `attachObject`，返回持有；Loader 返回 `nullptr` → 释放本次引用并返回失败结果
4. **异常边界**：Loader 抛出的任何异常（`catch (const std::exception&)` 后 `catch (...)` 兜底）一律视为加载失败 → 记录 error 日志并返回带 `failure()` 的空 `CpuResource`。因此 `hold<T>()` **保证不向外抛异常**（"预期失败走返回值"由门面唯一实施）

## 防护与加固

- `registerLoader` 拒绝空 loader（`if (!loader) return;`），杜绝后续加载解引用空指针
- 同类型重复注册**覆盖**（键唯一，后注册替代先注册）
- Loader 与对象加载失败均不留任何挂载残留（`getObject == nullptr`）与多余引用计数
