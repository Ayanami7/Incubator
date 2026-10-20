#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "runtime/render/frame_input.h"
#include "runtime/resource/loader/model_resource.h"

namespace Incubator
{
    namespace Render
    {
        class Renderer;
    }

    namespace Model
    {
        class PreparedModel;

        // Requires MODEL/TEXTURE Loader routes and an initialized Renderer. Scene shaders are
        // prepared explicitly by the caller. Throws on failure; releases only this call's CPU holds.
        std::shared_ptr<const PreparedModel> prepareModel(Render::Renderer& renderer, std::string_view virtualPath);

        // Immutable metadata and weak GPU references, shared by model consumers. No CPU asset is
        // retained. GPU resources belong to the Renderer and expire when it closes.
        class PreparedModel final
        {
        public:
            const std::vector<Render::Mesh>& meshes() const noexcept
            {
                return meshes_;
            }
            const std::vector<Render::Material>& materials() const noexcept
            {
                return materials_;
            }
            const std::vector<Render::Texture>& textures() const noexcept
            {
                return textures_;
            }
            const std::vector<Resource::ModelPart>& parts() const noexcept
            {
                return parts_;
            }
            const std::vector<Resource::ModelNode>& nodes() const noexcept
            {
                return nodes_;
            }
            uint32_t rootNode() const noexcept
            {
                return rootNode_;
            }
            // Depth-first, parent-before-children order; model matrices include the source root.
            const std::vector<Render::DrawItem>& draws() const noexcept
            {
                return draws_;
            }

        private:
            friend std::shared_ptr<const PreparedModel> prepareModel(Render::Renderer&, std::string_view);
            PreparedModel() = default;

            std::vector<Render::Mesh> meshes_;
            std::vector<Render::Material> materials_;
            std::vector<Render::Texture> textures_;
            std::vector<Resource::ModelPart> parts_;
            std::vector<Resource::ModelNode> nodes_;
            uint32_t rootNode_ = 0;
            std::vector<Render::DrawItem> draws_;
        };
    }  // namespace Model
}  // namespace Incubator
