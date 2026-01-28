#include "runtime/core/error/error.h"

#include <gtest/gtest.h>

#include <source_location>
#include <string>

// 错误处理单元测试：覆盖枚举字符串化、文件名提取以及异常内容。
using Incubator::ErrorCode;
using Incubator::Exception;
using Incubator::getFileName;
using Incubator::toString;

// 验证错误码枚举到字符串的映射
TEST(ErrorTest, ToStringMapsEnum)
{
    EXPECT_EQ(toString(ErrorCode::UNKNOWN), "UNKNOWN");
    EXPECT_EQ(toString(ErrorCode::IO), "IO");
    EXPECT_EQ(toString(ErrorCode::CONFIG), "CONFIG");
    EXPECT_EQ(toString(ErrorCode::NETWORK), "NETWORK");
}

// 验证路径分隔符兼容性
TEST(ErrorTest, GetFileNameHandlesSeparators)
{
    EXPECT_EQ(getFileName("/path/to/file.txt"), "file.txt");
    EXPECT_EQ(getFileName("C:\\path\\to\\file.txt"), "file.txt");
    EXPECT_EQ(getFileName("file.txt"), "file.txt");
}

// 验证异常what包含上下文信息
TEST(ErrorTest, ExceptionWhatContainsDetails)
{
    const int line = __LINE__ + 1;
    Exception ex(ErrorCode::CONFIG, "broken config", std::source_location::current());

    const std::string what = ex.what();

    EXPECT_NE(what.find("CONFIG"), std::string::npos);
    EXPECT_NE(what.find("broken config"), std::string::npos);
    EXPECT_NE(what.find(std::to_string(line)), std::string::npos);
    EXPECT_NE(what.find(std::string(getFileName(__FILE__))), std::string::npos);
}
