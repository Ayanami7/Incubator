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
        using Hash = uint64_t;

        // 内部实现（禁止外部使用）
        namespace Detail
        {
            // 原始字节哈希
            Hash hashBytes(const void* data, size_t size) noexcept;

            // FNV 参数
            constexpr Hash kOffset = 14695981039346656037ull;
            constexpr Hash kPrime = 1099511628211ull;

            // combine 魔数（boost）
            constexpr Hash kMul = 0x9e3779b97f4a7c15ull;
        }

        // 字符串哈希（最常用）
        Hash hash(std::string_view str) noexcept;

        // 快速文件哈希，使用元数据（路径 + size + timestamp）
        Hash hashFileFast(const std::filesystem::path& path) noexcept;

        // 内容文件哈希（完整文件）
        Hash hashFileContent(const std::filesystem::path& path);

        // 编译期哈希
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

        // 基础组合（对外提供）
        template <typename T>
        void combine(Hash& seed, const T& value) noexcept;

        // 基础类型的 combine 特化
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
