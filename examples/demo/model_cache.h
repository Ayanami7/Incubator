#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "runtime/model/prepared_model.h"
#include "runtime/render/renderer.h"

namespace Incubator::Demo
{
    /// @brief 绑定单个 Renderer 会话的模型准备与复用入口。
    class ModelCache final
    {
    public:
        explicit ModelCache(Render::Renderer& renderer) : renderer_(renderer) {}
        ModelCache(const ModelCache&) = delete;
        ModelCache& operator=(const ModelCache&) = delete;

        /// @brief 按规范化来源及完整选项复用或准备模型。
        std::shared_ptr<const Model::PreparedModel> resolve(const Model::ModelReference& reference)
        {
            const auto found =
                std::ranges::find_if(models_, [&](const auto& model) { return model->source() == reference; });
            if (found != models_.end())
                return *found;
            auto model = Model::prepareModel(renderer_, reference.path(), reference.options());
            models_.push_back(model);
            return model;
        }

    private:
        Render::Renderer& renderer_;
        std::vector<std::shared_ptr<const Model::PreparedModel>> models_;
    };
}  // namespace Incubator::Demo
