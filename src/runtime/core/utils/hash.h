#pragma once

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <type_traits>

namespace Incubator
{
    namespace Utils
    {
        /// @brief 64 位哈希值类型
        using Hash = uint64_t;

        /// @brief 内部实现（禁止外部使用）
        namespace Detail
        {
            /// @brief 对原始字节数组进行 FNV-1a 哈希
            /// @param data 数据指针
            /// @param size 数据大小
            /// @return 64 位哈希值
            Hash hashBytes(const void* data, size_t size) noexcept;

            /// @brief FNV-1a 偏移基数
            constexpr Hash kOffset = 14695981039346656037ull;

            /// @brief FNV-1a 质数乘数
            constexpr Hash kPrime = 1099511628211ull;

            /// @brief boost::hash_combine 魔数
            constexpr Hash kMul = 0x9e3779b97f4a7c15ull;
        }

        /// @brief 字符串哈希（FNV-1a）
        /// @param str 输入字符串
        /// @return 64 位哈希值
        Hash hash(std::string_view str) noexcept;

        /// @brief 快速文件哈希（基于文件元数据，不读内容）
        /// @param path 文件路径
        /// @return 哈希值（文件不存在返回 0）
        /// @note 仅检查路径 + 大小 + 时间戳，适合热重载检测
        Hash hashFileFast(const std::filesystem::path& path) noexcept;

        /// @brief 完整文件内容哈希
        /// @param path 文件路径
        /// @return 基于全部文件内容的哈希值
        /// @throw std::runtime_error 文件无法打开时抛出
        Hash hashFileContent(const std::filesystem::path& path);

        /// @brief 编译期字符串哈希
        /// @tparam N 字符串长度（含 null 终止符）
        /// @param str 字符串字面量
        /// @return 编译期确定的 64 位哈希值
        /// @note 仅支持字符串字面量，如 constHash("hello")
        template <size_t N>
        constexpr Hash constHash(const char (&str)[N]) noexcept
        {
            Hash result = Detail::kOffset;
            for (size_t i = 0; i < N - 1; ++i)
            {
                result ^= static_cast<Hash>(str[i]);
                result *= Detail::kPrime;
            }
            return result;
        }

        /// @brief 组合哈希值（将 value 混入 seed）
        /// @tparam T 值类型
        /// @param seed 待组合的哈希种子
        /// @param value 要加入哈希的值
        template <typename T>
        void combine(Hash& seed, const T& value) noexcept;

        /// @cond
        // 基础类型的 combine 特化（以下为内置类型的特化实现）
        /// @endcond

        template <>
        inline void combine<int32_t>(Hash& seed, const int32_t& value) noexcept
        {
            seed ^= static_cast<Hash>(value) + Detail::kMul + (seed << 6) + (seed >> 2);
        }

        template <>
        inline void combine<uint32_t>(Hash& seed, const uint32_t& value) noexcept
        {
            seed ^= static_cast<Hash>(value) + Detail::kMul + (seed << 6) + (seed >> 2);
        }

        template <>
        inline void combine<int64_t>(Hash& seed, const int64_t& value) noexcept
        {
            seed ^= static_cast<Hash>(value) + Detail::kMul + (seed << 6) + (seed >> 2);
        }

        template <>
        inline void combine<uint64_t>(Hash& seed, const uint64_t& value) noexcept
        {
            seed ^= static_cast<Hash>(value) + Detail::kMul + (seed << 6) + (seed >> 2);
        }

        template <>
        inline void combine<float>(Hash& seed, const float& value) noexcept
        {
            uint32_t intValue;
            std::memcpy(&intValue, &value, sizeof(float));
            combine(seed, intValue);
        }

        template <>
        inline void combine<double>(Hash& seed, const double& value) noexcept
        {
            uint64_t intValue;
            std::memcpy(&intValue, &value, sizeof(double));
            combine(seed, intValue);
        }

        template <>
        inline void combine<bool>(Hash& seed, const bool& value) noexcept
        {
            seed ^= static_cast<Hash>(value) + Detail::kMul + (seed << 6) + (seed >> 2);
        }

        template <>
        inline void combine<std::string_view>(Hash& seed, const std::string_view& value) noexcept
        {
            Hash strHash = hash(value);
            combine(seed, strHash);
        }

        template <>
        inline void combine<std::string>(Hash& seed, const std::string& value) noexcept
        {
            Hash strHash = hash(std::string_view(value));
            combine(seed, strHash);
        }

        template <typename T>
        void combine(Hash& seed, const T& value) noexcept
        {
            if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, float> && !std::is_same_v<T, double>)
            {
                combine(seed, static_cast<Hash>(value));
            }
            else if constexpr (std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>)
            {
                Hash valueHash = Detail::hashBytes(&value, sizeof(T));
                combine(seed, valueHash);
            }
            else
            {
                static_assert(std::is_arithmetic_v<T> || std::is_same_v<T, std::string_view> ||
                              std::is_same_v<T, std::string>,
                              "combine not specialized for this type");
            }
        }
    }
}
