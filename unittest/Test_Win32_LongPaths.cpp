#include "TestHarness.hpp"
#include "Common.hpp"

#if defined(TUNDRA_WIN32)
#include <Windows.h>


TEST(Win32_LongPaths, ShortRelativePath_IsReferencedDirectly)
{
    wchar_t srcPath[] = L"this\\path\\is\\relative";

    wchar_t buf[MAX_PATH];
    const size_t srcLength = ::GetFullPathNameW(srcPath, MAX_PATH, buf, nullptr);

    std::wstring dstPath = srcPath;

    ASSERT_TRUE(ConvertToLongPath(&dstPath));
    ASSERT_EQ(srcLength, dstPath.length());
    ASSERT_STREQ(buf, dstPath.c_str());
}

TEST(Win32_LongPaths, Null_Path_Fail)
{
    ASSERT_FALSE(ConvertToLongPath(nullptr));
}

TEST(Win32_LongPaths, Zero_Length)
{
    std::wstring dstPath;

    ASSERT_TRUE(ConvertToLongPath(&dstPath));
    ASSERT_EQ(0, dstPath.length());
}

TEST(Win32_LongPaths, LongRelativePath_Resolved)
{
    wchar_t srcPath[] = L"C:\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\..\\abcdefghijklmnopqrstuvwxyz\\..\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz";
    wchar_t resultPath[] = L"\\\\?\\C:\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz";

    const size_t srcLength = ::GetFullPathNameW(srcPath, 0, nullptr, nullptr);
    ASSERT_GT(srcLength, MAX_PATH);

    std::wstring dstPath = srcPath;

    ASSERT_TRUE(ConvertToLongPath(&dstPath));
    ASSERT_EQ(wcslen(resultPath), dstPath.length());
    ASSERT_STREQ(dstPath.c_str(), resultPath);
}

TEST(Win32_LongPaths, LongRelativePath_CreateDirectoryWSize)
{
    wchar_t srcPath[] = L"C:\\longs\\paths\\AppData\\Local\\Temp\\BeeTest\\BackendTests_Tundra.OutputWithLongPath_IsNotReb-n511prpy\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername";
    wchar_t resultPath[] = L"\\\\?\\C:\\longs\\paths\\AppData\\Local\\Temp\\BeeTest\\BackendTests_Tundra.OutputWithLongPath_IsNotReb-n511prpy\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername\\15charactername";

    const size_t srcLength = ::GetFullPathNameW(srcPath, 0, nullptr, nullptr);
    ASSERT_LT(srcLength, MAX_PATH);

    std::wstring dstPath = srcPath;

    ASSERT_TRUE(ConvertToLongPath(&dstPath));
    ASSERT_EQ(wcslen(resultPath), dstPath.length());
    ASSERT_STREQ(dstPath.c_str(), resultPath);
}

TEST(Win32_LongPaths, LongAbsolutePath_WithExtendedPrefix)
{
    wchar_t srcPath[] = L"C:\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz";
    const size_t srcLength = _countof(srcPath);
    ASSERT_GT(srcLength, MAX_PATH);

    std::wstring dstPath = srcPath;

    ASSERT_TRUE(ConvertToLongPath(&dstPath));
    ASSERT_GT(dstPath.length(), srcLength);
    ASSERT_STREQ(dstPath.c_str(), L"\\\\?\\C:\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz");
}

TEST(Win32_LongPaths, LongUNCPath_WithExtendedUNCPrefix)
{
    wchar_t srcPath[] = L"\\\\MYMACHINE\\C\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz";
    const size_t srcLength = _countof(srcPath);
    ASSERT_GT(srcLength, MAX_PATH);

    std::wstring dstPath = srcPath;

    ASSERT_TRUE(ConvertToLongPath(&dstPath));
    ASSERT_GT(dstPath.length(), srcLength);
    ASSERT_STREQ(dstPath.c_str(), L"\\\\?\\UNC\\MYMACHINE\\C\\long\\path\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz\\abcdefghijklmnopqrstuvwxyz");
}

#endif
