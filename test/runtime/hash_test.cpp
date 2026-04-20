#include <gtest/gtest.h>
#include "runtime/core/utils/hash.h"

using namespace Incubator::Utils;

TEST(HashTest, Hash32Basic)
{
    uint32_t h1 = hash32("hello");
    uint32_t h2 = hash32("hello");

    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, hash32("world"));
}

TEST(HashTest, Hash32Data)
{
    std::string data = "test data";
    uint32_t h1 = hash32(data.data(), data.size());
    uint32_t h2 = hash32("test data");

    EXPECT_EQ(h1, h2);
}

TEST(HashTest, Hash64Basic)
{
    uint64_t h1 = hash64("hello");
    uint64_t h2 = hash64("hello");

    EXPECT_EQ(h1, h2);
    EXPECT_NE(h1, hash64("world"));
}

TEST(HashTest, Hash64Data)
{
    std::string data = "test data";
    uint64_t h1 = hash64(data.data(), data.size());
    uint64_t h2 = hash64("test data");

    EXPECT_EQ(h1, h2);
}

TEST(HashTest, ConstHash32)
{
    constexpr uint32_t h1 = constHash32("hello");
    uint32_t h2 = hash32("hello");

    EXPECT_EQ(h1, h2);
}

TEST(HashTest, ConstHash32Different)
{
    constexpr uint32_t h1 = constHash32("hello");
    constexpr uint32_t h2 = constHash32("world");

    EXPECT_NE(h1, h2);
}

TEST(HashTest, HashCombine)
{
    size_t seed = 0;
    hashCombine(seed, 42);
    hashCombine(seed, "test");

    size_t seed2 = 0;
    hashCombine(seed2, 42);
    hashCombine(seed2, "test");

    EXPECT_EQ(seed, seed2);
}

TEST(HashTest, HashCombineDifferent)
{
    size_t seed = 0;
    hashCombine(seed, 42);
    hashCombine(seed, "test");

    size_t seed2 = 0;
    hashCombine(seed2, 42);
    hashCombine(seed2, "different");

    EXPECT_NE(seed, seed2);
}

TEST(HashTest, Hash64ConsistentWithHash32Different)
{
    uint32_t h32 = hash32("test string");
    uint64_t h64 = hash64("test string");

    EXPECT_NE(static_cast<uint64_t>(h32), h64);
}
