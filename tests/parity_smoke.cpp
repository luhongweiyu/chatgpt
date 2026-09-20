#include "../reference/reference_legacy.h"

// The old and recovered headers intentionally used the same historical guard.
#ifdef __INCLUDE_OBJ_H__
#undef __INCLUDE_OBJ_H__
#endif
#include "legacy_dm_x64.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void fail(const char *name, const std::string &detail) {
    ++g_failures;
    std::printf("[FAIL] %s: %s\n", name, detail.c_str());
}

void pass(const char *name) {
    std::printf("[PASS] %s\n", name);
}

template <class A, class B>
void eq(const char *name, const A &a, const B &b) {
    if (a == b) pass(name);
    else fail(name, "values differ");
}

void eq_str(const char *name, const char *a, const char *b) {
    const std::string sa = a ? a : "<null>";
    const std::string sb = b ? b : "<null>";
    if (sa == sb) pass(name);
    else fail(name, "legacy='" + sa + "' recovered='" + sb + "'");
}

std::filesystem::path make_root() {
    char temp[MAX_PATH]{};
    ::GetTempPathA(MAX_PATH, temp);
    auto root = std::filesystem::path(temp) / "hcbyj_parity";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

void test_pure(legacy_dmsoft &old_dm, dmsoft &new_dm) {
    const long hex32_values[] = {0, 1, -1, 0x12345678, std::numeric_limits<long>::min(), std::numeric_limits<long>::max()};
    for (long v : hex32_values) {
        const std::string oldv = old_dm.Hex32(v);
        const std::string newv = new_dm.Hex32(v);
        eq_str("Hex32", oldv.c_str(), newv.c_str());
    }

    const LONGLONG hex64_values[] = {
        0, 1, -1,
        static_cast<LONGLONG>(0x123456789abcdef0ULL),
        std::numeric_limits<LONGLONG>::min(),
        std::numeric_limits<LONGLONG>::max()
    };
    for (LONGLONG v : hex64_values) {
        const std::string oldv = old_dm.Hex64(v);
        const std::string newv = new_dm.Hex64(v);
        eq_str("Hex64", oldv.c_str(), newv.c_str());
    }

    for (const char *v : {"000000", "112233", "abcdef", "ABCDEF", "bad"}) {
        const std::string old_rgb = old_dm.RGB2BGR(v);
        const std::string new_rgb = new_dm.RGB2BGR(v);
        eq_str("RGB2BGR", old_rgb.c_str(), new_rgb.c_str());

        const std::string old_bgr = old_dm.BGR2RGB(v);
        const std::string new_bgr = new_dm.BGR2RGB(v);
        eq_str("BGR2RGB", old_bgr.c_str(), new_bgr.c_str());
    }

    struct StrCase { const char *s; const char *needle; };
    const StrCase cases[] = {
        {"abcdef", "cd"},
        {"abcdef", "xx"},
        {"", ""},
        {"abc", ""},
        {"aaaa", "aa"},
    };
    for (const auto &c : cases)
        eq("StrStr", old_dm.StrStr(c.s, c.needle), new_dm.StrStr(c.s, c.needle));
}

void test_file_ini(legacy_dmsoft &old_dm, dmsoft &new_dm) {
    const auto root = make_root();
    const auto d1 = root / "dir1";
    const auto d2 = root / "dir2";
    const auto f1 = root / "a.bin";
    const auto f2 = root / "b.bin";
    const auto ini = root / "test.ini";

    eq("CreateFolder", old_dm.CreateFolder(d1.string().c_str()), new_dm.CreateFolder(d2.string().c_str()));
    eq("IsFolderExist", old_dm.IsFolderExist(d1.string().c_str()), new_dm.IsFolderExist(d2.string().c_str()));

    // Use separate files so both implementations observe identical initial state.
    const auto old_file = root / "old.txt";
    const auto new_file = root / "new.txt";
    eq("WriteFile", old_dm.WriteFile(old_file.string().c_str(), "abc123"), new_dm.WriteFile(new_file.string().c_str(), "abc123"));
    eq("IsFileExist", old_dm.IsFileExist(old_file.string().c_str()), new_dm.IsFileExist(new_file.string().c_str()));
    eq("GetFileLength", old_dm.GetFileLength(old_file.string().c_str()), new_dm.GetFileLength(new_file.string().c_str()));

    const std::string old_read = old_dm.ReadFile(old_file.string().c_str());
    const std::string new_read = new_dm.ReadFile(new_file.string().c_str());
    eq_str("ReadFile", old_read.c_str(), new_read.c_str());

    const std::string old_slice = old_dm.ReadFileData(old_file.string().c_str(), 1, 3);
    const std::string new_slice = new_dm.ReadFileData(new_file.string().c_str(), 1, 3);
    eq_str("ReadFileData", old_slice.c_str(), new_slice.c_str());

    const auto old_ini = root / "old.ini";
    const auto new_ini = root / "new.ini";
    eq("WriteIni", old_dm.WriteIni("s", "k", "value", old_ini.string().c_str()),
                   new_dm.WriteIni("s", "k", "value", new_ini.string().c_str()));
    const std::string old_ini_v = old_dm.ReadIni("s", "k", old_ini.string().c_str());
    const std::string new_ini_v = new_dm.ReadIni("s", "k", new_ini.string().c_str());
    eq_str("ReadIni", old_ini_v.c_str(), new_ini_v.c_str());

    const std::string old_keys = old_dm.EnumIniKey("s", old_ini.string().c_str());
    const std::string new_keys = new_dm.EnumIniKey("s", new_ini.string().c_str());
    eq_str("EnumIniKey", old_keys.c_str(), new_keys.c_str());

    const std::string old_sections = old_dm.EnumIniSection(old_ini.string().c_str());
    const std::string new_sections = new_dm.EnumIniSection(new_ini.string().c_str());
    eq_str("EnumIniSection", old_sections.c_str(), new_sections.c_str());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

void test_system(legacy_dmsoft &old_dm, dmsoft &new_dm) {
    eq("GetScreenWidth", old_dm.GetScreenWidth(), new_dm.GetScreenWidth());
    eq("GetScreenHeight", old_dm.GetScreenHeight(), new_dm.GetScreenHeight());
    eq("GetScreenDepth", old_dm.GetScreenDepth(), new_dm.GetScreenDepth());
    eq("GetDPI", old_dm.GetDPI(), new_dm.GetDPI());
    eq("CheckFontSmooth", old_dm.CheckFontSmooth(), new_dm.CheckFontSmooth());
    eq("GetOsBuildNumber", old_dm.GetOsBuildNumber(), new_dm.GetOsBuildNumber());
}

} // namespace

int main() {
    const auto dll = std::filesystem::absolute("reference/private/hcbyj.dll");
    if (!LegacyLoadDm(dll.string().c_str())) {
        std::printf("Cannot load reference DLL: %s\n", dll.string().c_str());
        return 100;
    }

    // Native recovered methods do not need an external backend.
    LoadDm(nullptr);

    legacy_dmsoft old_dm;
    dmsoft new_dm;
    if (!old_dm.IsValid()) {
        std::puts("Reference dmsoft object is invalid.");
        return 101;
    }

    test_pure(old_dm, new_dm);
    test_file_ini(old_dm, new_dm);
    test_system(old_dm, new_dm);

    LegacyFreeDm();
    FreeDm();

    std::printf("\nParity failures: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
