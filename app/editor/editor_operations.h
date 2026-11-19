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
    /// @brief 组合选择状态并执行场景编辑与文件操作，维护文档脏状态，不持有场景或实体指针。
    class EditorOperations
    {
    public:
        void initialize(Render::Renderer& renderer)
        {
            cache_ = std::make_unique<Model::ModelManager>(renderer);
        }

        Scene::EntityId selected() const noexcept
        {
            return selection_.selected();
        }
        void select(Scene::EntityId id) noexcept
        {
            selection_.select(id);
        }

        bool addModel(Scene::Scene& scene, const std::array<float, 4>& clearColor, std::string_view path,
                      const Scene::Transform& transform = Scene::Transform{});
        bool rename(Scene::Scene& scene, const std::array<float, 4>& clearColor, std::string name);
        bool setVisible(Scene::Scene& scene, const std::array<float, 4>& clearColor, bool visible);
        bool setTransform(Scene::Scene& scene, const std::array<float, 4>& clearColor, Scene::Transform transform);
        Scene::EntityId duplicate(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        bool remove(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        bool clear(Scene::Scene& scene, const std::array<float, 4>& clearColor);
        bool setLighting(Scene::Scene& scene, const std::array<float, 4>& clearColor, Render::Lighting lighting);
        bool setClearColor(const Scene::Scene& scene, std::array<float, 4>& clearColor,
                           const std::array<float, 4>& newColor);

        bool save(FileIO::FileIO& file, const Scene::Scene& scene, const std::array<float, 4>& color,
                  std::string_view path);

        /// @brief 重建成功后返回新场景并清除脏状态；失败保留原脏状态、选择及文件关联。
        std::optional<Scene::RebuiltScene> load(FileIO::FileIO& file, std::string_view path, bool rethrow = false);

        bool isDirty() const noexcept
        {
            return dirty_;
        }
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
        EditorSelection selection_;
        std::unique_ptr<Model::ModelManager> cache_;
        std::string currentFile_;
        std::string message_;
        bool messageError_ = false;
        bool dirty_ = false;
    };
}  // namespace Incubator::Editor
