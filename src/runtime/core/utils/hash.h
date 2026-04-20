// hash.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>

namespace Incubator::Utils
{
    using Hash = uint64_t;

    /**
     * FNV-1a 32位哈希算法
     * 适用于资源ID等需要快速哈希的场景
     */
    uint32_t hash32(std::string_view str);
    uint32_t hash32(const void* data, size_t size);

    /**
     * FNV-1a 64位哈希算法
     * 提供更大的哈希空间，适合大型数据集
     */
    uint64_t hash64(std::string_view str);
    uint64_t hash64(const void* data, size_t size);

    /**
     * 组合哈希值
     * 用于构建复合键的哈希
     */
    template <typename T>
    void hashCombine(size_t& seed, const T& value)
    {
        seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    /**
     * 组合哈希值（字符串字面量重载）
     */
    void hashCombine(size_t& seed, const char* value);
    void hashCombine(size_t& seed, std::string_view value);

    /**
     * 编译时计算的32位哈希
     * 用于事件标识等编译时已知的字符串
     */
    constexpr uint32_t constHash32(std::string_view str)
    {
        uint32_t hash = 2166136261u;
        for (char c : str)
        {
            hash ^= static_cast<uint32_t>(static_cast<unsigned char>(c));
            hash *= 16777619u;
        }
        return hash;
    }

}  // namespace Incubator::Utils
