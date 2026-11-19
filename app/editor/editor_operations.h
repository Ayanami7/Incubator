#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "editor_selection.h"
#include "runtime/model/model_manager.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/scene/scene_rebuild.h"

namespace Incubator::Editor
{
    /// @brief 执行场景编辑和文件操作，不持有场景或实体指针。
    class EditorOperations : public EditorSelection
    {
    public:
        void initialize(Render::Renderer& renderer)
        {
            cache_ = std::make_unique<Model::ModelManager>(renderer);
        }

        bool addModel(Scene::Scene& scene, std::string_view path,
                      const Scene::Transform& transform = Scene::Transform{});

        bool save(FileIO::FileIO& file, const Scene::Scene& scene, const std::array<float, 4>& color,
                  std::string_view path);

        /// @brief 重建成功后返回新场景；失败保留选择及文件关联。
        std::optional<Scene::RebuiltScene> load(FileIO::FileIO& file, std::string_view path, bool rethrow = false);

        size_t cachedModels() const
        {
            return cache_->size();
        }
        size_t collectUnusedModels();
        const std::string& currentFile() const
        {
            return currentFile_;
        }
        const std::string& message() const
        {
            return message_;
        }
        bool messageError() const
        {
            return messageError_;
        }

    private:
        static std::string logPath(std::string_view path);
        static constexpr Model::PrepareOptions PREVIEW_OPTIONS{
            Render::TextureMipMode::FullChain,
            {Render::TextureFilter::Linear, Render::TextureFilter::Linear, Render::MipFilter::Linear},
            Render::ShadingMode::Lambert};
        std::unique_ptr<Model::ModelManager> cache_;
        std::string currentFile_;
        std::string message_;
        bool messageError_ = false;
    };
}  // namespace Incubator::Editor
