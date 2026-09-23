# ResourceId 资源 ID 模块设计

## 目标

提供**全局稳定资源标识**：同一 (类型, 虚拟路径) 在任何会话计算结果一致，供注册去重、持久化与对象挂载使用。

## 虚拟路径规范化（canonicalizeResourceKey）

```cpp
std::string canonicalizeResourceKey(std::string_view virtualPath);
```

规范化顺序（`resource_id.cpp:18-47`）：

1. `Path::normalize`（统一分隔符、折叠 `.`/`..`）
2. 空路径 → 返回空串
3. 拒绝任何"名称根"：盘符前缀（含 `C:foo` 这种 drive-relative）与 UNC 前缀 → 返回空串（此处不用 `Path::isAbsolute`，因为 `C:foo` 无根目录时 `is_absolute()==false` 但作为资源键必须拒绝）
4. 剥离单个前导 `/`（根相对路径与相对路径归一）
5. 拒绝规范化后仍以 `..` 开头（越出虚拟资源根）的路径 → 返回空串
6. ASCII 小写折叠

非法输入返回空串，调用方据此判定无效。

## ResourceId

```cpp
class ResourceId
{
public:
    ResourceId() = default;                                        // 无效 ID（value_ = 0）

    static ResourceId fromPath(ResourceType type, std::string_view virtualPath);
    static ResourceId fromPathOnly(std::string_view virtualPath);  // 不带类型 tag

    bool valid() const noexcept;             // value() != 0
    Utils::Hash value() const noexcept;
    explicit operator Utils::Hash() const noexcept;

    std::string toString() const;                                 // 十六进制（无 0x 前缀）
    static ResourceId fromString(std::string_view hex);            // 无法解析/为 0 → 无效 ID
};
```

- **ID 计算**：`combine(combine(域种子, 类型tag), hash(规范化键))`
- **域种子** `constHash("Incubator::Resource")` 隔离资源域，避免与其他哈希域冲突（资源域不使用 `Utils::Detail::kOffset`）
- **值语义**：可拷贝；`operator==` / `operator<` 支持 map/set key
- `fromString` / `toString` 往返：`Utils::from_chars`/`to_chars` 十六进制，字节精确

## 碰撞哲学

基于哈希，存在极小碰撞概率。碰撞由 **Registry 登记边界**检测并拒绝（`injectEntry` 双向校验），
非安全场景；单个资源 ID 冲突只会导致登记被拒，不追溯为数据损坏。