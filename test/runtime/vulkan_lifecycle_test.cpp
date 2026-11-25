#include "vulkan_review_fixture.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Render;
    using namespace Incubator::Render::Vulkan;
    using namespace Incubator::Test;

    class VulkanLifecycleTest : public VulkanReviewTest
    {};


    TEST_F(VulkanLifecycleTest, IndependentTexturesShareUploadsAndExpireAfterRendererMoveAndClose)
    {
        Log::Registry::get("render.vulkan").setLevel(Log::Level::DEBUG);
        auto window = createWindow(true, 640, 480);
        Texture reference, direction;
        const std::string texturePath = "textures/资源入口 方向 🧪.png";
        struct TemporaryTexture
        {
            std::filesystem::path path;
            ~TemporaryTexture()
            {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } temporary{Utils::Utf8::toFsPath("assets/" + texturePath)};
        std::filesystem::copy_file("assets/textures/direction_rgba.png", temporary.path,
                                   std::filesystem::copy_options::overwrite_existing);
        FileAudit firstAudit, secondAudit;
        secondAudit.paths.texture = texturePath;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            Material material;
            {
                SceneAssets assets({}, std::make_unique<AuditedFileIO>(firstAudit));
                reference = renderer.createTexture(*assets.texture);
                releaseCpuAssets(assets);
            }
            {
                SceneAssets assets(secondAudit.paths, std::make_unique<AuditedFileIO>(secondAudit));
                direction = renderer.createTexture(*assets.texture);
                const auto shared = direction;
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                material = renderer.createMaterial({shared});
                releaseCpuAssets(assets);
            }
            EXPECT_TRUE(firstAudit.cpuReleasedBeforeDestroy && secondAudit.cpuReleasedBeforeDestroy);
            EXPECT_TRUE(reference.valid() && direction.valid() && !Texture{}.valid());
            Renderer moved(std::move(renderer));
            EXPECT_THROW(renderer.createTexture(Resource::TextureResource({}, 1, 1, {255, 255, 255, 255})),
                         Error::Exception);
            const auto data = Demo::makePlaneMesh();
            const std::array draws{DrawItem{moved.createMesh(data.vertices, data.indices), material, glm::mat4(1)}};
            for (int f = 0; f < 6; ++f)
            {
                submitControlledScene(*window, moved, draws);
                glfwWaitEventsTimeout(0.025);
            }
            const auto image = WindowCapture::capture(*window);
            const auto b = WindowCapture::bounds(image);
            ASSERT_GT(b[2] - b[0], 100);
            const size_t offset =
                (static_cast<size_t>(b[1] + (b[3] - b[1]) / 8) * image.width + b[0] + (b[2] - b[0]) / 6) * 4;
            for (int c = 0; c < 3; ++c)
                EXPECT_GT(image.rgba[offset + c], 220);
            EXPECT_EQ(TextureGpuReview::countSceneTextureUploads(messages_.str()), 2u);
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 4u);
            EXPECT_EQ(firstAudit.textureOpens, 1u);
            EXPECT_EQ(secondAudit.textureOpens, 1u);
        }
        EXPECT_FALSE(reference.valid() || direction.valid());
        expectReleased({});
        expectReleased(secondAudit.paths);
        expectCleanValidation();
    }

    static_assert(!std::is_copy_constructible_v<GpuResources>);
    static_assert(std::is_nothrow_move_constructible_v<GpuResources>);

    TEST_F(VulkanLifecycleTest, RendererInitFailureInvalidReferencesAndSubmissionRejection)
    {
        {
            auto native = createWindow();
            DeferredFramebufferWindow window(*native);
            window.throwOnSize = true;
            EXPECT_THROW((Renderer(window, RendererValidationMode::Required)), Error::Exception);
            Renderer recovered(*native, RendererValidationMode::Required);
            submitControlledScene(*native, recovered, {}, true);
        }
        {
            auto window = createWindow();
            SceneAssets assets;
            Texture expired;
            {
                Renderer previous(*window, RendererValidationMode::Required);
                expired = previous.createTexture(*assets.texture);
            }
            EXPECT_FALSE(expired.valid());
            Texture observer;
            {
                Renderer renderer(*window, RendererValidationMode::Required);
                auto texture = renderer.createTexture(*assets.texture);
                observer = texture;
                Resource::ShaderResource emptyShader({}, {});
                EXPECT_THROW(renderer.createMaterial({texture}), Error::Exception);
                EXPECT_THROW(renderer.prepareSceneResources(emptyShader, *assets.fragment), Error::Exception);
                EXPECT_THROW(renderer.prepareSceneResources(*assets.vertex, emptyShader), Error::Exception);
                const auto mesh = createCubeMesh(renderer);
                auto extent = renderer.beginUiFrame();
                ASSERT_TRUE(extent);
                renderer.imgui().endFrame();
                int recorded = 0;
                EXPECT_THROW(renderer.drawFrame(controlledFrame(*extent, std::array{DrawItem{mesh, {}, glm::mat4(1)}}),
                                                {0.2f, 0.24f, 0.28f, 1}, [&](void*) { ++recorded; }),
                             Error::Exception);
                EXPECT_EQ(recorded, 0);
                renderer.drawFrame(controlledFrame(*extent, {}), {0.2f, 0.24f, 0.28f, 1});
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                EXPECT_THROW(renderer.createMaterial({}), Error::Exception);
                EXPECT_THROW(renderer.createMaterial({expired}), Error::Exception);
                const auto material = renderer.createMaterial({texture});
                EXPECT_THROW(renderer.prepareSceneResources(*assets.vertex, *assets.fragment), Error::Exception);
                texture = {};
                releaseCpuAssets(assets);
                EXPECT_TRUE(observer.valid());
                EXPECT_GT(drawUiFrame(*window, renderer, mesh, material), 0);
            }
            EXPECT_FALSE(observer.valid());
            expectReleased({});
        }
        {
            auto window = createWindow();
            SceneAssets assets;
            Material expired;
            Mesh expiredMesh;
            {
                Renderer previous(*window, RendererValidationMode::Required);
                previous.prepareSceneResources(*assets.vertex, *assets.fragment);
                expired = previous.createMaterial({previous.createTexture(*assets.texture)});
                expiredMesh = createCubeMesh(previous);
            }
            EXPECT_FALSE(expired.valid());
            Material observer;
            {
                Renderer renderer(*window, RendererValidationMode::Required);
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
                MaterialDesc desc{renderer.createTexture(*assets.texture)};
                observer = renderer.createMaterial(desc);
                Renderer moved(std::move(renderer));
                const auto output = moved.prepareSceneTarget({320, 240});
                EXPECT_THROW(renderer.prepareSceneTarget({320, 240}), Error::Exception);
                EXPECT_THROW(renderer.drawSceneTarget(output, {}, {0, 0, 0, 1}), Error::Exception);
                EXPECT_THROW((void)renderer.sceneImage(output), Error::Exception);
                EXPECT_THROW(renderer.createResources(), Error::Exception);
                EXPECT_THROW(renderer.createMaterial(desc), Error::Exception);
                EXPECT_THROW(renderer.prepareSceneResources(*assets.vertex, *assets.fragment), Error::Exception);
                EXPECT_THROW(renderer.drawFrame(controlledFrame({320, 240}, {}), {0, 0, 0, 1}), Error::Exception);
                EXPECT_NE(moved.sceneImage(output), 0u);
                moved.drawSceneTarget(output, controlledFrame(output.extent(), {}), {0, 0, 0, 1});
                const auto mesh = createCubeMesh(moved);
                releaseCpuAssets(assets);
                const auto extent = moved.beginUiFrame();
                ASSERT_TRUE(extent);
                moved.imgui().endFrame();
                int recorded = 0;
                for (const auto& invalid : {DrawItem{mesh, {}, glm::mat4(1.0f)}, DrawItem{mesh, expired, glm::mat4(1.0f)},
                                            DrawItem{expiredMesh, observer, glm::mat4(1.0f)}})
                    EXPECT_THROW(moved.drawFrame(controlledFrame(*extent, std::array{DrawItem{mesh, observer, glm::mat4(1)}, invalid}),
                                                {0.2f, 0.24f, 0.28f, 1}, [&](void*) { ++recorded; }),
                                 Error::Exception);
                EXPECT_EQ(recorded, 0);
                EXPECT_GT(drawUiFrame(*window, moved, mesh, observer), 0);
            }
            EXPECT_FALSE(observer.valid());
            expectReleased({});
        }
        expectCleanValidation();
    }

    TEST_F(VulkanLifecycleTest, ScopedResourcesPreserveDependenciesRollbackAndSharedLateRelease)
    {
        auto window = createWindow();
        SceneAssets assets;
        std::shared_ptr<GpuResources> external;
        Mesh extMesh;
        Texture extTex;
        Material extMat;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto geometry = Demo::makeCubeMesh();
            const auto directMesh = renderer.createMesh(geometry.vertices, geometry.indices);
            const auto directTexture = renderer.createTexture(*assets.texture);
            const auto directMaterial = renderer.createMaterial({directTexture});
            const auto expectCounts = [&](size_t meshes, size_t textures, size_t materials) {
                const auto counts = Detail::RendererReviewAccess::resourceCounts(renderer);
                EXPECT_EQ(counts.meshes, meshes);
                EXPECT_EQ(counts.textures, textures);
                EXPECT_EQ(counts.materials, materials);
            };
            expectCounts(1, 1, 1);
            GpuResources invalid;
            EXPECT_THROW(renderer.createMesh(invalid, geometry.vertices, geometry.indices), Error::Exception);
            EXPECT_THROW(renderer.createTexture(invalid, *assets.texture), Error::Exception);
            EXPECT_THROW(renderer.createMaterial(invalid, {directTexture}), Error::Exception);
            {
                auto textures = renderer.createResources(), bindings = renderer.createResources();
                const auto tex = renderer.createTexture(textures, *assets.texture);
                const auto mat = renderer.createMaterial(bindings, {tex});
                const auto mesh = renderer.createMesh(bindings, geometry.vertices, geometry.indices);
                textures.reset();
                EXPECT_TRUE(tex.valid());
                expectCounts(2, 2, 2);
                auto moved = std::move(bindings);
                auto assigned = renderer.createResources();
                const auto discarded = renderer.createMesh(assigned, geometry.vertices, geometry.indices);
                assigned = std::move(moved);
                EXPECT_FALSE(bindings.valid() || discarded.valid() || moved.valid());
                EXPECT_TRUE(mesh.valid() && mat.valid());
                assigned.reset();
                EXPECT_FALSE(mesh.valid() || mat.valid() || tex.valid());
                EXPECT_THROW(renderer.drawFrame(controlledFrame({640, 360}, std::array{DrawItem{mesh, mat, glm::mat4(1)}}),
                                                {0, 0, 0, 1}),
                             Error::Exception);
            }
            expectCounts(1, 1, 1);
            for (int stage = 0; stage != 3; ++stage)
            {
                Mesh iMesh;
                Texture iTex;
                Material iMat;
                EXPECT_THROW(
                    {
                        auto attempt = renderer.createResources();
                        iMesh = renderer.createMesh(attempt, geometry.vertices, geometry.indices);
                        if (stage >= 1)
                            iTex = renderer.createTexture(attempt, *assets.texture);
                        if (stage >= 2)
                            iMat = renderer.createMaterial(attempt, {iTex});
                        throw std::runtime_error("interrupted");
                    },
                    std::runtime_error);
                EXPECT_FALSE(iMesh.valid() || iTex.valid() || iMat.valid());
                expectCounts(1, 1, 1);
            }
            EXPECT_GT(drawUiFrame(*window, renderer, directMesh, directMaterial), 0);
            external = std::make_shared<GpuResources>(renderer.createResources());
            extMesh = renderer.createMesh(*external, geometry.vertices, geometry.indices);
            extTex = renderer.createTexture(*external, *assets.texture);
            extMat = renderer.createMaterial(*external, {extTex});
            releaseCpuAssets(assets);
            Renderer moved(std::move(renderer));
            EXPECT_TRUE(external->valid() && extMesh.valid());
            EXPECT_GT(drawUiFrame(*window, moved, extMesh, extMat), 0);
        }
        EXPECT_FALSE(external->valid() || extMesh.valid() || extTex.valid() || extMat.valid());
        external.reset();
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanLifecycleTest, ScopedMainAndShadowResourcesStayUntilTheirFrameFencesComplete)
    {
        auto window = createWindow();
        SceneAssets assets;
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            renderer.prepareDisplayResources(*assets.displayVertex, *assets.displayFragment);
            auto depth = Resource::ResourceManager::instance().hold<Resource::ShaderResource>(
                Resource::ResourceType::SHADER, "shaders/shadow.vert.spv");
            ASSERT_TRUE(depth);
            renderer.prepareShadowResources(*depth.get<Resource::ShaderResource>());
            depth.reset();
            const auto geometry = Demo::makeCubeMesh();
            for (const bool shadow : {false, true})
            {
                auto resources = renderer.createResources();
                const auto texture = renderer.createTexture(resources, *assets.texture);
                const auto mesh = renderer.createMesh(resources, geometry.vertices, geometry.indices);
                const auto material = renderer.createMaterial(resources, {texture, {0, 1, 0}});
                Mesh caster = shadow ? renderer.createMesh(resources, geometry.vertices, geometry.indices) : Mesh{};
                const std::array draws{DrawItem{mesh, material, glm::mat4(1)}};
                const std::array casters{DrawItem{caster, {}, glm::mat4(1)}};
                SceneOutput output;
                for (int slot = 0; slot != 2; ++slot)
                {
                    const auto extent = renderer.beginUiFrame();
                    ASSERT_TRUE(extent);
                    renderer.imgui().endFrame();
                    if (shadow)
                    {
                        output = renderer.prepareSceneTarget({384, 256});
                        auto frame = controlledFrame(output.extent(), draws);
                        ShadowSettings settings;
                        settings.enabled = true;
                        frame.shadow = makeShadowFrame(settings, frame.lighting.direction);
                        frame.shadowDraws = casters;
                        renderer.drawSceneTarget(output, frame, {0.2f, 0.24f, 0.28f, 1});
                    }
                    else
                        renderer.drawFrame(controlledFrame(*extent, draws), {0.2f, 0.24f, 0.28f, 1});
                }
                resources.reset();
                EXPECT_TRUE(mesh.valid() && texture.valid() && material.valid());
                renderer.prepareSceneTarget({384, 256});
                EXPECT_TRUE(mesh.valid());
                if (shadow)
                {
                    EXPECT_TRUE(caster.valid());
                    Detail::RendererReviewAccess::withSceneOutput(
                        renderer, output, [&](const auto& device, const auto& image) {
                            EXPECT_LT(TextureGpuReview::captureMip(device, image, 0, {384, 256})[(128u * 384u + 192u) * 4u], 5u);
                        });
                }
                else
                    Detail::RendererReviewAccess::waitForFrames(renderer);
                EXPECT_FALSE(mesh.valid() || material.valid() || texture.valid() || caster.valid());
                const auto counts = Detail::RendererReviewAccess::resourceCounts(renderer);
                EXPECT_EQ(counts.meshes, 0u);
                EXPECT_EQ(counts.textures, 0u);
                EXPECT_EQ(counts.materials, 0u);
                EXPECT_THROW(renderer.createMaterial({texture}), Error::Exception);
            }
            releaseCpuAssets(assets);
        }
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanLifecycleTest, MeshCreationValidationCrossRendererRejectionAndLifecycle)
    {
        auto firstWindow = createWindow(), secondWindow = createWindow();
        SceneAssets assets;
        Mesh observer;
        {
            Renderer first(*firstWindow, RendererValidationMode::Required);
            const auto firstTex = first.createTexture(*assets.texture);
            first.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto firstMat = first.createMaterial({firstTex});
            auto data = Demo::makePlaneMesh();
            EXPECT_THROW(first.createMesh({}, data.indices), Error::Exception);
            EXPECT_THROW(first.createMesh(data.vertices, {}), Error::Exception);
            for (const auto& bad : {std::vector<uint32_t>{0, 1}, std::vector<uint32_t>{0, 1, 4},
                                    std::vector<uint32_t>{0, 1, std::numeric_limits<uint32_t>::max()}})
                EXPECT_THROW(first.createMesh(data.vertices, bad), Error::Exception);
            for (int axis = 0; axis < 3; ++axis)
                for (const float v : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
                {
                    auto badVerts = data.vertices;
                    badVerts.back().normal[axis] = v;
                    EXPECT_THROW(first.createMesh(badVerts, data.indices), Error::Exception);
                }
            auto mesh = first.createMesh(data.vertices, data.indices);
            observer = mesh;
            data = {};
            ASSERT_TRUE(mesh.valid() && observer.valid() && !Mesh{}.valid());
            {
                struct RestoreContext
                {
                    ImGuiContext* previous;
                    ~RestoreContext() { ImGui::SetCurrentContext(previous); }
                } restore{ImGui::GetCurrentContext()};
                ImGui::SetCurrentContext(nullptr);
                Renderer second(*secondWindow, RendererValidationMode::Required);
                second.prepareSceneResources(*assets.vertex, *assets.fragment);
                auto foreignGroup = first.createResources();
                EXPECT_THROW(second.createMesh(foreignGroup, data.vertices, data.indices), Error::Exception);
                EXPECT_THROW(second.createMaterial(foreignGroup, {firstTex}), Error::Exception);
                EXPECT_THROW(second.createMaterial({firstTex}), Error::Exception);
                const auto secMat = second.createMaterial({second.createTexture(*assets.texture)});
                const auto ownMesh = createCubeMesh(second);
                const auto extent = second.beginUiFrame();
                ASSERT_TRUE(extent);
                second.imgui().endFrame();
                int recorded = 0;
                for (const auto& foreign : {DrawItem{mesh, secMat, glm::mat4(1)}, DrawItem{ownMesh, firstMat, glm::mat4(1)}})
                    EXPECT_THROW(second.drawFrame(Demo::makeSceneFrame(*extent, std::array{DrawItem{ownMesh, secMat, glm::mat4(1)}, foreign}),
                                                  {0.2f, 0.3f, 0.4f, 1}, [&](void*) { ++recorded; }),
                                 Error::Exception);
                EXPECT_EQ(recorded, 0);
            }
            releaseCpuAssets(assets);
            mesh = {};
            for (int f = 0; f < 6; ++f)
                EXPECT_GT(drawUiFrame(*firstWindow, first, observer, firstMat), 0);
            Renderer moved(std::move(first));
            EXPECT_THROW(first.createMesh({}, {}), Error::Exception);
            EXPECT_GT(drawUiFrame(*firstWindow, moved, observer, firstMat), 0);
        }
        EXPECT_FALSE(observer.valid());
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanLifecycleTest, FirstZeroFramebufferVisibleMinimizeRestoreAndRapidShutdown)
    {
        auto native = createWindow(true);
        for (const bool prepareScene : {false, true})
        {
            DeferredFramebufferWindow window(*native);
            Renderer renderer(window, RendererValidationMode::Required);
            Mesh mesh;
            Material material;
            if (prepareScene)
            {
                SceneAssets assets;
                renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
                material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
                mesh = createCubeMesh(renderer);
                releaseCpuAssets(assets);
            }
            EXPECT_EQ(ImGui::GetIO().BackendRendererUserData, nullptr);
            const auto skippedLogs = countOccurrences(messages_.str(), "Swapchain ");
            for (int f = 0; f < 5; ++f)
                EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(ImGui::GetFrameCount(), 0);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), skippedLogs);
            window.drawable = true;
            int recorded = 0;
            size_t steadyLogs = 0;
            for (int f = 0; f < 8; ++f)
            {
                if (prepareScene)
                    recorded += drawUiFrame(window, renderer, mesh, material);
                else
                {
                    submitControlledScene(window, renderer, {}, true);
                    ++recorded;
                }
                if (f == 0)
                    steadyLogs = countOccurrences(messages_.str(), "Swapchain ");
            }
            EXPECT_GT(recorded, 0);
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), steadyLogs);
            EXPECT_TRUE(ImGui::GetIO().BackendRendererUserData && ImGui::GetIO().Fonts->IsBuilt());
            window.drawable = false;
            EXPECT_FALSE(renderer.beginUiFrame());
            EXPECT_EQ(countOccurrences(messages_.str(), "Swapchain "), steadyLogs);
        }
        FileAudit audit;
        SceneAssets assets({}, std::make_unique<AuditedFileIO>(audit));
        {
            Renderer renderer(*native, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            auto* hwnd = static_cast<GLFWwindow*>(native->getNativeHandle());
            for (int cycle = 0; cycle < 3; ++cycle)
            {
                glfwSetWindowSize(hwnd, 680 + cycle * 20, 400 + cycle * 10);
                EXPECT_GT(drawUiFrame(*native, renderer, mesh, material), 0);
                glfwIconifyWindow(hwnd);
                for (int i = 0; i < 100 && !native->isIconified(); ++i, glfwWaitEventsTimeout(0.005))
                    native->pollEvents();
                ASSERT_TRUE(native->isIconified());
                EXPECT_EQ(drawUiFrame(*native, renderer, mesh, material), 0);
                glfwRestoreWindow(hwnd);
                for (int i = 0; i < 100 && native->isIconified(); ++i, glfwWaitEventsTimeout(0.005))
                    native->pollEvents();
                ASSERT_FALSE(native->isIconified());
                int rec = 0;
                for (int f = 0; f < 8; ++f)
                    rec += drawUiFrame(*native, renderer, mesh, material);
                EXPECT_GT(rec, 0);
                EXPECT_EQ(audit.textureOpens, 1u);
            }
        }
        expectCleanValidation();
    }
    TEST_F(VulkanLifecycleTest, SwapchainResizeAndMatrixProjectionControlPresentedImage)
    {
        auto window = createWindow(true, 640, 480);
        SceneAssets assets(referenceSceneAssetPaths());
        {
            Renderer renderer(*window, RendererValidationMode::Required);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto material = renderer.createMaterial({renderer.createTexture(*assets.texture)});
            const auto mesh = createCubeMesh(renderer);
            releaseCpuAssets(assets);
            const auto extent = renderer.beginUiFrame();
            ASSERT_TRUE(extent);
            renderer.imgui().endFrame();
            const std::array draws{DrawItem{mesh, material, glm::mat4(1)}};
            const auto stale = Demo::makeSceneFrame(*extent, draws);
            glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 810, 450);
            window->pollEvents();
            int recorded = 0;
            renderer.drawFrame(stale, {0.1f, 0.2f, 0.3f, 1}, [&](void*) { ++recorded; });
            EXPECT_EQ(recorded, 0);
            const auto resizedExtent = renderer.beginUiFrame();
            ASSERT_TRUE(resizedExtent && resizedExtent->width == 810u && resizedExtent->height == 450u);
            renderer.imgui().endFrame();
            renderer.drawFrame(Demo::makeSceneFrame(*resizedExtent, draws), {0.1f, 0.2f, 0.3f, 1}, [&](void*) {
                ++recorded;
                glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 640, 480);
            });
            EXPECT_EQ(recorded, 1);

            auto present = [&](float modelX, float cameraX) {
                for (int f = 0; f < 8; ++f, glfwWaitEventsTimeout(0.025))
                {
                    window->pollEvents();
                    const auto ex = renderer.beginUiFrame();
                    if (!ex)
                        throw Error::Exception(Error::Code::RENDER, "Visible matrix review frame was unavailable");
                    std::array d{DrawItem{mesh, material, glm::translate(glm::mat4(1), {modelX, 0, 0})}};
                    auto input = Demo::makeSceneFrame(*ex, d);
                    input.camera.view = glm::lookAtRH(glm::vec3(cameraX, 0, 3), glm::vec3(cameraX, 0, 0), glm::vec3(0, 1, 0));
                    renderer.imgui().endFrame();
                    renderer.drawFrame(input, {0.2f, 0.24f, 0.28f, 1});
                    d[0].model = input.camera.projection = glm::mat4(0);
                }
                return WindowCapture::capture(*window);
            };
            const auto first = present(0, 0), stable = present(0, 0);
            const auto base = WindowCapture::bounds(first);
            ASSERT_GT(base[2] - base[0], 100);
            EXPECT_NEAR(base[2] - base[0], base[3] - base[1], 2);
            size_t changed = 0;
            for (int y = 8; y < first.height - 8; ++y)
                for (int x = 8; x < first.width - 8; ++x)
                {
                    const size_t p = (size_t(y) * first.width + x) * 4;
                    changed += (std::abs(int(first.rgba[p]) - int(stable.rgba[p])) +
                                std::abs(int(first.rgba[p + 1]) - int(stable.rgba[p + 1])) +
                                std::abs(int(first.rgba[p + 2]) - int(stable.rgba[p + 2]))) > 12;
                }
            EXPECT_LT(changed, 32u);
            EXPECT_GT(WindowCapture::bounds(present(0.5f, 0))[0] - base[0], 50);
            EXPECT_GT(base[0] - WindowCapture::bounds(present(0, 0.5f))[0], 50);
            glfwSetWindowSize(static_cast<GLFWwindow*>(window->getNativeHandle()), 800, 400);
            const auto resImg = present(0, 0);
            const auto rb = WindowCapture::bounds(resImg);
            EXPECT_EQ(resImg.width, 800);
            EXPECT_EQ(resImg.height, 400);
            EXPECT_NEAR(rb[2] - rb[0], rb[3] - rb[1], 2);
        }
        expectCleanValidation();
    }

    TEST_F(VulkanLifecycleTest, MultiObjectTransformsDepthOrderAndCountsPreserveEmptyUi)
    {
        auto native = createWindow(true, 640, 480);
        DeferredFramebufferWindow window(*native);
        window.drawable = true;
        FileAudit audit;
        SceneAssets assets(referenceSceneAssetPaths(), std::make_unique<AuditedFileIO>(audit));
        Mesh planeObs, cubeObs;
        {
            Renderer renderer(window, RendererValidationMode::Required);
            for (int f = 0; f < 4; ++f, glfwWaitEventsTimeout(0.02))
                submitControlledScene(window, renderer, {}, true);
            const auto prePrep = WindowCapture::capture(window);
            const auto px = [](const WindowCapture::Image& img, int x, int y, int c) {
                return img.rgba[(size_t(y) * img.width + x) * 4 + c];
            };
            for (int c = 0; c < 3; ++c)
                EXPECT_EQ(px(prePrep, 320, 240, c), px(prePrep, 8, 8, c));
            EXPECT_EQ(countOccurrences(messages_.str(), "Created graphics pipeline"), 0u);
            renderer.prepareSceneResources(*assets.vertex, *assets.fragment);
            const auto texture = renderer.createTexture(*assets.texture);
            const auto mat = renderer.createMaterial({texture}), frontMat = renderer.createMaterial({texture, {0.5f, 1, 0.5f}});
            auto cube = createCubeMesh(renderer);
            const auto pd = Demo::makePlaneMesh();
            auto plane = renderer.createMesh(pd.vertices, pd.indices);
            cubeObs = cube;
            planeObs = plane;
            releaseCpuAssets(assets);
            FrameExtent lastExtent{};
            const auto present = [&](std::vector<DrawItem> draws, bool withUi = true) {
                for (int f = 0; f < 2; ++f, glfwWaitEventsTimeout(0.03))
                    lastExtent = submitControlledScene(window, renderer, draws, withUi);
                draws.clear();
                return WindowCapture::capture(window);
            };
            const auto s = glm::scale(glm::mat4(1), glm::vec3(0.55f));
            std::array<int, 4> fixed{};
            for (int f = 0; f < 6; ++f)
            {
                const float y = f % 2 == 0 ? 0.25f : -0.25f;
                const auto img = present(
                    {{plane, mat, glm::translate(glm::mat4(1), {-0.6f, 0, 0}) * s},
                     {f < 4 ? cube : plane, mat,
                      glm::translate(glm::mat4(1), {0.6f, y, 0}) * glm::rotate(glm::mat4(1), 0.4f, {0, 1, 0}) * s}},
                    false);
                const auto l = WindowCapture::bounds(img, 8, img.width / 2), r = WindowCapture::bounds(img, img.width / 2, img.width - 8);
                EXPECT_NEAR((l[0] + l[2]) * 0.5f, img.width * 0.5f - 0.6f * img.height * 0.5f, 2.0f);
                EXPECT_NEAR((r[1] + r[3]) * 0.5f, img.height * (0.5f - y * 0.5f), 2.0f);
                if (f == 0)
                    fixed = l;
                else
                    EXPECT_EQ(l, fixed);
            }
            const auto ds = glm::scale(glm::mat4(1), glm::vec3(1.4f));
            const DrawItem back{plane, mat, glm::translate(glm::mat4(1), {-0.2f, 0, -0.3f}) * ds};
            const DrawItem front{plane, frontMat, glm::translate(glm::mat4(1), {0.2f, 0, 0.3f}) * ds};
            const auto frontFirst = present({front, back}), frontLast = present({back, front});
            EXPECT_GT(px(frontFirst, frontFirst.width / 2, frontFirst.height * 3 / 10, 1), 220);
            expectInteriorEqual(frontFirst, frontLast);
            std::vector<DrawItem> grid;
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    grid.push_back({plane, mat,
                                    glm::translate(glm::mat4(1), {(x - 1.5f) * 0.5f, (1.5f - y) * 0.5f, 0}) *
                                        glm::scale(glm::mat4(1), glm::vec3(0.25f))});
            const auto many = present(grid);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                {
                    const int cx = many.width / 2 + int((x - 1.5f) * many.height / 4),
                              cy = many.height / 2 + int((y - 1.5f) * many.height / 4);
                    EXPECT_GT(std::abs(int(px(many, cx, cy, 0)) - int(px(many, 8, 8, 0))) +
                                  std::abs(int(px(many, cx, cy, 1)) - int(px(many, 8, 8, 1))) +
                                  std::abs(int(px(many, cx, cy, 2)) - int(px(many, 8, 8, 2))),
                              40);
                }
            window.drawable = false;
            renderer.drawFrame(controlledFrame(lastExtent, grid), {0.2f, 0.24f, 0.28f, 1});
            grid.clear();
            cube = plane = {};
            window.drawable = true;
            const auto empty = present({});
            for (int c = 0; c < 3; ++c)
                EXPECT_EQ(px(empty, empty.width / 2, empty.height / 2, c), px(empty, 8, 8, c));
            EXPECT_GT(px(empty, 24, 24, 0), 220);
            EXPECT_EQ(audit.textureOpens, 1u);
        }
        EXPECT_FALSE(cubeObs.valid() || planeObs.valid());
        expectReleased({});
        expectCleanValidation();
    }

    TEST_F(VulkanLifecycleTest, ValidationModesMissingLayerAndImGuiSingleImageRejection)
    {
        auto window = createWindow();
        {
            for (const auto mode : {ValidationMode::Optional, ValidationMode::Required, ValidationMode::Disabled})
                EXPECT_EQ(VulkanInstance(requiredExtensions(), mode).validationEnabled(), mode != ValidationMode::Disabled);
            VulkanInstance instance(requiredExtensions(), ValidationMode::Required);
            const auto submit = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
                vkGetInstanceProcAddr(instance.handle(), "vkSubmitDebugUtilsMessageEXT"));
            ASSERT_NE(submit, nullptr);
            VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
            data.pMessageIdName = "INCUBATOR_REVIEW_INJECTION";
            data.pMessage = "Review injected warning";
            submit(instance.handle(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
            data.pMessage = "Review injected error with operation/VkResult evidence";
            submit(instance.handle(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);

            GlfwSurface surface(instance, *window);
            VulkanDevice device(instance, surface);
            Integration::ImGuiRenderer adapter(*window);
            EXPECT_THROW(adapter.initializeVulkan(instance.handle(), device.physicalDevice(), device.handle(),
                                                  device.queueFamilies().graphics, device.graphicsQueue(),
                                                  VK_FORMAT_B8G8R8A8_UNORM, device.findDepthFormat(), 1),
                         Error::Exception);
            EXPECT_EQ(ImGui::GetIO().BackendRendererUserData, nullptr);
            adapter.initializeVulkan(instance.handle(), device.physicalDevice(), device.handle(),
                                     device.queueFamilies().graphics, device.graphicsQueue(), VK_FORMAT_B8G8R8A8_UNORM,
                                     device.findDepthFormat(), 2);
            EXPECT_TRUE(ImGui::GetIO().BackendRendererUserData && ImGui::GetIO().Fonts->IsBuilt());
        }
        EXPECT_NE(messages_.str().find("warning|Review injected warning"), std::string::npos);
        EXPECT_NE(messages_.str().find("error|Review injected error with operation/VkResult evidence"),
                  std::string::npos);

        {
            EmptyLayerSearch missing;
            uint32_t count = 0;
            ASSERT_EQ(vkEnumerateInstanceLayerProperties(&count, nullptr), VK_SUCCESS);
            std::vector<VkLayerProperties> layers(count);
            ASSERT_EQ(vkEnumerateInstanceLayerProperties(&count, layers.data()), VK_SUCCESS);
            ASSERT_FALSE(std::any_of(layers.begin(), layers.end(), [](const auto& l) {
                return std::string_view(l.layerName) == "VK_LAYER_KHRONOS_validation";
            }));
            EXPECT_FALSE(VulkanInstance(requiredExtensions(), ValidationMode::Optional).validationEnabled());
            EXPECT_NE(messages_.str().find("continuing without validation"), std::string::npos);
            EXPECT_THROW(VulkanInstance(requiredExtensions(), ValidationMode::Required), Error::Exception);
            EXPECT_FALSE(VulkanInstance(requiredExtensions(), ValidationMode::Disabled).validationEnabled());
        }
    }
}  // namespace
