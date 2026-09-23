#pragma once

#include <string>

#include "runtime/resource/resource_object.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 文本资源对象（载荷 = 文本内容）
        class TextResource final : public ResourceObject
        {
        public:
            TextResource(ResourceId id, std::string text);

            /// @brief 文本内容
            const std::string& text() const noexcept
            {
                return text_;
            }

        private:
            std::string text_;
        };
    }  // namespace Resource
}  // namespace Incubator