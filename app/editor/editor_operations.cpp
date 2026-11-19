#include "editor_operations.h"

#include <filesystem>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/scene/scene_file.h"
#include "runtime/scene/scene_rebuild.h"

namespace Incubator::Editor
{
    bool EditorOperations::addModel(Scene::Scene& scene, std::string_view path, const Scene::Transform& transform)
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

    bool EditorOperations::save(FileIO::FileIO& file, const Scene::Scene& scene, const std::array<float, 4>& color,
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

    std::optional<Scene::RebuiltScene> EditorOperations::load(FileIO::FileIO& file, std::string_view path, bool rethrow)
    {
        try
        {
            const auto description = Scene::readScene(file, path);
            auto rebuilt =
                Scene::rebuildScene(description, [this](const auto& source) { return cache_->resolve(source); });
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

    size_t EditorOperations::collectUnusedModels()
    {
        const auto removed = cache_->collectUnused();
        messageError_ = false;
        message_ = "Removed " + std::to_string(removed) + " unused cached models";
        return removed;
    }

    std::string EditorOperations::logPath(std::string_view path)
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(Utils::Utf8::toFsPath(path), error);
        return error ? std::string(path) : Utils::Utf8::toUtf8String(absolute.lexically_normal());
    }
}  // namespace Incubator::Editor
