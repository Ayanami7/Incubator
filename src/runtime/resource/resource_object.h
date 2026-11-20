#pragma once

#include "runtime/resource/resource_id.h"
#include "runtime/resource/resource_type.h"

namespace Incubator
{
    namespace Resource
    {
        /// @brief 运行时资源对象的薄基类。
        ///
        /// 该类只保存身份与类型。载荷由派生类保存并由对应 Loader 创建；
        /// 路径唯一真源位于 AssetInfo，加载和卸载职责位于 Loader 与 ResourceManager。
        class ResourceObject
        {
        public:
            virtual ~ResourceObject() = default;

            ResourceId id() const noexcept
            {
                return id_;
            }

            ResourceType type() const noexcept
            {
                return type_;
            }

            ResourceObject(const ResourceObject&) = delete;
            ResourceObject& operator=(const ResourceObject&) = delete;

        protected:
            ResourceObject(ResourceId id, ResourceType type);

        private:
            ResourceId id_;
            ResourceType type_;
        };
    }  // namespace Resource
}  // namespace Incubator
