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
    namespace
    {
        bool sameTransform(const Scene::Transform& a, const Scene::Transform& b) noexcept
        {
            return a.position() == b.position() && a.rotation() == b.rotation() && a.scale() == b.scale();
        }

        bool sameEntity(const Scene::Entity& a, const Scene::Entity& b) noexcept
        {
            return a.id == b.id && a.name == b.name && a.visible == b.visible && a.model == b.model &&
                   sameTransform(a.transform, b.transform);
        }

        bool sameSnapshotState(const std::vector<Scene::Entity>& beforeEntities, const Render::Lighting& beforeLighting,
                               const std::array<float, 4>& beforeClearColor, const Scene::Scene& scene,
                               const std::array<float, 4>& clearColor)
        {
            if (!(beforeLighting == scene.lighting()) || beforeClearColor != clearColor)
                return false;
            const auto entities = scene.entities();
            if (beforeEntities.size() != entities.size())
                return false;
            for (size_t i = 0; i < beforeEntities.size(); ++i)
            {
                if (!sameEntity(beforeEntities[i], entities[i]))
                    return false;
            }
            return true;
        }
    }  // namespace

    EditorOperations::HistorySnapshot EditorOperations::captureSnapshot(const Scene::Scene& scene,
                                                                        const std::array<float, 4>& clearColor,
                                                                        uint64_t revision) const
    {
        const auto entities = scene.entities();
        return {std::vector<Scene::Entity>(entities.begin(), entities.end()), scene.lighting(), clearColor,
                selection_.selected(), revision};
    }

    void EditorOperations::pushUndoSnapshot(HistorySnapshot before)
    {
        undoStack_.push_back(std::move(before));
        if (undoStack_.size() > MAX_HISTORY)
        {
            const uint64_t evictedRevision = undoStack_.front().revision;
            undoStack_.erase(undoStack_.begin());
            if (savedRevision_ && *savedRevision_ == evictedRevision)
                savedRevision_.reset();
        }
        redoStack_.clear();
        currentRevision_ = nextRevision_++;
    }

    void EditorOperations::applySnapshot(Scene::Scene& scene, std::array<float, 4>& clearColor,
                                         const HistorySnapshot& snapshot)
    {
        scene.restoreEntities(snapshot.entities);
        scene.setLighting(snapshot.lighting);
        clearColor = snapshot.clearColor;
        selection_.select(snapshot.selected);
        currentRevision_ = snapshot.revision;
    }

    void EditorOperations::beginTransaction(const Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        if (transaction_)
            return;
        transaction_ = captureSnapshot(scene, clearColor, currentRevision_);
    }

    bool EditorOperations::commitTransaction(const Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        if (!transaction_)
            return false;
        auto before = std::move(*transaction_);
        transaction_.reset();
        if (sameSnapshotState(before.entities, before.lighting, before.clearColor, scene, clearColor))
            return false;
        pushUndoSnapshot(std::move(before));
        return true;
    }

    bool EditorOperations::cancelTransaction(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        if (!transaction_)
            return false;
        const auto before = std::move(*transaction_);
        transaction_.reset();
        applySnapshot(scene, clearColor, before);
        return true;
    }

    bool EditorOperations::addModel(Scene::Scene& scene, const std::array<float, 4>& clearColor, std::string_view path,
                                    const Scene::Transform& transform)
    {
        try
        {
            commitTransaction(scene, clearColor);
            auto model = cache_->resolve(Model::ModelReference(path, PREVIEW_OPTIONS));
            auto name = Utils::Utf8::toUtf8String(Utils::Utf8::toFsPath(path).stem());
            auto before = captureSnapshot(scene, clearColor, currentRevision_);
            selection_.select(scene.createEntity(std::move(model), transform, true, std::move(name)));
            pushUndoSnapshot(std::move(before));
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

    bool EditorOperations::rename(Scene::Scene& scene, const std::array<float, 4>& clearColor, std::string name)
    {
        const auto* entity = scene.find(selection_.selected());
        if (!entity)
            return false;
        if (entity->name == name)
            return true;
        if (transaction_)
            return selection_.rename(scene, std::move(name));
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        if (!selection_.rename(scene, std::move(name)))
            return false;
        pushUndoSnapshot(std::move(before));
        return true;
    }

    bool EditorOperations::setVisible(Scene::Scene& scene, const std::array<float, 4>& clearColor, bool visible)
    {
        return setVisible(scene, clearColor, selection_.selected(), visible);
    }

    bool EditorOperations::setVisible(Scene::Scene& scene, const std::array<float, 4>& clearColor, Scene::EntityId id,
                                      bool visible)
    {
        const auto* entity = scene.find(id);
        if (!entity)
            return false;
        if (entity->visible == visible)
            return true;
        if (transaction_)
            return scene.setVisible(id, visible);
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        if (!scene.setVisible(id, visible))
            return false;
        pushUndoSnapshot(std::move(before));
        return true;
    }

    bool EditorOperations::setTransform(Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                        Scene::Transform transform)
    {
        const auto* entity = scene.find(selection_.selected());
        if (!entity)
            return false;
        if (sameTransform(entity->transform, transform))
            return true;
        if (transaction_)
            return selection_.setTransform(scene, std::move(transform));
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        if (!selection_.setTransform(scene, std::move(transform)))
            return false;
        pushUndoSnapshot(std::move(before));
        return true;
    }

    Scene::EntityId EditorOperations::duplicate(Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        commitTransaction(scene, clearColor);
        const auto* source = scene.find(selection_.selected());
        if (!source)
            return {};
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        const auto id = selection_.duplicate(scene);
        if (id.valid())
            pushUndoSnapshot(std::move(before));
        return id;
    }

    bool EditorOperations::remove(Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        commitTransaction(scene, clearColor);
        const bool existed = scene.find(selection_.selected()) != nullptr;
        if (!existed)
        {
            selection_.remove(scene);
            return false;
        }
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        selection_.remove(scene);
        pushUndoSnapshot(std::move(before));
        return true;
    }

    bool EditorOperations::clear(Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        commitTransaction(scene, clearColor);
        if (scene.entities().empty())
        {
            selection_.clear(scene);
            return false;
        }
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        selection_.clear(scene);
        pushUndoSnapshot(std::move(before));
        return true;
    }

    bool EditorOperations::setLighting(Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                       Render::Lighting lighting)
    {
        if (scene.lighting() == lighting)
            return true;
        if (transaction_)
        {
            scene.setLighting(lighting);
            return true;
        }
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        scene.setLighting(lighting);
        pushUndoSnapshot(std::move(before));
        return true;
    }

    bool EditorOperations::setClearColor(const Scene::Scene& scene, std::array<float, 4>& clearColor,
                                         const std::array<float, 4>& newColor)
    {
        if (clearColor == newColor)
            return true;
        if (transaction_)
        {
            clearColor = newColor;
            return true;
        }
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        clearColor = newColor;
        pushUndoSnapshot(std::move(before));
        return true;
    }

    bool EditorOperations::undo(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        commitTransaction(scene, clearColor);
        if (undoStack_.empty())
            return false;
        auto current = captureSnapshot(scene, clearColor, currentRevision_);
        auto target = std::move(undoStack_.back());
        undoStack_.pop_back();
        applySnapshot(scene, clearColor, target);
        redoStack_.push_back(std::move(current));
        messageError_ = false;
        message_ = "Undid last action";
        return true;
    }

    bool EditorOperations::redo(Scene::Scene& scene, std::array<float, 4>& clearColor)
    {
        commitTransaction(scene, clearColor);
        if (redoStack_.empty())
            return false;
        auto current = captureSnapshot(scene, clearColor, currentRevision_);
        auto target = std::move(redoStack_.back());
        redoStack_.pop_back();
        applySnapshot(scene, clearColor, target);
        undoStack_.push_back(std::move(current));
        if (undoStack_.size() > MAX_HISTORY)
        {
            const uint64_t evictedRevision = undoStack_.front().revision;
            undoStack_.erase(undoStack_.begin());
            if (savedRevision_ && *savedRevision_ == evictedRevision)
                savedRevision_.reset();
        }
        messageError_ = false;
        message_ = "Redid action";
        return true;
    }

    bool EditorOperations::save(FileIO::FileIO& file, const Scene::Scene& scene, const std::array<float, 4>& color,
                                std::string_view path)
    {
        try
        {
            commitTransaction(scene, color);
            std::string target(path);
            Scene::writeScene(file, path, Scene::captureScene(scene, color));
            messageError_ = false;
            message_ = "Saved: " + target;
            currentFile_ = std::move(target);
            savedRevision_ = currentRevision_;
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
            selection_.select({});
            undoStack_.clear();
            redoStack_.clear();
            transaction_.reset();
            currentRevision_ = nextRevision_++;
            savedRevision_ = currentRevision_;
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
