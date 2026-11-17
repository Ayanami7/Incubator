#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "editor_selection.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/model_manager.h"
#include "runtime/scene/scene_file.h"
#include "runtime/scene/scene_rebuild.h"

namespace Incubator::Editor
{
    /// @brief 执行场景编辑和文件操作，不持有场景或实体指针。
    class EditorOperations : public EditorSelection
    {
    public:
        void initialize(Render::Renderer& renderer) { cache_ = std::make_unique<Model::ModelManager>(renderer); }

        bool addModel(Scene::Scene& scene, std::string_view path,
                      const Scene::Transform& transform = Scene::Transform{})
        {
            try
            {
                auto model = cache_->resolve(Model::ModelReference(path, PREVIEW_OPTIONS));
                auto name = Utils::Utf8::toUtf8String(Utils::Utf8::toFsPath(path).stem());
                select(scene.createEntity(std::move(model), transform, true, std::move(name)));
                messageError_ = false;
                message_ = "Added model: " + std::string(path);
                Log::Registry::get("editor").info("{}", message_);
                return true;
            }
            catch (const Error::Exception& error)
            {
                messageError_ = true;
                message_ = "Cannot add '" + std::string(path) + "': " + error.message();
                if (!dynamic_cast<const Model::LoadError*>(&error))
                    Log::Registry::get("editor").error("{}", message_);
                return false;
            }
        }

        bool save(FileIO::FileIO& file, const Scene::Scene& scene, const std::array<float, 4>& color,
                  std::string_view path)
        {
            try
            {
                std::string target(path);
                Scene::writeScene(file, path, Scene::captureScene(scene, color));
                messageError_ = false;
                message_ = "Saved: " + target;
                currentFile_ = std::move(target);
                Log::Registry::get("editor").info("Saved scene '{}' ({} entities)", logPath(currentFile_),
                                                  scene.entities().size());
                return true;
            }
            catch (const Error::Exception& error)
            {
                messageError_ = true;
                message_ = error.message();
                Log::Registry::get("editor").error("Failed to save scene '{}': {}", path, error.message());
                return false;
            }
        }

        /// @brief 重建成功后返回新场景；失败保留选择及文件关联。
        std::optional<Scene::RebuiltScene> load(FileIO::FileIO& file, std::string_view path, bool rethrow = false)
        {
            try
            {
                const auto description = Scene::readScene(file, path);
                auto rebuilt = Scene::rebuildScene(description, [this](const auto& source) { return cache_->resolve(source); });
                currentFile_ = path;
                message_ = "Loaded: " + currentFile_;
                messageError_ = false;
                select({});
                Log::Registry::get("editor").info("Loaded scene '{}' ({} entities)", logPath(currentFile_),
                                                  rebuilt.scene->entities().size());
                return rebuilt;
            }
            catch (const Error::Exception& error)
            {
                if (rethrow)
                    throw;
                messageError_ = true;
                message_ = error.message();
                if (!dynamic_cast<const Model::LoadError*>(&error))
                    Log::Registry::get("editor").error("Failed to load scene '{}': {}", path, error.message());
                return std::nullopt;
            }
        }

        size_t cachedModels() const { return cache_->size(); }
        size_t collectUnusedModels()
        {
            const auto removed = cache_->collectUnused();
            messageError_ = false;
            message_ = "Removed " + std::to_string(removed) + " unused cached models";
            return removed;
        }
        const std::string& currentFile() const { return currentFile_; }
        const std::string& message() const { return message_; }
        bool messageError() const { return messageError_; }

    private:
        static std::string logPath(std::string_view path)
        {
            std::error_code error;
            const auto absolute = std::filesystem::absolute(Utils::Utf8::toFsPath(path), error);
            return error ? std::string(path) : Utils::Utf8::toUtf8String(absolute.lexically_normal());
        }
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
