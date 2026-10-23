#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "runtime/core/error/error.h"
#include "runtime/resource/loader/texture_resource.h"

namespace
{
    using namespace Incubator;
    using namespace Incubator::Resource;

    TEST(TextureResourceTest, OwnsImmutableRgbaPayloadWithTextureIdentity)
    {
        const auto id = ResourceId::fromPath(ResourceType::TEXTURE, "textures/pixel.png");
        std::vector<uint8_t> input = {17, 34, 51, 0};
        TextureResource texture(id, 1, 1, input);
        input[0] = 255;
        EXPECT_EQ(texture.id(), id);
        EXPECT_EQ(texture.type(), ResourceType::TEXTURE);
        EXPECT_EQ(texture.state(), ResourceState::UNLOADED);
        EXPECT_EQ(texture.width(), 1u);
        EXPECT_EQ(texture.height(), 1u);
        EXPECT_EQ(texture.pixels(), (std::vector<uint8_t>{17, 34, 51, 0}));
    }

    TEST(TextureResourceTest, RejectsInvalidDimensionsBeforeSizeMultiplication)
    {
        const auto id = ResourceId::fromPath(ResourceType::TEXTURE, "textures/pixel.png");
        EXPECT_THROW(TextureResource(id, 0, 1, {}), Error::Exception);
        EXPECT_THROW(TextureResource(id, 1, 0, {}), Error::Exception);
        EXPECT_THROW(TextureResource(id, 4097, 1, {}), Error::Exception);
        EXPECT_THROW(TextureResource(id, 1, 4097, {}), Error::Exception);
        EXPECT_THROW(TextureResource(id, std::numeric_limits<uint32_t>::max(),
                                     std::numeric_limits<uint32_t>::max(), {}), Error::Exception);
    }

    TEST(TextureResourceTest, RejectsPayloadLengthMismatch)
    {
        const auto id = ResourceId::fromPath(ResourceType::TEXTURE, "textures/pixel.png");
        EXPECT_THROW(TextureResource(id, 1, 1, {0, 0, 0}), Error::Exception);
        EXPECT_THROW(TextureResource(id, 1, 1, {0, 0, 0, 0, 0}), Error::Exception);
        // 最大合法尺寸需要 64 MiB；使用空 vector 检查
        // 该不变量，避免实际分配这段数据。
        EXPECT_THROW(TextureResource(id, 4096, 4096, {}), Error::Exception);
    }
}  // namespace
