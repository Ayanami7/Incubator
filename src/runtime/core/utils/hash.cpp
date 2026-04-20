// hash.cpp
#include "runtime/core/utils/hash.h"

namespace Incubator::Utils
{

    uint32_t hash32(std::string_view str)
    {
        return hash32(str.data(), str.size());
    }

    void hashCombine(size_t& seed, const char* value)
    {
        seed ^= hash64(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    void hashCombine(size_t& seed, std::string_view value)
    {
        seed ^= hash64(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    uint32_t hash32(const void* data, size_t size)
    {
        static constexpr uint32_t FNV_OFFSET_BASIS = 2166136261u;
        static constexpr uint32_t FNV_PRIME = 16777619u;

        uint32_t hash = FNV_OFFSET_BASIS;
        const unsigned char* bytes = static_cast<const unsigned char*>(data);

        for (size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint32_t>(bytes[i]);
            hash *= FNV_PRIME;
        }

        return hash;
    }

    uint64_t hash64(std::string_view str)
    {
        return hash64(str.data(), str.size());
    }

    uint64_t hash64(const void* data, size_t size)
    {
        static constexpr uint64_t FNV_OFFSET_BASIS = 14695981039346656037ULL;
        static constexpr uint64_t FNV_PRIME = 1099511628211ULL;

        uint64_t hash = FNV_OFFSET_BASIS;
        const unsigned char* bytes = static_cast<const unsigned char*>(data);

        for (size_t i = 0; i < size; ++i)
        {
            hash ^= static_cast<uint64_t>(bytes[i]);
            hash *= FNV_PRIME;
        }

        return hash;
    }

}  // namespace Incubator::Utils
