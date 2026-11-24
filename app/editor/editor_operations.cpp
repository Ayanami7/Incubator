#include "editor_operations.h"

#include <algorithm>
#include <filesystem>
#include <utility>

#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/model/prepared_model.h"
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

        bool sameMaterialOverrides(std::span<const Scene::EntityMaterialOverride> a,
                                   std::span<const Scene::EntityMaterialOverride> b) noexcept
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].value != b[i].value)
                    return false;
            }
            return true;
        }

        bool sameEntity(const Scene::Entity& a, const Scene::Entity& b) noexcept
        {
            return a.id == b.id && a.parent == b.parent && a.name == b.name && a.visible == b.visible &&
                   a.model == b.model && sameTransform(a.transform, b.transform) &&
                   sameMaterialOverrides(a.materialOverrides, b.materialOverrides);
        }

        bool sameSnapshotState(const std::vector<Scene::Entity>& beforeEntities, const Render::Lighting& beforeLighting,
                               const Render::DisplaySettings& beforeDisplay,
                               const std::array<float, 4>& beforeClearColor, const Scene::Scene& scene,
                               const std::array<float, 4>& clearColor)
        {
            if (!(beforeLighting == scene.lighting()) || !(beforeDisplay == scene.display()) ||
                beforeClearColor != clearColor)
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
        return {std::vector<Scene::Entity>(entities.begin(), entities.end()), scene.lighting(), scene.display(),
                clearColor, selection_.selected(), revision};
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
        scene.setDisplay(snapshot.display);
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
        if (sameSnapshotState(before.entities, before.lighting, before.display, before.clearColor, scene, clearColor))
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

    bool EditorOperations::setEntityShading(Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                            Render::ShadingMode shading)
    {
        const auto* entity = scene.find(selection_.selected());
        if (!entity || !entity->model)
            return false;
        const auto& source = entity->model->source();
        if (source.options().shading == shading)
            return false;
        Model::PrepareOptions options = source.options();
        options.shading = shading;
        const std::string path = source.path();
        try
        {
            auto model = cache_->resolve(Model::ModelReference(path, options));
            auto before = captureSnapshot(scene, clearColor, currentRevision_);
            if (!scene.setModel(entity->id, std::move(model)))
                return false;
            pushUndoSnapshot(std::move(before));
            messageError_ = false;
            message_ = fmt::format("Updated shading for '{}' to {}", entity->name,
                                   shading == Render::ShadingMode::Unlit ? "Unlit"
                                   : shading == Render::ShadingMode::Pbr ? "PBR"
                                                                         : "Lambert");
            Log::Registry::get("editor").info("{}", message_);
            return true;
        }
        catch (const Error::Exception& error)
        {
            messageError_ = true;
            message_ = "Cannot update shading for '" + path + "': " + error.message();
            if (!dynamic_cast<const Model::LoadError*>(&error))
                Log::Registry::get("editor").error("{}", message_);
            return false;
        }
    }

    bool EditorOperations::setMaterialOverride(Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                               const Scene::MaterialOverride& overrideValue)
    {
        const auto* entity = scene.find(selection_.selected());
        if (!entity || !entity->model)
            return false;
        const auto* slot = entity->model->findMaterialSlot(overrideValue.materialSlot);
        if (!slot || slot->sourceDesc.shading != Render::ShadingMode::Pbr)
            return false;
        for (const auto& existing : entity->materialOverrides)
        {
            if (existing.value == overrideValue)
                return true;
        }
        try
        {
            std::shared_ptr<const Model::PreparedMaterialOverride> compiled;
            if (renderer_)
            {
                compiled = Model::prepareMaterialOverride(
                    *renderer_, *entity->model, overrideValue.materialSlot, overrideValue.color,
                    overrideValue.roughnessMode, overrideValue.roughness, overrideValue.metallicMode,
                    overrideValue.metallic);
            }
            if (transaction_)
                return scene.setMaterialOverride(entity->id, overrideValue, std::move(compiled));
            auto before = captureSnapshot(scene, clearColor, currentRevision_);
            if (!scene.setMaterialOverride(entity->id, overrideValue, std::move(compiled)))
                return false;
            pushUndoSnapshot(std::move(before));
            messageError_ = false;
            return true;
        }
        catch (const Error::Exception& error)
        {
            messageError_ = true;
            message_ = error.message();
            Log::Registry::get("editor").error("Failed to override material slot {}: {}", overrideValue.materialSlot,
                                               error.message());
            return false;
        }
    }

    bool EditorOperations::resetMaterialOverride(Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                                 uint32_t materialSlot)
    {
        const auto* entity = scene.find(selection_.selected());
        if (!entity)
            return false;
        const bool hasSlot = std::ranges::any_of(entity->materialOverrides, [materialSlot](const auto& entry) {
            return entry.value.materialSlot == materialSlot;
        });
        if (!hasSlot)
            return false;
        if (transaction_)
            return scene.clearMaterialOverride(entity->id, materialSlot);
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        if (!scene.clearMaterialOverride(entity->id, materialSlot))
            return false;
        pushUndoSnapshot(std::move(before));
        messageError_ = false;
        return true;
    }

    bool EditorOperations::resetAllMaterialOverrides(Scene::Scene& scene, const std::array<float, 4>& clearColor)
    {
        const auto* entity = scene.find(selection_.selected());
        if (!entity || entity->materialOverrides.empty())
            return false;
        if (transaction_)
            return scene.clearMaterialOverrides(entity->id);
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        if (!scene.clearMaterialOverrides(entity->id))
            return false;
        pushUndoSnapshot(std::move(before));
        messageError_ = false;
        return true;
    }

    bool EditorOperations::setParent(Scene::Scene& scene, const std::array<float, 4>& clearColor, Scene::EntityId id,
                                     Scene::EntityId parent)
    {
        commitTransaction(scene, clearColor);
        const auto* entity = scene.find(id);
        if (!entity)
            return false;
        if (entity->parent == parent)
            return false;
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        if (!scene.setParent(id, parent))
        {
            messageError_ = true;
            message_ = "Cannot reparent entity: invalid target or cycle";
            return false;
        }
        pushUndoSnapshot(std::move(before));
        messageError_ = false;
        message_ = parent.valid() ? fmt::format("Parented #{} under #{} (kept local transform)", id.value(), parent.value())
                                  : fmt::format("Unparented #{} (kept local transform)", id.value());
        Log::Registry::get("editor").info("{}", message_);
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
        const auto* entity = scene.find(selection_.selected());
        if (!entity)
        {
            selection_.remove(scene);
            return false;
        }
        if (scene.hasChildren(entity->id))
        {
            messageError_ = true;
            message_ = fmt::format("Cannot delete '{}' (#{}): unparent or move children first",
                                   entity->name.empty() ? "Unnamed" : entity->name, entity->id.value());
            Log::Registry::get("editor").warn("{}", message_);
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

    bool EditorOperations::setDisplay(Scene::Scene& scene, const std::array<float, 4>& clearColor,
                                      Render::DisplaySettings display)
    {
        if (scene.display() == display)
            return true;
        if (transaction_)
        {
            scene.setDisplay(display);
            return true;
        }
        auto before = captureSnapshot(scene, clearColor, currentRevision_);
        scene.setDisplay(display);
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
            Scene::MaterialOverrideResolver overrideResolver;
            if (renderer_)
            {
                overrideResolver = [this](const Model::PreparedModel& model, const Scene::MaterialOverride& ov) {
                    return Model::prepareMaterialOverride(*renderer_, model, ov.materialSlot, ov.color,
                                                          ov.roughnessMode, ov.roughness, ov.metallicMode, ov.metallic);
                };
            }
            auto rebuilt = Scene::rebuildScene(
                description, [this](const auto& source) { return cache_->resolve(source); }, overrideResolver);
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

    std::optional<ModelReloadReport> EditorOperations::reloadModel(Scene::Scene& scene, std::string_view virtualPath)
    {
        if (inTransaction())
        {
            message_ = "Finish the active edit before reloading a model";
            messageError_ = true;
            return std::nullopt;
        }

        struct Replacement
        {
            Scene::EntityId id;
            Scene::Entity* snapshotEntity;
            std::shared_ptr<const Model::PreparedModel> model;
            std::vector<Scene::EntityMaterialOverride> overrides;
        };
        std::vector<Replacement> replacements;
        ModelReloadReport report;
        std::string success = fmt::format("No cached model for '{}'", virtualPath);
        try
        {
            if (cache_)
            {
                cache_->reloadByPath(virtualPath, [&](std::span<const Model::ReloadedModel> variants) {
                    report.reloadedVariants = variants.size();
                    const auto prepare = [&](const Scene::Entity& entity, Scene::Entity* snapshotEntity) {
                        const auto variant = std::ranges::find(variants, entity.model, &Model::ReloadedModel::previous);
                        if (variant == variants.end())
                            return;
                        Replacement replacement{entity.id, snapshotEntity, variant->current, {}};
                        replacement.overrides.reserve(entity.materialOverrides.size());
                        for (const auto& entry : entity.materialOverrides)
                        {
                            const auto& value = entry.value;
                            try
                            {
                                auto compiled = Model::prepareMaterialOverride(
                                    *renderer_, *variant->current, value.materialSlot, value.color,
                                    value.roughnessMode, value.roughness, value.metallicMode, value.metallic);
                                replacement.overrides.push_back({value, std::move(compiled)});
                            }
                            catch (const Error::Exception& error)
                            {
                                throw Error::Exception(
                                    error.code(), fmt::format("Entity '{}' ({}), material slot {}: {}",
                                                              entity.name, entity.id.value(), value.materialSlot,
                                                              error.message()));
                            }
                        }
                        replacements.push_back(std::move(replacement));
                    };
                    for (const auto& entity : scene.entities())
                        prepare(entity, nullptr);
                    report.updatedEntities = replacements.size();
                    for (auto* stack : {&undoStack_, &redoStack_})
                    {
                        for (auto& snapshot : *stack)
                        {
                            const auto before = replacements.size();
                            for (auto& entity : snapshot.entities)
                                prepare(entity, &entity);
                            if (replacements.size() != before)
                                ++report.updatedHistorySnapshots;
                        }
                    }
                    success = fmt::format("Reloaded '{}': {} variants, {} entities, {} history snapshots",
                                          variants.front().current->source().path(), report.reloadedVariants,
                                          report.updatedEntities, report.updatedHistorySnapshots);
                });
            }
        }
        catch (const Error::Exception& error)
        {
            message_ = error.message();
            messageError_ = true;
            return std::nullopt;
        }

        // 缓存已通过验证，场景与历史只移动准备完成的模型和覆盖。
        for (auto& replacement : replacements)
        {
            if (auto* entity = replacement.snapshotEntity)
            {
                entity->materialOverrides = std::move(replacement.overrides);
                entity->model = std::move(replacement.model);
            }
            else
            {
                scene.setReloadedModel(replacement.id, std::move(replacement.model),
                                       std::move(replacement.overrides));
            }
        }
        message_ = std::move(success);
        messageError_ = false;
        return report;
    }

    std::string EditorOperations::logPath(std::string_view path)
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(Utils::Utf8::toFsPath(path), error);
        return error ? std::string(path) : Utils::Utf8::toUtf8String(absolute.lexically_normal());
    }
}  // namespace Incubator::Editor
