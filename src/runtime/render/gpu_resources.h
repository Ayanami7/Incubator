#pragma once

#include <memory>
#include <utility>

namespace Incubator::Render
{
    /// @brief 一组显式可释放的 GPU 资源持有，只可移动，不延长 Renderer 或设备寿命。
    /// @note reset 或析构撤销本组持有；材质依赖及已提交帧仍保留其使用的对象至使用结束。
    /// 未提交的 DrawItem/SceneFrame 不拥有资源，提取至提交期间必须保留对应资源组。
    class GpuResources
    {
    public:
        GpuResources() = default;
        GpuResources(const GpuResources&) = delete;
        GpuResources& operator=(const GpuResources&) = delete;
        GpuResources(GpuResources&&) noexcept = default;
        GpuResources& operator=(GpuResources&&) noexcept = default;

        /// @brief 创建它的 Renderer 尚未关闭且本组未 reset 时有效。
        bool valid() const noexcept;
        /// @brief 撤销本组持有，不等待 GPU；在途对象由帧槽保护，组随后不可再用于创建。
        void reset() noexcept
        {
            impl_.reset();
        }

    private:
        friend class Renderer;
        struct Impl;
        explicit GpuResources(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
        std::shared_ptr<Impl> impl_;
    };
}  // namespace Incubator::Render
