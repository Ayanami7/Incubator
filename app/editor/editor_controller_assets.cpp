#include "editor_controller.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <utility>

#include "editor_app.h"
#include "runtime/core/error/error.h"
#include "runtime/core/log/logger.h"
#include "runtime/core/utils/utf8.h"
#include "runtime/platform/fileio/fileio.h"
#include "runtime/resource/asset_importer.h"
#include "runtime/resource/resource_manager.h"

namespace Incubator::Editor
{
    bool EditorController::browseDirectory(std::string_view path)
    {
        return browseDirectory(path, nullptr);
    }

    bool EditorController::browseDirectory(std::string_view path, const Resource::AssetSource* source)
    {
        try
        {
            auto entries = source ? source->listDirectory(path) : Resource::ResourceManager::instance().listDirectory(path);
            if (assetDirectory_ != path)
            {
                assetDirectory_ = path;
                focusedAssetPath_.clear();
            }
            assetEntries_ = std::move(entries);
            assetError_.clear();
            return true;
        }
        catch (const Error::Exception& error)
        {
            assetEntries_.clear();
            assetError_ = "Cannot browse '" + std::string(path) + "': " + error.message();
            Log::Registry::get("editor").error("{}", assetError_);
            return false;
        }
    }

    bool EditorController::parentDirectory()
    {
        const auto slash = assetDirectory_.find_last_of('/');
        return browseDirectory(slash == std::string::npos ? std::string{} : assetDirectory_.substr(0, slash));
    }

    std::optional<std::string> EditorController::importExternalModel(Resource::AssetSource& assetSource,
                                                                    const std::filesystem::path& externalPath)
    {
        Resource::ImportedModelResult imported;
        try
        {
            imported = Resource::importExternalModel(assetSource, externalPath,
                                                      {assetDirectory_.empty() ? "models" : assetDirectory_});
        }
        catch (const Error::Exception& error)
        {
            operations_.setStatus(fmt::format("Cannot import '{}': {}", Utils::Utf8::toUtf8String(externalPath),
                                               error.message()), true);
            return std::nullopt;
        }
        if (browseDirectory(imported.targetDirectory, &assetSource))
            focusedAssetPath_ = imported.virtualPath;
        operations_.setStatus("Imported: " + imported.virtualPath);
        return std::move(imported.virtualPath);
    }

    std::optional<std::string> EditorController::importExternalModel(EditorApp& app,
                                                                    const std::filesystem::path& externalPath)
    {
        return importExternalModel(app.assetSource(), externalPath);
    }

    bool EditorController::reloadAsset(Scene::Scene& scene, std::string_view virtualPath)
    {
        return operations_.reloadModel(scene, virtualPath).has_value();
    }

    bool EditorController::reloadAsset(EditorApp& app, std::string_view virtualPath)
    {
        return reloadAsset(app.scene(), virtualPath);
    }

    void EditorController::refreshSceneFiles(FileIO::FileIO& fileIO)
    {
        sceneFiles_.clear();
        std::vector<std::string> scanDirs;
        scanDirs.emplace_back("assets/scenes");
        if (!operations_.currentFile().empty() && Utils::Utf8::isValid(operations_.currentFile()))
        {
            const std::filesystem::path currentParent =
                Utils::Utf8::toFsPath(operations_.currentFile()).parent_path();
            if (!currentParent.empty() && !currentParent.is_absolute())
            {
                std::string parentStr = Utils::Utf8::toUtf8String(currentParent);
                std::replace(parentStr.begin(), parentStr.end(), '\\', '/');
                if (parentStr != "assets/scenes")
                    scanDirs.push_back(std::move(parentStr));
            }
        }

        for (const auto& dir : scanDirs)
        {
            try
            {
                const std::filesystem::path dirPath = Utils::Utf8::toFsPath(dir);
                if (!fileIO.directoryExists(dirPath))
                    continue;
                for (const auto& entry : fileIO.listDirectory(dirPath))
                {
                    if (entry.directory || !Utils::Utf8::isValid(entry.name))
                        continue;
                    const std::filesystem::path entryPath = Utils::Utf8::toFsPath(entry.name);
                    std::string ext = Utils::Utf8::toUtf8String(entryPath.extension());
                    std::transform(ext.begin(), ext.end(), ext.begin(),
                                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (ext != ".json")
                        continue;
                    std::string normalized = Utils::Utf8::toUtf8String(dirPath / entryPath);
                    std::replace(normalized.begin(), normalized.end(), '\\', '/');
                    sceneFiles_.push_back(std::move(normalized));
                }
            }
            catch (...)
            {
                // 静默容错：目录不存在或枚举失败时不改写 operations_.message() / messageError()。
            }
        }
        std::sort(sceneFiles_.begin(), sceneFiles_.end());
        sceneFiles_.erase(std::unique(sceneFiles_.begin(), sceneFiles_.end()), sceneFiles_.end());
    }

    void EditorController::refreshSceneFiles(EditorApp& app)
    {
        refreshSceneFiles(app.fileIO());
    }

}  // namespace Incubator::Editor
