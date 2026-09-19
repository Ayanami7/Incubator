#include "runtime/resource/resource_id.h"

#include <charconv>
#include <system_error>

#include "runtime/core/path/path.h"

namespace Incubator
{
    namespace Resource
    {
        namespace
        {
            /// @brief 资源域隔离种子（不使用 Utils::Detail::kOffset）
            constexpr Utils::Hash kResourceDomainSalt = Utils::constHash("Incubator::Resource");
        }  // namespace

        std::string canonicalizeResourceKey(std::string_view virtualPath)
        {
            std::string key = Incubator::Path::normalize(virtualPath);
            if (key.empty())
            {
                return {};
            }

            // 拒绝任何"名称根"：盘符前缀（含 drive-relative 的 C:foo）或 UNC 前缀。
            // 这里不用 Path::isAbsolute：C:foo 仅含盘符根名、无根目录，is_absolute()==false，
            // 但作为虚拟资源键仍必须拒绝。
            if (Path::hasRootName(key))
            {
                return {};
            }

            // 剥离单个前导 '/'（根相对路径与相对路径归一；UNC 已在上面排除）
            if (key[0] == '/')
            {
                key.erase(0, 1);
            }

            // A normalized path that still starts with ".." escapes the virtual resource root.
            if (key == ".." || key.starts_with("../"))
            {
                return {};
            }

            return Incubator::Path::toLowerASCII(key);
        }

        ResourceId::ResourceId(Utils::Hash value) noexcept : value_(value) {}

        ResourceId ResourceId::computeId(ResourceType type, const std::string& key)
        {
            Utils::Hash seed = kResourceDomainSalt;
            Utils::combine(seed, static_cast<Utils::Hash>(type));
            Utils::combine(seed, Utils::hash(key));
            return ResourceId(seed);
        }

        ResourceId ResourceId::fromPath(ResourceType type, std::string_view virtualPath)
        {
            if (type == ResourceType::INVALID)
            {
                return {};
            }
            std::string key = canonicalizeResourceKey(virtualPath);
            if (key.empty())
            {
                return {};
            }
            return computeId(type, key);
        }

        ResourceId ResourceId::fromPathOnly(std::string_view virtualPath)
        {
            std::string key = canonicalizeResourceKey(virtualPath);
            if (key.empty())
            {
                return {};
            }
            return computeId(ResourceType::INVALID, key);
        }

        bool ResourceId::valid() const noexcept
        {
            return value_ != 0;
        }

        Utils::Hash ResourceId::value() const noexcept
        {
            return value_;
        }

        ResourceId::operator Utils::Hash() const noexcept
        {
            return value_;
        }

        std::string ResourceId::toString() const
        {
            char buffer[17];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer) - 1, value_, 16);
            return std::string(buffer, result.ptr);
        }

        ResourceId ResourceId::fromString(std::string_view hex)
        {
            Utils::Hash value = 0;
            const auto result = std::from_chars(hex.data(), hex.data() + hex.size(), value, 16);
            if (result.ec != std::errc{} || result.ptr != hex.data() + hex.size())
            {
                return {};
            }
            return ResourceId(value);
        }

        bool operator==(ResourceId lhs, ResourceId rhs) noexcept
        {
            return lhs.value() == rhs.value();
        }

        bool operator<(ResourceId lhs, ResourceId rhs) noexcept
        {
            return lhs.value() < rhs.value();
        }
    }  // namespace Resource
}  // namespace Incubator
