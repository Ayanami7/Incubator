# Resource 资源系统

## 目标

为引擎提供**全局稳定资源标识**、**资源注册/去重**与**同步加载**能力。子系统沿
`ResourceId（身份）→ ResourceRegistry（登记）→ ResourceManager（门面）→ Loader（桥）→ ResourceObject（实例）`
分层，覆盖从"资源身份"到"可用的运行时对象"的完整链路。

## 分层图

```
ResourceId ──► ResourceRegistry ──► ResourceManager ──► Loader ──► ResourceObject
  身份             登记                 门面               桥              实例
 稳定ID       去重/碰撞/计数/挂载      Loader路由+加载      磁盘→对象         运行时载荷
```

- **身份**：`(类型, 规范化虚拟路径)` 哈希，跨会话稳定
- **登记**：全局户口本，去重、哈希碰撞检测、引用计数、运行时对象唯一挂载点
- **门面**：Loader 路由、同步加载、生命周期（acquire/release）与异常边界
- **桥**：把磁盘数据解析并构建为 ResourceObject，按 ResourceType 路由
- **实例**：薄基类承载 id/type/state，具体载荷由派生类持有

## 组件索引

| 组件 | 文档 | 状态 |
|---|---|---|
| ResourceType 封闭枚举 | [resource_type.md](./resource_type.md) | ✅ 已实现 |
| ResourceId + 虚拟路径规范化 | [resource_id.md](./resource_id.md) | ✅ 已实现 |
| ResourceRegistry + Entry（X3） | [resource_registry.md](./resource_registry.md) | ✅ 已实现 |
| AssetInfo 描述壳 | [asset_info.md](./asset_info.md) | ✅ 已实现 |
| ResourceObject 薄基类 + ResourceState | [resource_object.md](./resource_object.md) | ✅ 已实现 |
| ResourceManager 门面 | [resource_manager.md](./resource_manager.md) | ✅ 已实现 |
| LoaderBase + 虚拟路径解析约定 | [loader/loader.md](./loader/loader.md) | ✅ 已实现 |
| TextLoader 首个实现 | [loader/text_loader.md](./loader/text_loader.md) | ✅ 已实现 |

## 文件布局（镜像 src）

```
src/runtime/resource/
├── resource_type.h
├── resource_id.h/.cpp
├── resource_state.h
├── asset_info.h
├── resource_object.h/.cpp
├── resource_registry.h/.cpp
├── resource_manager.h/.cpp
└── loader/
    ├── loader_base.h
    ├── text_loader.h/.cpp
    └── text_resource.h/.cpp
```

## 演进路线（未实现项触发条件）

| 项目 | 触发条件 |
|---|---|
| 异步加载（启用 LOADING/ERROR 态） | 跨帧加载需求出现（资源接入渲染管线后） |
| 热重载（contentHash 变更检测） | 需要运行时资源迭代的工作流 |
| VFS（虚拟路径 → 真实文件解析 / 打包） | .pak 打包、多资源根需求 |
| ResourceHandle / LOD / 流式加载 | 一个 Asset 多实例需求时引入 |

## 设计原则

1. **身份稳定**：ID 由 (类型, 规范化虚拟路径) 哈希，不依赖运行时顺序，跨会话可复现
2. **单一真源**：虚拟路径唯一真源在 AssetInfo；refCount 与对象挂载在 Registry::Entry
3. **封闭类型**：ResourceType 为封闭枚举，同时充当类型 tag 与 Loader 路由键
4. **失败走返回值**：预期失败（未注册/无 Loader/文件缺失）返回 `nullptr`；Loader 抛异常由 Manager 门面统一捕获，`load()` 保证不向外抛异常
5. **同步原语**：当前即用即得；LOADING/ERROR 状态为异步预留