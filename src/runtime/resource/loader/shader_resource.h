#pragma once

#include <cstdint>
#include <vector>

#include "runtime/resource/resource_object.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 由 CPU 持有的 SPIR-V 代码数据。
        class ShaderResource final : public ResourceObject
        {
        public:
            ShaderResource(ResourceId id, std::vector<uint32_t> words);

            const std::vector<uint32_t>& words() const noexcept
            {
                return words_;
            }

        private:
            std::vector<uint32_t> words_;
        };
    }  // namespace Resource
}  // namespace Incubator
