# AssetInfo 资源描述壳模块设计

## 目标

描述磁盘侧资源的**身份与来源**，作为资源注册登记的法定信息载体。该结构**不执行磁盘 I/O，也不持有运行时加载状态**。

## 定义

```cpp
struct AssetInfo
{
    ResourceId id;                              ///< 资源唯一身份（自包含冗余）
    ResourceType type = ResourceType::INVALID;  ///< 资源类型标签
    std::string virtualPath;                    ///< 规范化后的虚拟路径
    Utils::Hash contentHash = 0;                ///< 内容指纹；0 = 当前未计算

    bool valid() const noexcept;                // 仅校验 id.valid()
};
```

## 字段取舍

- **`id` 冗余自包含**：id 可由 (type, virtualPath) 重算，但 Entry 携带完整身份，查询/挂载无需反推
- **`virtualPath`**：是**规范化后的虚拟路径**，而非机器相关的绝对物理路径（真实路径解析是 Loader/VFS 的职责）
- **`contentHash` 预留**：内容指纹（复用 `hashFileFast` 的热重载检测基础），当前**不参与 ID 计算**，恒为 0

## 有效性约定

`valid()` 有意保持宽松——只检查 `id.valid()`，**不**验证 type、virtualPath、contentHash 及它们与 id 的一致性。
完整的完整性不开放在 Registry 登记边界维护（`resource_registry.md`）。

## 与 Registry 的整合（X3）

`ResourceRegistry::Entry` 现为 X3 结构：`AssetInfo + refCount + unique_ptr<ResourceObject>`。
AssetInfo 是 Entry 的身份信息部分；唯一真源原则——虚拟路径、类型、ID 均以 Entry 内的 AssetInfo 为准。