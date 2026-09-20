#include "legacy_rva_client.h"
#include "legacy_dm_x64.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <system_error>

namespace {

int g_failures = 0;
int g_passes = 0;

void pass(const char *name) {
    ++g_passes;
    std::printf("[PASS] %s\n", name);
}

void fail(const char *name, const std::string &legacy, const std::string &recovered) {
    ++g_failures;
    std::printf("[FAIL] %s\n  legacy   = %s\n  recovered= %s\n",
                name, legacy.c_str(), recovered.c_str());
}

template<class A, class B>
void eq_num(const char *name, A oldv, B newv) {
    if (oldv == newv) pass(name);
    else fail(name, std::to_string(oldv), std::to_string(newv));
}

void eq_str(const char *name, const std::string &oldv, const std::string &newv) {
    if (oldv == newv) pass(name);
    else fail(name, oldv, newv);
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

void test_pure(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    eq_num("Is64Bit", old_dm.Is64Bit(), new_dm.Is64Bit());

    const long h32[] = {
        0, 1, -1, 0x12345678,
        std::numeric_limits<long>::min(),
        std::numeric_limits<long>::max()
    };
    for (long v : h32) {
        const std::string a = old_dm.Hex32(v) ? old_dm.Hex32(v) : "<null>";
        const std::string b = new_dm.Hex32(v) ? new_dm.Hex32(v) : "<null>";
        eq_str("Hex32", a, b);
    }

    const LONGLONG h64[] = {
        0, 1, -1,
        static_cast<LONGLONG>(0x123456789abcdef0ULL),
        std::numeric_limits<LONGLONG>::min(),
        std::numeric_limits<LONGLONG>::max()
    };
    for (LONGLONG v : h64) {
        const char *oa = old_dm.Hex64(v);
        const char *nb = new_dm.Hex64(v);
        eq_str("Hex64", oa ? std::string(oa) : "<null>", nb ? std::string(nb) : "<null>");
    }

    for (const char *v : {"000000", "112233", "abcdef", "ABCDEF", "bad", ""}) {
        const char *oa = old_dm.RGB2BGR(v);
        const char *nb = new_dm.RGB2BGR(v);
        eq_str("RGB2BGR", oa ? std::string(oa) : "<null>", nb ? std::string(nb) : "<null>");

        oa = old_dm.BGR2RGB(v);
        nb = new_dm.BGR2RGB(v);
        eq_str("BGR2RGB", oa ? std::string(oa) : "<null>", nb ? std::string(nb) : "<null>");
    }

    struct C { const char *s; const char *needle; };
    const C cases[] = {
        {"abcdef", "cd"}, {"abcdef", "xx"}, {"", ""},
        {"abc", ""}, {"aaaa", "aa"}, {"abcabc", "abc"}
    };
    for (const auto &c : cases)
        eq_num("StrStr", old_dm.StrStr(c.s, c.needle), new_dm.StrStr(c.s, c.needle));
}

void test_system(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    eq_num("GetScreenWidth", old_dm.GetScreenWidth(), new_dm.GetScreenWidth());
    eq_num("GetScreenHeight", old_dm.GetScreenHeight(), new_dm.GetScreenHeight());
    eq_num("GetScreenDepth", old_dm.GetScreenDepth(), new_dm.GetScreenDepth());
    eq_num("GetDPI", old_dm.GetDPI(), new_dm.GetDPI());
    eq_num("GetOsBuildNumber", old_dm.GetOsBuildNumber(), new_dm.GetOsBuildNumber());
    eq_num("CheckFontSmooth", old_dm.CheckFontSmooth(), new_dm.CheckFontSmooth());
}

void test_env(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    eq_num("SetEnv", old_dm.SetEnv(7, "parity", "value"), new_dm.SetEnv(7, "parity", "value"));

    const char *oa = old_dm.GetEnv(7, "parity");
    const char *nb = new_dm.GetEnv(7, "parity");
    eq_str("GetEnv", oa ? std::string(oa) : "<null>", nb ? std::string(nb) : "<null>");

    eq_num("DelEnv", old_dm.DelEnv(7, "parity"), new_dm.DelEnv(7, "parity"));

    oa = old_dm.GetEnv(7, "parity");
    nb = new_dm.GetEnv(7, "parity");
    eq_str("GetEnv-after-delete", oa ? std::string(oa) : "<null>", nb ? std::string(nb) : "<null>");
}

void test_file_ini(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const auto root = make_root();
    const auto old_dir = root / "legacy_dir";
    const auto new_dir = root / "recovered_dir";
    const auto old_file = root / "legacy.txt";
    const auto new_file = root / "recovered.txt";
    const auto old_copy = root / "legacy_copy.txt";
    const auto new_copy = root / "recovered_copy.txt";
    const auto old_move = root / "legacy_move.txt";
    const auto new_move = root / "recovered_move.txt";
    const auto old_ini = root / "legacy.ini";
    const auto new_ini = root / "recovered.ini";

    eq_num("CreateFolder",
           old_dm.CreateFolder(old_dir.string().c_str()),
           new_dm.CreateFolder(new_dir.string().c_str()));
    eq_num("IsFolderExist",
           old_dm.IsFolderExist(old_dir.string().c_str()),
           new_dm.IsFolderExist(new_dir.string().c_str()));

    eq_num("WriteFile",
           old_dm.WriteFile(old_file.string().c_str(), "abc123"),
           new_dm.WriteFile(new_file.string().c_str(), "abc123"));
    eq_num("IsFileExist",
           old_dm.IsFileExist(old_file.string().c_str()),
           new_dm.IsFileExist(new_file.string().c_str()));
    eq_num("GetFileLength",
           old_dm.GetFileLength(old_file.string().c_str()),
           new_dm.GetFileLength(new_file.string().c_str()));

    {
        const char *a = old_dm.ReadFile(old_file.string().c_str());
        const char *b = new_dm.ReadFile(new_file.string().c_str());
        eq_str("ReadFile", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.ReadFileData(old_file.string().c_str(), 1, 3);
        const char *b = new_dm.ReadFileData(new_file.string().c_str(), 1, 3);
        eq_str("ReadFileData", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }

    eq_num("CopyFile",
           old_dm.CopyFile(old_file.string().c_str(), old_copy.string().c_str(), 1),
           new_dm.CopyFile(new_file.string().c_str(), new_copy.string().c_str(), 1));
    eq_num("MoveFile",
           old_dm.MoveFile(old_copy.string().c_str(), old_move.string().c_str()),
           new_dm.MoveFile(new_copy.string().c_str(), new_move.string().c_str()));

    eq_num("WriteIni",
           old_dm.WriteIni("section", "key", "value", old_ini.string().c_str()),
           new_dm.WriteIni("section", "key", "value", new_ini.string().c_str()));
    {
        const char *a = old_dm.ReadIni("section", "key", old_ini.string().c_str());
        const char *b = new_dm.ReadIni("section", "key", new_ini.string().c_str());
        eq_str("ReadIni", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.EnumIniKey("section", old_ini.string().c_str());
        const char *b = new_dm.EnumIniKey("section", new_ini.string().c_str());
        eq_str("EnumIniKey", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.EnumIniSection(old_ini.string().c_str());
        const char *b = new_dm.EnumIniSection(new_ini.string().c_str());
        eq_str("EnumIniSection", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    eq_num("DeleteIni",
           old_dm.DeleteIni("section", "key", old_ini.string().c_str()),
           new_dm.DeleteIni("section", "key", new_ini.string().c_str()));

    eq_num("DeleteFile",
           old_dm.DeleteFile(old_move.string().c_str()),
           new_dm.DeleteFile(new_move.string().c_str()));
    eq_num("DeleteFolder",
           old_dm.DeleteFolder(old_dir.string().c_str()),
           new_dm.DeleteFolder(new_dir.string().c_str()));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main(int argc, char **argv) {
#if defined(_WIN64)
    std::puts("Parity probe must be built/run as Win32 because the reference DLL is x86.");
    return 200;
#else
    const char *legacy_path = argc > 1 ? argv[1] : "hcbyj.dll";

    try {
        LegacyRvaClient old_dm(legacy_path);
        LoadDm(nullptr);
        dmsoft new_dm;

        test_pure(old_dm, new_dm);
        test_system(old_dm, new_dm);
        test_env(old_dm, new_dm);
        test_file_ini(old_dm, new_dm);

        FreeDm();

        std::printf("\nSUMMARY passes=%d failures=%d\n", g_passes, g_failures);
        return g_failures == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::printf("PARITY PROBE ERROR: %s\n", e.what());
        return 201;
    }
#endif
}
