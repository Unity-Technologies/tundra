#include "TestHarness.hpp"
#include "Common.hpp"

#if defined(TUNDRA_WIN32)
#include <Windows.h>

TEST(Win32_LongPaths, ShortAbsolutePath_IsReferencedDirectly)
{
    wchar_t srcPath[] = L"C:\\src\\path";
    const size_t srcLength = _countof(srcPath);

    wchar_t* dstPath = nullptr;
    size_t dstLength = 0;

    ASSERT_TRUE(ConvertToLongPath(srcPath, srcLength, &dstPath, &dstLength));
    ASSERT_EQ(srcPath, dstPath);
    ASSERT_EQ(srcLength, dstLength);
}

TEST(Win32_LongPaths, ShortRelativePath_IsReferencedDirectly)
{
    wchar_t srcPath[] = L"this\\path\\is\\relative";
    const size_t srcLength = _countof(srcPath);

    wchar_t* dstPath = nullptr;
    size_t dstLength = 0;

    ASSERT_TRUE(ConvertToLongPath(srcPath, srcLength, &dstPath, &dstLength));
    ASSERT_EQ(srcPath, dstPath);
    ASSERT_EQ(srcLength, dstLength);
}

TEST(Win32_LongPaths, LongAbsolutePath_FailsWithRequestForCorrectBufferSize_ThenWorksWithExtendedPrefix)
{
    wchar_t srcPath[] = L"C:\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz";
    const size_t srcLength = _countof(srcPath);
    ASSERT_GT(srcLength, MAX_PATH);

    wchar_t* dstPath = nullptr;
    size_t dstLength = 0;

    ASSERT_FALSE(ConvertToLongPath(srcPath, srcLength, &dstPath, &dstLength));
    ASSERT_EQ(dstPath, nullptr);
    ASSERT_GT(dstLength, srcLength);

    dstPath = static_cast<wchar_t*>(_alloca(sizeof(wchar_t) * dstLength));
    ASSERT_TRUE(ConvertToLongPath(srcPath, srcLength, &dstPath, &dstLength));
    ASSERT_STREQ(dstPath, L"\\\\?\\C:\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz");
    ASSERT_EQ(dstLength, wcslen(dstPath));
}

TEST(Win32_LongPaths, LongUNCPath_FailsWithRequestForCorrectBufferSize_ThenWorksWithExtendedUNCPrefix)
{
    wchar_t srcPath[] = L"\\\\MYMACHINE\\C\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz";
    const size_t srcLength = _countof(srcPath);
    ASSERT_GT(srcLength, MAX_PATH);

    wchar_t* dstPath = nullptr;
    size_t dstLength = 0;

    ASSERT_FALSE(ConvertToLongPath(srcPath, srcLength, &dstPath, &dstLength));
    ASSERT_EQ(dstPath, nullptr);
    ASSERT_GT(dstLength, srcLength);

    dstPath = static_cast<wchar_t*>(_alloca(sizeof(wchar_t) * dstLength));
    ASSERT_TRUE(ConvertToLongPath(srcPath, srcLength, &dstPath, &dstLength));
    ASSERT_STREQ(dstPath, L"\\\\?\\UNC\\MYMACHINE\\C\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz");
    ASSERT_EQ(dstLength, wcslen(dstPath));
}

#endif
