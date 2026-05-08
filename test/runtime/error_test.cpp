#include "runtime/core/error/error.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <source_location>
#include <string>

using Incubator::Error::Code;
using Incubator::Error::Exception;
using Incubator::Error::toString;

TEST(ErrorTest, ToStringMapsEnum)
{
    EXPECT_EQ(toString(Code::UNKNOWN), "UNKNOWN");
    EXPECT_EQ(toString(Code::IO), "IO");
    EXPECT_EQ(toString(Code::CONFIG), "CONFIG");
    EXPECT_EQ(toString(Code::NETWORK), "NETWORK");
}

TEST(ErrorTest, ExceptionWhatContainsDetails)
{
    const int line = __LINE__ + 1;
    Exception ex(Code::CONFIG, "broken config", std::source_location::current());

    const std::string what = ex.what();

    EXPECT_NE(what.find("CONFIG"), std::string::npos);
    EXPECT_NE(what.find("broken config"), std::string::npos);
    EXPECT_NE(what.find(std::to_string(line)), std::string::npos);
    // Exception 内部使用 std::filesystem::path::filename() 提取文件名
    std::string filename = std::filesystem::path(__FILE__).filename().string();
    EXPECT_NE(what.find(filename), std::string::npos);
}
