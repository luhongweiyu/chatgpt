#include "legacy_rva_client.h"
#include "legacy_dm_x64.h"

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
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

    for (long type = -1; type <= 5; ++type) {
        const char *a = old_dm.GetDir(type);
        const char *b = new_dm.GetDir(type);
        eq_str(("GetDir-type" + std::to_string(type)).c_str(),
               a ? std::string(a) : "<null>",
               b ? std::string(b) : "<null>");
    }
    eq_num("GetOsType", old_dm.GetOsType(), new_dm.GetOsType());

    eq_num("CheckFontSmooth", old_dm.CheckFontSmooth(), new_dm.CheckFontSmooth());
    eq_num("GetKeyState", old_dm.GetKeyState(VK_F24), new_dm.GetKeyState(VK_F24));

    const long old_speed = old_dm.GetMouseSpeed();
    const long new_speed = new_dm.GetMouseSpeed();
    eq_num("GetMouseSpeed", old_speed, new_speed);
    if (old_speed >= 1 && old_speed <= 11 && new_speed >= 1 && new_speed <= 11) {
        eq_num("SetMouseSpeed-current",
               old_dm.SetMouseSpeed(old_speed),
               new_dm.SetMouseSpeed(new_speed));
    }
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


void test_memory(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const long pid = static_cast<long>(::GetCurrentProcessId());
    eq_num("SetMemoryHwndAsProcessId",
           old_dm.SetMemoryHwndAsProcessId(1),
           new_dm.SetMemoryHwndAsProcessId(1));

    std::int32_t old_i = 0x12345678;
    std::int32_t new_i = 0x12345678;
    eq_num("ReadIntAddr",
           old_dm.ReadIntAddr(pid, reinterpret_cast<LONGLONG>(&old_i), 0),
           new_dm.ReadIntAddr(pid, reinterpret_cast<LONGLONG>(&new_i), 0));

    const long old_wi = old_dm.WriteIntAddr(pid, reinterpret_cast<LONGLONG>(&old_i), 0, 0x13572468);
    const long new_wi = new_dm.WriteIntAddr(pid, reinterpret_cast<LONGLONG>(&new_i), 0, 0x13572468);
    eq_num("WriteIntAddr-ret", old_wi, new_wi);
    eq_num("WriteIntAddr-value", old_i, new_i);

    float old_f = 12.25f, new_f = 12.25f;
    eq_num("ReadFloatAddr", old_dm.ReadFloatAddr(pid, reinterpret_cast<LONGLONG>(&old_f)),
                            new_dm.ReadFloatAddr(pid, reinterpret_cast<LONGLONG>(&new_f)));
    eq_num("WriteFloatAddr-ret",
           old_dm.WriteFloatAddr(pid, reinterpret_cast<LONGLONG>(&old_f), -3.5f),
           new_dm.WriteFloatAddr(pid, reinterpret_cast<LONGLONG>(&new_f), -3.5f));
    eq_num("WriteFloatAddr-value", old_f, new_f);

    double old_d = 1234.5, new_d = 1234.5;
    eq_num("ReadDoubleAddr", old_dm.ReadDoubleAddr(pid, reinterpret_cast<LONGLONG>(&old_d)),
                             new_dm.ReadDoubleAddr(pid, reinterpret_cast<LONGLONG>(&new_d)));
    eq_num("WriteDoubleAddr-ret",
           old_dm.WriteDoubleAddr(pid, reinterpret_cast<LONGLONG>(&old_d), -88.125),
           new_dm.WriteDoubleAddr(pid, reinterpret_cast<LONGLONG>(&new_d), -88.125));
    eq_num("WriteDoubleAddr-value", old_d, new_d);

    unsigned char old_bytes[6] = {0x00,0x11,0x7f,0x80,0xfe,0xff};
    unsigned char new_bytes[6] = {0x00,0x11,0x7f,0x80,0xfe,0xff};
    {
        const char *a = old_dm.ReadDataAddr(pid, reinterpret_cast<LONGLONG>(old_bytes), 6);
        const char *b = new_dm.ReadDataAddr(pid, reinterpret_cast<LONGLONG>(new_bytes), 6);
        eq_str("ReadDataAddr", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    eq_num("WriteDataAddr-ret",
           old_dm.WriteDataAddr(pid, reinterpret_cast<LONGLONG>(old_bytes), "12 34 56 78 9a bc"),
           new_dm.WriteDataAddr(pid, reinterpret_cast<LONGLONG>(new_bytes), "12 34 56 78 9a bc"));
    eq_num("WriteDataAddr-bytes", std::memcmp(old_bytes, new_bytes, sizeof(old_bytes)), 0);

    char exe[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const char *base = std::strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    eq_num("GetModuleBaseAddr", old_dm.GetModuleBaseAddr(pid, base), new_dm.GetModuleBaseAddr(pid, base));
    eq_num("GetModuleSize", old_dm.GetModuleSize(pid, base), new_dm.GetModuleSize(pid, base));

    {
        const long old_handle = old_dm.OpenProcess(pid);
        const long new_handle = new_dm.OpenProcess(pid);
        eq_num("OpenProcess-success", old_handle != 0 ? 1 : 0, new_handle != 0 ? 1 : 0);
        if (old_handle) ::CloseHandle(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(old_handle)));
        if (new_handle) ::CloseHandle(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(new_handle)));
    }

    void *old_page = ::VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    void *new_page = ::VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (old_page && new_page) {
        const long old_previous = old_dm.VirtualProtectEx(
            pid, reinterpret_cast<LONGLONG>(old_page), 4096, 0, 0);
        const long new_previous = new_dm.VirtualProtectEx(
            pid, reinterpret_cast<LONGLONG>(new_page), 4096, 0, 0);
        eq_num("VirtualProtectEx-old-protect", old_previous, new_previous);

        const long old_restore = old_dm.VirtualProtectEx(
            pid, reinterpret_cast<LONGLONG>(old_page), 4096, 1, old_previous);
        const long new_restore = new_dm.VirtualProtectEx(
            pid, reinterpret_cast<LONGLONG>(new_page), 4096, 1, new_previous);
        eq_num("VirtualProtectEx-restore-return", old_restore, new_restore);
    }

    struct MBI32Probe {
        DWORD BaseAddress;
        DWORD AllocationBase;
        DWORD AllocationProtect;
        DWORD RegionSize;
        DWORD State;
        DWORD Protect;
        DWORD Type;
    };
    if (old_page) {
        MBI32Probe old_mbi{}, new_mbi{};
        const char *a = old_dm.VirtualQueryEx(
            pid, reinterpret_cast<LONGLONG>(old_page),
            static_cast<long>(reinterpret_cast<INT_PTR>(&old_mbi)));
        const std::string old_query = a ? a : "<null>";
        const char *b = new_dm.VirtualQueryEx(
            pid, reinterpret_cast<LONGLONG>(old_page),
            static_cast<long>(reinterpret_cast<INT_PTR>(&new_mbi)));
        const std::string new_query = b ? b : "<null>";
        eq_str("VirtualQueryEx-string", old_query, new_query);
        eq_num("VirtualQueryEx-struct",
               std::memcmp(&old_mbi, &new_mbi, sizeof(old_mbi)), 0);
    }

    eq_num("FreeProcessMemory",
           old_dm.FreeProcessMemory(pid),
           new_dm.FreeProcessMemory(pid));


    auto hex_addr = [](const void *ptr) {
        char buf[32]{};
        std::snprintf(buf, sizeof(buf), "%llX",
            static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(ptr)));
        return std::string(buf);
    };

    std::int32_t old_expr_i = 0x24681357;
    std::int32_t new_expr_i = 0x24681357;
    const std::string old_expr_addr = hex_addr(&old_expr_i);
    const std::string new_expr_addr = hex_addr(&new_expr_i);
    eq_num("ReadInt-expr-absolute",
           old_dm.ReadInt(pid, old_expr_addr.c_str(), 0),
           new_dm.ReadInt(pid, new_expr_addr.c_str(), 0));
    eq_num("WriteInt-expr-ret",
           old_dm.WriteInt(pid, old_expr_addr.c_str(), 0, 0x11223344),
           new_dm.WriteInt(pid, new_expr_addr.c_str(), 0, 0x11223344));
    eq_num("WriteInt-expr-value", old_expr_i, new_expr_i);

    std::int32_t old_pointer_value = 0x55667711;
    std::int32_t new_pointer_value = 0x55667711;
    ULONG_PTR old_p1 = reinterpret_cast<ULONG_PTR>(&old_pointer_value);
    ULONG_PTR new_p1 = reinterpret_cast<ULONG_PTR>(&new_pointer_value);
    ULONG_PTR old_p2 = reinterpret_cast<ULONG_PTR>(&old_p1);
    ULONG_PTR new_p2 = reinterpret_cast<ULONG_PTR>(&new_p1);

    const std::string old_one = "[" + hex_addr(&old_p1) + "]";
    const std::string new_one = "[" + hex_addr(&new_p1) + "]";
    eq_num("ReadInt-expr-pointer1",
           old_dm.ReadInt(pid, old_one.c_str(), 0),
           new_dm.ReadInt(pid, new_one.c_str(), 0));

    const std::string old_two = "[[" + hex_addr(&old_p2) + "]]";
    const std::string new_two = "[[" + hex_addr(&new_p2) + "]]";
    eq_num("ReadInt-expr-pointer2",
           old_dm.ReadInt(pid, old_two.c_str(), 0),
           new_dm.ReadInt(pid, new_two.c_str(), 0));

    static std::int32_t module_value = 0x13579BDF;
    char module_path[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, module_path, MAX_PATH);
    const char *module_name = std::strrchr(module_path, '\\');
    module_name = module_name ? module_name + 1 : module_path;
    const ULONG_PTR module_base = reinterpret_cast<ULONG_PTR>(::GetModuleHandleA(nullptr));
    const ULONG_PTR module_offset = reinterpret_cast<ULONG_PTR>(&module_value) - module_base;
    char module_expr[512]{};
    std::snprintf(module_expr, sizeof(module_expr), "<%s>+%llX", module_name,
                  static_cast<unsigned long long>(module_offset));
    eq_num("ReadInt-expr-module",
           old_dm.ReadInt(pid, module_expr, 0),
           new_dm.ReadInt(pid, module_expr, 0));

    float old_expr_f = 7.25f, new_expr_f = 7.25f;
    eq_num("ReadFloat-expr",
           old_dm.ReadFloat(pid, hex_addr(&old_expr_f).c_str()),
           new_dm.ReadFloat(pid, hex_addr(&new_expr_f).c_str()));
    eq_num("WriteFloat-expr-ret",
           old_dm.WriteFloat(pid, hex_addr(&old_expr_f).c_str(), -9.5f),
           new_dm.WriteFloat(pid, hex_addr(&new_expr_f).c_str(), -9.5f));
    eq_num("WriteFloat-expr-value", old_expr_f, new_expr_f);

    double old_expr_d = 91.125, new_expr_d = 91.125;
    eq_num("ReadDouble-expr",
           old_dm.ReadDouble(pid, hex_addr(&old_expr_d).c_str()),
           new_dm.ReadDouble(pid, hex_addr(&new_expr_d).c_str()));
    eq_num("WriteDouble-expr-ret",
           old_dm.WriteDouble(pid, hex_addr(&old_expr_d).c_str(), -123.75),
           new_dm.WriteDouble(pid, hex_addr(&new_expr_d).c_str(), -123.75));
    eq_num("WriteDouble-expr-value", old_expr_d, new_expr_d);

    char old_ascii[64] = "hello-memory";
    char new_ascii[64] = "hello-memory";
    {
        const char *a = old_dm.ReadStringAddr(pid, reinterpret_cast<LONGLONG>(old_ascii), 0, 0);
        const char *b = new_dm.ReadStringAddr(pid, reinterpret_cast<LONGLONG>(new_ascii), 0, 0);
        eq_str("ReadStringAddr-ascii", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    eq_num("WriteStringAddr-ascii-ret",
           old_dm.WriteStringAddr(pid, reinterpret_cast<LONGLONG>(old_ascii), 0, "changed"),
           new_dm.WriteStringAddr(pid, reinterpret_cast<LONGLONG>(new_ascii), 0, "changed"));
    eq_str("WriteStringAddr-ascii-value", old_ascii, new_ascii);

    wchar_t old_wide[64] = L"wide-test";
    wchar_t new_wide[64] = L"wide-test";
    {
        const char *a = old_dm.ReadStringAddr(pid, reinterpret_cast<LONGLONG>(old_wide), 1, 0);
        const char *b = new_dm.ReadStringAddr(pid, reinterpret_cast<LONGLONG>(new_wide), 1, 0);
        eq_str("ReadStringAddr-unicode", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    eq_num("WriteStringAddr-unicode-ret",
           old_dm.WriteStringAddr(pid, reinterpret_cast<LONGLONG>(old_wide), 1, "wide-changed"),
           new_dm.WriteStringAddr(pid, reinterpret_cast<LONGLONG>(new_wide), 1, "wide-changed"));
    eq_num("WriteStringAddr-unicode-value",
           std::wcscmp(old_wide, new_wide), 0);

    unsigned char old_data_expr[8] = {1,2,3,4,5,6,7,8};
    unsigned char new_data_expr[8] = {1,2,3,4,5,6,7,8};
    {
        const char *a = old_dm.ReadData(pid, hex_addr(old_data_expr).c_str(), 8);
        const char *b = new_dm.ReadData(pid, hex_addr(new_data_expr).c_str(), 8);
        eq_str("ReadData-expr", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    eq_num("WriteData-expr-ret",
           old_dm.WriteData(pid, hex_addr(old_data_expr).c_str(), "AA BB CC DD"),
           new_dm.WriteData(pid, hex_addr(new_data_expr).c_str(), "AA BB CC DD"));
    eq_num("WriteData-expr-value",
           std::memcmp(old_data_expr, new_data_expr, sizeof(old_data_expr)), 0);

    char old_string_expr[64] = "expr-string";
    char new_string_expr[64] = "expr-string";
    {
        const char *a = old_dm.ReadString(pid, hex_addr(old_string_expr).c_str(), 0, 0);
        const char *b = new_dm.ReadString(pid, hex_addr(new_string_expr).c_str(), 0, 0);
        eq_str("ReadString-expr", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    eq_num("WriteString-expr-ret",
           old_dm.WriteString(pid, hex_addr(old_string_expr).c_str(), 0, "expr-new"),
           new_dm.WriteString(pid, hex_addr(new_string_expr).c_str(), 0, "expr-new"));
    eq_str("WriteString-expr-value", old_string_expr, new_string_expr);

    if (old_page) ::VirtualFree(old_page, 0, MEM_RELEASE);
    if (new_page) ::VirtualFree(new_page, 0, MEM_RELEASE);
}

LRESULT CALLBACK ParityWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return ::DefWindowProcA(hwnd, msg, wp, lp);
}

void test_window(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const char *cls = "hcbyj_parity_window";
    WNDCLASSA wc{};
    wc.lpfnWndProc = ParityWndProc;
    wc.hInstance = ::GetModuleHandleA(nullptr);
    wc.lpszClassName = cls;
    ::RegisterClassA(&wc);

    HWND hwnd = ::CreateWindowExA(
        0, cls, "parity-start", WS_OVERLAPPEDWINDOW,
        40, 50, 420, 300, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        fail("CreateWindow", "success", "failed");
        return;
    }
    const long h = static_cast<long>(reinterpret_cast<INT_PTR>(hwnd));

    eq_num("GetWindowProcessId", old_dm.GetWindowProcessId(h), new_dm.GetWindowProcessId(h));
    eq_num("GetWindowThreadId", old_dm.GetWindowThreadId(h), new_dm.GetWindowThreadId(h));

    long ax1=0,ay1=0,ax2=0,ay2=0,bx1=0,by1=0,bx2=0,by2=0;
    const long ar = old_dm.GetWindowRect(h,&ax1,&ay1,&ax2,&ay2);
    const long br = new_dm.GetWindowRect(h,&bx1,&by1,&bx2,&by2);
    eq_num("GetWindowRect-ret", ar, br);
    eq_num("GetWindowRect-x1", ax1, bx1);
    eq_num("GetWindowRect-y1", ay1, by1);
    eq_num("GetWindowRect-x2", ax2, bx2);
    eq_num("GetWindowRect-y2", ay2, by2);

    ax1=ay1=ax2=ay2=bx1=by1=bx2=by2=0;
    eq_num("GetClientRect-ret",
           old_dm.GetClientRect(h,&ax1,&ay1,&ax2,&ay2),
           new_dm.GetClientRect(h,&bx1,&by1,&bx2,&by2));
    eq_num("GetClientRect-x1", ax1, bx1);
    eq_num("GetClientRect-y1", ay1, by1);
    eq_num("GetClientRect-x2", ax2, bx2);
    eq_num("GetClientRect-y2", ay2, by2);

    long aw=0,ah=0,bw=0,bh=0;
    eq_num("GetClientSize-ret", old_dm.GetClientSize(h,&aw,&ah), new_dm.GetClientSize(h,&bw,&bh));
    eq_num("GetClientSize-w", aw, bw);
    eq_num("GetClientSize-h", ah, bh);

    {
        const char *a = old_dm.GetWindowTitle(h);
        const char *b = new_dm.GetWindowTitle(h);
        eq_str("GetWindowTitle", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.GetWindowClass(h);
        const char *b = new_dm.GetWindowClass(h);
        eq_str("GetWindowClass", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.GetWindowProcessPath(h);
        const char *b = new_dm.GetWindowProcessPath(h);
        eq_str("GetWindowProcessPath", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.GetRealPath(".");
        const char *b = new_dm.GetRealPath(".");
        eq_str("GetRealPath", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }

    long ax=7, ay=11, bx=7, by=11;
    eq_num("ClientToScreen-ret", old_dm.ClientToScreen(h,&ax,&ay), new_dm.ClientToScreen(h,&bx,&by));
    eq_num("ClientToScreen-x", ax, bx);
    eq_num("ClientToScreen-y", ay, by);

    eq_num("ScreenToClient-ret", old_dm.ScreenToClient(h,&ax,&ay), new_dm.ScreenToClient(h,&bx,&by));
    eq_num("ScreenToClient-x", ax, bx);
    eq_num("ScreenToClient-y", ay, by);

    ::SetWindowTextA(hwnd, "parity-start");
    const long old_text_ret = old_dm.SetWindowText(h, "parity-changed");
    char old_title[128]{};
    ::GetWindowTextA(hwnd, old_title, sizeof(old_title));
    ::SetWindowTextA(hwnd, "parity-start");
    const long new_text_ret = new_dm.SetWindowText(h, "parity-changed");
    char new_title[128]{};
    ::GetWindowTextA(hwnd, new_title, sizeof(new_title));
    eq_num("SetWindowText-ret", old_text_ret, new_text_ret);
    eq_str("SetWindowText-effect", old_title, new_title);

    ::SetWindowPos(hwnd,nullptr,40,50,420,300,SWP_NOZORDER|SWP_NOACTIVATE);
    const long old_size_ret = old_dm.SetWindowSize(h, 500, 360);
    RECT old_wr{}; ::GetWindowRect(hwnd,&old_wr);
    ::SetWindowPos(hwnd,nullptr,40,50,420,300,SWP_NOZORDER|SWP_NOACTIVATE);
    const long new_size_ret = new_dm.SetWindowSize(h, 500, 360);
    RECT new_wr{}; ::GetWindowRect(hwnd,&new_wr);
    eq_num("SetWindowSize-ret", old_size_ret, new_size_ret);
    eq_num("SetWindowSize-width", old_wr.right-old_wr.left, new_wr.right-new_wr.left);
    eq_num("SetWindowSize-height", old_wr.bottom-old_wr.top, new_wr.bottom-new_wr.top);

    ::SetWindowPos(hwnd,nullptr,40,50,420,300,SWP_NOZORDER|SWP_NOACTIVATE);
    const long old_client_ret = old_dm.SetClientSize(h, 320, 200);
    RECT old_cr{}; ::GetClientRect(hwnd,&old_cr);
    ::SetWindowPos(hwnd,nullptr,40,50,420,300,SWP_NOZORDER|SWP_NOACTIVATE);
    const long new_client_ret = new_dm.SetClientSize(h, 320, 200);
    RECT new_cr{}; ::GetClientRect(hwnd,&new_cr);
    eq_num("SetClientSize-ret", old_client_ret, new_client_ret);
    eq_num("SetClientSize-width", old_cr.right-old_cr.left, new_cr.right-new_cr.left);
    eq_num("SetClientSize-height", old_cr.bottom-old_cr.top, new_cr.bottom-new_cr.top);


    const long state_flags[] = {0, 2, 3, 4, 5, 7, 8, 9};
    for (long flag : state_flags)
        eq_num(("GetWindowState-flag" + std::to_string(flag)).c_str(),
               old_dm.GetWindowState(h, flag), new_dm.GetWindowState(h, flag));

    HWND child = ::CreateWindowExA(
        0, "STATIC", "parity-child", WS_CHILD | WS_VISIBLE,
        5, 5, 80, 30, hwnd, nullptr, wc.hInstance, nullptr);
    if (child) {
        eq_num("GetWindow-parent", old_dm.GetWindow(static_cast<long>(reinterpret_cast<INT_PTR>(child)), 0),
                                   new_dm.GetWindow(static_cast<long>(reinterpret_cast<INT_PTR>(child)), 0));
        eq_num("GetWindow-child", old_dm.GetWindow(h, 1), new_dm.GetWindow(h, 1));
        eq_num("GetWindow-root", old_dm.GetWindow(static_cast<long>(reinterpret_cast<INT_PTR>(child)), 7),
                                 new_dm.GetWindow(static_cast<long>(reinterpret_cast<INT_PTR>(child)), 7));
    }

    const long pid = static_cast<long>(::GetCurrentProcessId());
    eq_num("FindWindowByProcessId",
           old_dm.FindWindowByProcessId(pid, cls, "parity-changed"),
           new_dm.FindWindowByProcessId(pid, cls, "parity-changed"));

    char exe_path[MAX_PATH]{};
    ::GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    const char *exe_name = std::strrchr(exe_path, '\\');
    exe_name = exe_name ? exe_name + 1 : exe_path;

    eq_num("FindWindowByProcess",
           old_dm.FindWindowByProcess(exe_name, cls, "parity-changed"),
           new_dm.FindWindowByProcess(exe_name, cls, "parity-changed"));

    {
        const char *a = old_dm.EnumWindowByProcessId(pid, "parity-changed", cls, 1 | 2);
        const char *b = new_dm.EnumWindowByProcessId(pid, "parity-changed", cls, 1 | 2);
        eq_str("EnumWindowByProcessId", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.EnumWindowByProcess(exe_name, "parity-changed", cls, 1 | 2);
        const char *b = new_dm.EnumWindowByProcess(exe_name, "parity-changed", cls, 1 | 2);
        eq_str("EnumWindowByProcess", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.EnumWindow(0, "parity-changed", cls, 1 | 2 | 8);
        const char *b = new_dm.EnumWindow(0, "parity-changed", cls, 1 | 2 | 8);
        eq_str("EnumWindow", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.EnumProcess(exe_name);
        const char *b = new_dm.EnumProcess(exe_name);
        eq_str("EnumProcess", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }

    // Safe SetWindowState flags only. Compare both return value and observable state.
    const long safe_flags[] = {6, 7, 8, 9, 10, 11};
    for (long flag : safe_flags) {
        const long old_ret = old_dm.SetWindowState(h, flag);
        const long old_visible = ::IsWindowVisible(hwnd) ? 1 : 0;
        const long old_enabled = ::IsWindowEnabled(hwnd) ? 1 : 0;
        const long old_topmost = (::GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;

        // Reset to neutral before running the recovered version.
        ::ShowWindow(hwnd, SW_SHOWNA);
        ::EnableWindow(hwnd, TRUE);
        ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        const long new_ret = new_dm.SetWindowState(h, flag);
        const long new_visible = ::IsWindowVisible(hwnd) ? 1 : 0;
        const long new_enabled = ::IsWindowEnabled(hwnd) ? 1 : 0;
        const long new_topmost = (::GetWindowLongPtr(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;

        eq_num(("SetWindowState-ret-" + std::to_string(flag)).c_str(), old_ret, new_ret);
        eq_num(("SetWindowState-visible-" + std::to_string(flag)).c_str(), old_visible, new_visible);
        eq_num(("SetWindowState-enabled-" + std::to_string(flag)).c_str(), old_enabled, new_enabled);
        eq_num(("SetWindowState-topmost-" + std::to_string(flag)).c_str(), old_topmost, new_topmost);

        ::ShowWindow(hwnd, SW_SHOWNA);
        ::EnableWindow(hwnd, TRUE);
        ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }


    // Transparency is tested only on this temporary parity window.
    ::SetWindowLongPtr(hwnd, GWL_EXSTYLE,
        ::GetWindowLongPtr(hwnd, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_LAYERED));
    const long old_trans_ret = old_dm.SetWindowTransparent(h, 180);
    BYTE old_alpha = 255;
    DWORD old_flags = 0;
    COLORREF old_key = 0;
    const BOOL old_layered = ::GetLayeredWindowAttributes(hwnd, &old_key, &old_alpha, &old_flags);

    ::SetWindowLongPtr(hwnd, GWL_EXSTYLE,
        ::GetWindowLongPtr(hwnd, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_LAYERED));

    const long new_trans_ret = new_dm.SetWindowTransparent(h, 180);
    BYTE new_alpha = 255;
    DWORD new_flags = 0;
    COLORREF new_key = 0;
    const BOOL new_layered = ::GetLayeredWindowAttributes(hwnd, &new_key, &new_alpha, &new_flags);

    eq_num("SetWindowTransparent-ret", old_trans_ret, new_trans_ret);
    eq_num("SetWindowTransparent-query", old_layered ? 1 : 0, new_layered ? 1 : 0);
    eq_num("SetWindowTransparent-alpha", old_alpha, new_alpha);
    eq_num("SetWindowTransparent-flags", old_flags, new_flags);

    ::SetWindowLongPtr(hwnd, GWL_EXSTYLE,
        ::GetWindowLongPtr(hwnd, GWL_EXSTYLE) & ~static_cast<LONG_PTR>(WS_EX_LAYERED));


    HWND edit = ::CreateWindowExA(
        WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        10, 50, 250, 24, hwnd, nullptr, wc.hInstance, nullptr);
    if (edit) {
        const long edit_h = static_cast<long>(reinterpret_cast<INT_PTR>(edit));

        ::OpenClipboard(nullptr);
        ::EmptyClipboard();
        const char paste_text[] = "hcbyj-parity-paste";
        HGLOBAL clip_mem = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(paste_text));
        if (clip_mem) {
            void *clip_ptr = ::GlobalLock(clip_mem);
            if (clip_ptr) {
                std::memcpy(clip_ptr, paste_text, sizeof(paste_text));
                ::GlobalUnlock(clip_mem);
                ::SetClipboardData(CF_TEXT, clip_mem);
            } else {
                ::GlobalFree(clip_mem);
            }
        }
        ::CloseClipboard();

        ::SetWindowTextA(edit, "");
        const long old_paste_ret = old_dm.SendPaste(edit_h);
        char old_paste[128]{};
        ::GetWindowTextA(edit, old_paste, sizeof(old_paste));

        ::SetWindowTextA(edit, "");
        const long new_paste_ret = new_dm.SendPaste(edit_h);
        char new_paste[128]{};
        ::GetWindowTextA(edit, new_paste, sizeof(new_paste));

        eq_num("SendPaste-edit-ret", old_paste_ret, new_paste_ret);
        eq_str("SendPaste-edit-effect", old_paste, new_paste);

        ::SetWindowTextA(edit, "");
        const long old_send_ret = old_dm.SendString(edit_h, "Abc123");
        char old_send[128]{};
        ::GetWindowTextA(edit, old_send, sizeof(old_send));

        ::SetWindowTextA(edit, "");
        const long new_send_ret = new_dm.SendString(edit_h, "Abc123");
        char new_send[128]{};
        ::GetWindowTextA(edit, new_send, sizeof(new_send));

        eq_num("SendString-edit-ret", old_send_ret, new_send_ret);
        eq_str("SendString-edit-effect", old_send, new_send);
        ::DestroyWindow(edit);
    }

    if (child) ::DestroyWindow(child);

    ::DestroyWindow(hwnd);
    ::UnregisterClassA(cls, wc.hInstance);
}


void test_pure_extended(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    for (const char *v : {"", "a", "abc", "hello world", "中文"}) {
        const char *a = old_dm.Md5(v);
        const char *b = new_dm.Md5(v);
        eq_str("Md5", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }

    for (const char *v : {"", "1,2", "1,2|3,4", "-5,6|7,-8|9,10,extra"}) {
        eq_num("GetResultCount", old_dm.GetResultCount(v), new_dm.GetResultCount(v));
        for (long i = -1; i < 5; ++i) {
            long ox=111, oy=222, nx=111, ny=222;
            const long orv = old_dm.GetResultPos(v, i, &ox, &oy);
            const long nrv = new_dm.GetResultPos(v, i, &nx, &ny);
            eq_num("GetResultPos-ret", orv, nrv);
            eq_num("GetResultPos-x", ox, nx);
            eq_num("GetResultPos-y", oy, ny);
        }
    }

    const LONGLONG ints[] = {0,1,-1,0x12345678LL,0x123456789abcdef0LL};
    for (long type = -1; type <= 7; ++type) {
        for (LONGLONG v : ints) {
            const char *a = old_dm.IntToData(v, type);
            const char *b = new_dm.IntToData(v, type);
            eq_str("IntToData", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
        }
    }

    for (float v : {0.0f, 1.0f, -1.0f, 1.5f, 123.25f}) {
        const char *a = old_dm.FloatToData(v);
        const char *b = new_dm.FloatToData(v);
        eq_str("FloatToData", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }

    for (double v : {0.0, 1.0, -1.0, 1.5, 123456.25}) {
        const char *a = old_dm.DoubleToData(v);
        const char *b = new_dm.DoubleToData(v);
        eq_str("DoubleToData", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }

    for (const char *v : {"", "abc", "A B", "中文"}) {
        for (long type = -1; type <= 2; ++type) {
            const char *a = old_dm.StringToData(v, type);
            const char *b = new_dm.StringToData(v, type);
            eq_str("StringToData", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
        }
    }

    eq_num("GetLocale", old_dm.GetLocale(), new_dm.GetLocale());
    eq_num("CheckUAC", old_dm.CheckUAC(), new_dm.CheckUAC());
}


void test_basic_settings(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    eq_num("SetPath-dot", old_dm.SetPath("."), new_dm.SetPath("."));
    {
        const char *a = old_dm.GetPath();
        const char *b = new_dm.GetPath();
        eq_str("GetPath", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.GetBasePath();
        const char *b = new_dm.GetBasePath();
        eq_str("GetBasePath", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }

    eq_num("GetDmCount", old_dm.GetDmCount(), new_dm.GetDmCount());
    eq_num("SetEnumWindowDelay", old_dm.SetEnumWindowDelay(12345), new_dm.SetEnumWindowDelay(12345));
    eq_num("SetShowErrorMsg-0", old_dm.SetShowErrorMsg(0), new_dm.SetShowErrorMsg(0));
    eq_num("SetShowErrorMsg-1", old_dm.SetShowErrorMsg(1), new_dm.SetShowErrorMsg(1));

    const long old_id1 = old_dm.GetID();
    const long old_id2 = old_dm.GetID();
    const long new_id1 = new_dm.GetID();
    const long new_id2 = new_dm.GetID();
    eq_num("GetID-legacy-stable", old_id1, old_id2);
    eq_num("GetID-recovered-stable", new_id1, new_id2);
    eq_num("GetID-legacy-nonzero", old_id1 != 0 ? 1 : 0, 1);
    eq_num("GetID-recovered-nonzero", new_id1 != 0 ? 1 : 0, 1);
}


void test_position_algorithms(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    struct Case { long type; const char *text; };
    const Case cases[] = {
        {0, "0,10,20|2,100,200|1,30,40|3,10,10"},
        {1, "10,20|100,200|30,40|10,10"},
        {2, "A$10$20|B$100$200|C$30$40|D$10$10"},
        {3, "a.bmp,10,20|b.bmp,100,200|c.bmp,30,40|d.bmp,10,10"},
    };

    for (const auto &c : cases) {
        {
            const char *a = old_dm.ExcludePos(c.text, c.type, 9, 9, 30, 40);
            const char *b = new_dm.ExcludePos(c.text, c.type, 9, 9, 30, 40);
            eq_str("ExcludePos", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
        }
        {
            const char *a = old_dm.FindNearestPos(c.text, c.type, 11, 12);
            const char *b = new_dm.FindNearestPos(c.text, c.type, 11, 12);
            eq_str("FindNearestPos", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
        }
        {
            const char *a = old_dm.SortPosDistance(c.text, c.type, 0, 0);
            const char *b = new_dm.SortPosDistance(c.text, c.type, 0, 0);
            eq_str("SortPosDistance-distance", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
        }
        {
            const char *a = old_dm.SortPosDistance(c.text, c.type, 65535, 0);
            const char *b = new_dm.SortPosDistance(c.text, c.type, 65535, 0);
            eq_str("SortPosDistance-x", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
        }
        {
            const char *a = old_dm.SortPosDistance(c.text, c.type, 0, 65535);
            const char *b = new_dm.SortPosDistance(c.text, c.type, 0, 65535);
            eq_str("SortPosDistance-y", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
        }
    }
}


void test_word_result_and_input(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const char *cases[] = {
        "10,30|20,40|A|B",
        "1|2|only",
        "|2|empty-x",
        "",
        "10,20|30,40|hello|world|"
    };

    for (const char *v : cases) {
        eq_num("GetWordResultCount", old_dm.GetWordResultCount(v), new_dm.GetWordResultCount(v));
        for (long index = -1; index < 5; ++index) {
            long ox = 777, oy = 888, nx = 777, ny = 888;
            const long orv = old_dm.GetWordResultPos(v, index, &ox, &oy);
            const long nrv = new_dm.GetWordResultPos(v, index, &nx, &ny);
            eq_num("GetWordResultPos-ret", orv, nrv);
            eq_num("GetWordResultPos-x", ox, nx);
            eq_num("GetWordResultPos-y", oy, ny);

            const char *os = old_dm.GetWordResultStr(v, index);
            const char *ns = new_dm.GetWordResultStr(v, index);
            eq_str("GetWordResultStr",
                   os ? std::string(os) : "<null>",
                   ns ? std::string(ns) : "<null>");
        }
    }

    // F24 is intentionally chosen because normal desktop applications almost
    // never bind it. Down/up is paired immediately to avoid a stuck key.
    eq_num("SetKeypadDelay-normal",
           old_dm.SetKeypadDelay("normal", 1),
           new_dm.SetKeypadDelay("normal", 1));
    const long old_down = old_dm.KeyDown(VK_F24);
    const long old_up = old_dm.KeyUp(VK_F24);
    const long new_down = new_dm.KeyDown(VK_F24);
    const long new_up = new_dm.KeyUp(VK_F24);
    eq_num("KeyDown-F24", old_down, new_down);
    eq_num("KeyUp-F24", old_up, new_up);

    // Timeout-only path: no user input is synthesized here.
    eq_num("WaitKey-F24-timeout",
           old_dm.WaitKey(VK_F24, 1),
           new_dm.WaitKey(VK_F24, 1));

    eq_num("SetMouseDelay-normal",
           old_dm.SetMouseDelay("normal", 1),
           new_dm.SetMouseDelay("normal", 1));

    POINT pt{};
    ::GetCursorPos(&pt);
    eq_num("MoveR-zero", old_dm.MoveR(0, 0), new_dm.MoveR(0, 0));
    eq_num("MoveTo-current",
           old_dm.MoveTo(pt.x, pt.y),
           new_dm.MoveTo(pt.x, pt.y));
    const long old_char_down = old_dm.KeyDownChar("f24");
    const long old_char_up = old_dm.KeyUpChar("f24");
    const long new_char_down = new_dm.KeyDownChar("f24");
    const long new_char_up = new_dm.KeyUpChar("f24");
    eq_num("KeyDownChar-f24", old_char_down, new_char_down);
    eq_num("KeyUpChar-f24", old_char_up, new_char_up);

    eq_num("KeyPressChar-invalid",
           old_dm.KeyPressChar("not-a-key"),
           new_dm.KeyPressChar("not-a-key"));
    eq_num("KeyPressStr-empty",
           old_dm.KeyPressStr("", 1),
           new_dm.KeyPressStr("", 1));
}


std::vector<std::string> split_pipe_test(const std::string &value) {
    std::vector<std::string> out;
    size_t begin = 0;
    for (;;) {
        const size_t pos = value.find('|', begin);
        if (pos == std::string::npos) {
            out.push_back(value.substr(begin));
            break;
        }
        out.push_back(value.substr(begin, pos - begin));
        begin = pos + 1;
    }
    return out;
}

void test_system_identity(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    for (long flag = -1; flag <= 2; ++flag) {
        eq_num(("GetSpecialWindow-" + std::to_string(flag)).c_str(),
               old_dm.GetSpecialWindow(flag),
               new_dm.GetSpecialWindow(flag));
    }

    const long pid = static_cast<long>(::GetCurrentProcessId());
    old_dm.SetMemoryHwndAsProcessId(1);
    new_dm.SetMemoryHwndAsProcessId(1);
    {
        const char *a = old_dm.GetCommandLine(pid);
        const char *b = new_dm.GetCommandLine(pid);
        eq_str("GetCommandLine-self",
               a ? std::string(a) : "<null>",
               b ? std::string(b) : "<null>");
    }
    old_dm.SetMemoryHwndAsProcessId(0);
    new_dm.SetMemoryHwndAsProcessId(0);

    for (long index = -1; index <= 6; ++index) {
        {
            const char *a = old_dm.GetDiskModel(index);
            const char *b = new_dm.GetDiskModel(index);
            eq_str(("GetDiskModel-" + std::to_string(index)).c_str(),
                   a ? std::string(a) : "<null>",
                   b ? std::string(b) : "<null>");
        }
        {
            const char *a = old_dm.GetDiskReversion(index);
            const char *b = new_dm.GetDiskReversion(index);
            eq_str(("GetDiskReversion-" + std::to_string(index)).c_str(),
                   a ? std::string(a) : "<null>",
                   b ? std::string(b) : "<null>");
        }
        {
            const char *a = old_dm.GetDiskSerial(index);
            const char *b = new_dm.GetDiskSerial(index);
            eq_str(("GetDiskSerial-" + std::to_string(index)).c_str(),
                   a ? std::string(a) : "<null>",
                   b ? std::string(b) : "<null>");
        }
    }

    {
        const char *a = old_dm.GetDisplayInfo();
        const char *b = new_dm.GetDisplayInfo();
        eq_str("GetDisplayInfo",
               a ? std::string(a) : "<null>",
               b ? std::string(b) : "<null>");
    }

    // GetProcessInfo deliberately samples for about one second. CPU and working-set
    // values can change between the two sequential calls, so compare the stable
    // fields and validate the documented 4-field shape/ranges.
    {
        const char *a_raw = old_dm.GetProcessInfo(pid);
        const std::string a = a_raw ? a_raw : "";
        const char *b_raw = new_dm.GetProcessInfo(pid);
        const std::string b = b_raw ? b_raw : "";
        const auto af = split_pipe_test(a);
        const auto bf = split_pipe_test(b);
        eq_num("GetProcessInfo-field-count-old", static_cast<long>(af.size()), 4L);
        eq_num("GetProcessInfo-field-count-new", static_cast<long>(bf.size()), 4L);
        if (af.size() == 4 && bf.size() == 4) {
            eq_str("GetProcessInfo-name", af[0], bf[0]);
            eq_str("GetProcessInfo-path", af[1], bf[1]);
            const long old_cpu = std::strtol(af[2].c_str(), nullptr, 10);
            const long new_cpu = std::strtol(bf[2].c_str(), nullptr, 10);
            eq_num("GetProcessInfo-old-cpu-range", old_cpu >= 0 && old_cpu <= 100 ? 1L : 0L, 1L);
            eq_num("GetProcessInfo-new-cpu-range", new_cpu >= 0 && new_cpu <= 100 ? 1L : 0L, 1L);
            const unsigned long long old_mem = std::strtoull(af[3].c_str(), nullptr, 10);
            const unsigned long long new_mem = std::strtoull(bf[3].c_str(), nullptr, 10);
            eq_num("GetProcessInfo-old-memory-nonzero", old_mem > 0 ? 1L : 0L, 1L);
            eq_num("GetProcessInfo-new-memory-nonzero", new_mem > 0 ? 1L : 0L, 1L);
        }
    }
}


LRESULT CALLBACK ColorParityWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        RECT r{};
        ::GetClientRect(hwnd, &r);
        const int mid_x = (r.right - r.left) / 2;
        const int mid_y = (r.bottom - r.top) / 2;

        struct Fill { RECT rc; COLORREF color; };
        const Fill fills[] = {
            {{0, 0, mid_x, mid_y}, RGB(255, 0, 0)},
            {{mid_x, 0, r.right, mid_y}, RGB(0, 255, 0)},
            {{0, mid_y, mid_x, r.bottom}, RGB(0, 0, 255)},
            {{mid_x, mid_y, r.right, r.bottom}, RGB(255, 255, 255)},
        };
        for (const auto &fill : fills) {
            HBRUSH brush = ::CreateSolidBrush(fill.color);
            ::FillRect(dc, &fill.rc, brush);
            ::DeleteObject(brush);
        }
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    return ::DefWindowProcA(hwnd, msg, wp, lp);
}

void test_color_core(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const char *cls = "hcbyj_color_parity_window";
    WNDCLASSA wc{};
    wc.lpfnWndProc = ColorParityWndProc;
    wc.hInstance = ::GetModuleHandleA(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    ::RegisterClassA(&wc);

    HWND hwnd = ::CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        cls,
        "hcbyj-color-parity",
        WS_POPUP,
        120, 120, 64, 64,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        fail("ColorParityWindow", "success", "failed");
        return;
    }

    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ::UpdateWindow(hwnd);
    ::SetWindowPos(
        hwnd, HWND_TOPMOST, 120, 120, 64, 64,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    ::GdiFlush();
    ::Sleep(80);

    POINT origin{0, 0};
    ::ClientToScreen(hwnd, &origin);
    const long x1 = origin.x;
    const long y1 = origin.y;
    const long x2 = x1 + 63;
    const long y2 = y1 + 63;

    struct PointCase {
        long x;
        long y;
        const char *label;
    };
    const PointCase points[] = {
        {x1 + 8,  y1 + 8,  "red"},
        {x1 + 40, y1 + 8,  "green"},
        {x1 + 8,  y1 + 40, "blue"},
        {x1 + 40, y1 + 40, "white"},
    };

    for (long capture_mode : {0L, 1L}) {
        eq_num(("EnableGetColorByCapture-" + std::to_string(capture_mode)).c_str(),
               old_dm.EnableGetColorByCapture(capture_mode),
               new_dm.EnableGetColorByCapture(capture_mode));

        for (const auto &pt : points) {
            {
                const char *a = old_dm.GetColor(pt.x, pt.y);
                const char *b = new_dm.GetColor(pt.x, pt.y);
                eq_str((std::string("GetColor-") + pt.label + "-" + std::to_string(capture_mode)).c_str(),
                       a ? std::string(a) : "<null>",
                       b ? std::string(b) : "<null>");
            }
            {
                const char *a = old_dm.GetColorBGR(pt.x, pt.y);
                const char *b = new_dm.GetColorBGR(pt.x, pt.y);
                eq_str((std::string("GetColorBGR-") + pt.label + "-" + std::to_string(capture_mode)).c_str(),
                       a ? std::string(a) : "<null>",
                       b ? std::string(b) : "<null>");
            }
            {
                const char *a = old_dm.GetColorHSV(pt.x, pt.y);
                const char *b = new_dm.GetColorHSV(pt.x, pt.y);
                eq_str((std::string("GetColorHSV-") + pt.label + "-" + std::to_string(capture_mode)).c_str(),
                       a ? std::string(a) : "<null>",
                       b ? std::string(b) : "<null>");
            }
        }
    }

    old_dm.EnableGetColorByCapture(1);
    new_dm.EnableGetColorByCapture(1);

    {
        const char *a = old_dm.GetAveRGB(x1, y1, x1 + 31, y1 + 31);
        const char *b = new_dm.GetAveRGB(x1, y1, x1 + 31, y1 + 31);
        eq_str("GetAveRGB-red-block", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.GetAveHSV(x1, y1, x1 + 31, y1 + 31);
        const char *b = new_dm.GetAveHSV(x1, y1, x1 + 31, y1 + 31);
        eq_str("GetAveHSV-red-block", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.GetAveRGB(x1, y1, x2, y2);
        const char *b = new_dm.GetAveRGB(x1, y1, x2, y2);
        eq_str("GetAveRGB-four-color", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.GetAveHSV(x1, y1, x2, y2);
        const char *b = new_dm.GetAveHSV(x1, y1, x2, y2);
        eq_str("GetAveHSV-four-color", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }

    eq_num("CmpColor-red-exact",
           old_dm.CmpColor(x1 + 8, y1 + 8, "ff0000-000000", 1.0),
           new_dm.CmpColor(x1 + 8, y1 + 8, "ff0000-000000", 1.0));
    eq_num("CmpColor-red-mismatch",
           old_dm.CmpColor(x1 + 8, y1 + 8, "00ff00-000000", 1.0),
           new_dm.CmpColor(x1 + 8, y1 + 8, "00ff00-000000", 1.0));
    eq_num("CmpColor-red-sim",
           old_dm.CmpColor(x1 + 8, y1 + 8, "ee0000", 0.9),
           new_dm.CmpColor(x1 + 8, y1 + 8, "ee0000", 0.9));

    for (const char *spec : {"ff0000-000000", "ff0000|00ff00", "@ff0000|00ff00"}) {
        eq_num((std::string("GetColorNum-") + spec).c_str(),
               old_dm.GetColorNum(x1, y1, x2, y2, spec, 1.0),
               new_dm.GetColorNum(x1, y1, x2, y2, spec, 1.0));
    }

    for (long dir = 0; dir <= 8; ++dir) {
        long ox=-9, oy=-9, nx=-9, ny=-9;
        const long orv = old_dm.FindColor(
            x1, y1, x2, y2, "ff0000-000000", 1.0, dir, &ox, &oy);
        const long nrv = new_dm.FindColor(
            x1, y1, x2, y2, "ff0000-000000", 1.0, dir, &nx, &ny);
        eq_num(("FindColor-ret-" + std::to_string(dir)).c_str(), orv, nrv);
        eq_num(("FindColor-x-" + std::to_string(dir)).c_str(), ox, nx);
        eq_num(("FindColor-y-" + std::to_string(dir)).c_str(), oy, ny);
    }

    {
        const char *a = old_dm.FindColorE(
            x1, y1, x2, y2, "00ff00-000000", 1.0, 0);
        const char *b = new_dm.FindColorE(
            x1, y1, x2, y2, "00ff00-000000", 1.0, 0);
        eq_str("FindColorE", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }
    {
        // A 4x4 subset keeps the returned coordinate string short and deterministic.
        const char *a = old_dm.FindColorEx(
            x1, y1, x1 + 3, y1 + 3, "ff0000-000000", 1.0, 0);
        const char *b = new_dm.FindColorEx(
            x1, y1, x1 + 3, y1 + 3, "ff0000-000000", 1.0, 0);
        eq_str("FindColorEx", a ? std::string(a) : "<null>", b ? std::string(b) : "<null>");
    }



    const char *multi_offsets =
        "32|0|00ff00-000000,0|32|0000ff-000000,32|32|ffffff-000000";
    for (long dir = 0; dir <= 3; ++dir) {
        long ox=-1, oy=-1, nx=-1, ny=-1;
        const long orv = old_dm.FindMultiColor(
            x1, y1, x1 + 31, y1 + 31,
            "ff0000-000000", multi_offsets, 1.0, dir, &ox, &oy);
        const long nrv = new_dm.FindMultiColor(
            x1, y1, x1 + 31, y1 + 31,
            "ff0000-000000", multi_offsets, 1.0, dir, &nx, &ny);
        eq_num(("FindMultiColor-ret-" + std::to_string(dir)).c_str(), orv, nrv);
        eq_num(("FindMultiColor-x-" + std::to_string(dir)).c_str(), ox, nx);
        eq_num(("FindMultiColor-y-" + std::to_string(dir)).c_str(), oy, ny);
    }
    {
        const char *a = old_dm.FindMultiColorE(
            x1, y1, x1 + 31, y1 + 31,
            "ff0000-000000", multi_offsets, 1.0, 0);
        const char *b = new_dm.FindMultiColorE(
            x1, y1, x1 + 31, y1 + 31,
            "ff0000-000000", multi_offsets, 1.0, 0);
        eq_str("FindMultiColorE", a ? std::string(a) : "<null>",
                                   b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.FindMultiColorEx(
            x1, y1, x1 + 3, y1 + 3,
            "ff0000-000000", multi_offsets, 1.0, 0);
        const char *b = new_dm.FindMultiColorEx(
            x1, y1, x1 + 3, y1 + 3,
            "ff0000-000000", multi_offsets, 1.0, 0);
        eq_str("FindMultiColorEx", a ? std::string(a) : "<null>",
                                    b ? std::string(b) : "<null>");
    }

    const char *shape = "1|0|1,32|0|0,0|32|0,1|1|1";
    for (long dir = 0; dir <= 3; ++dir) {
        long ox=-1, oy=-1, nx=-1, ny=-1;
        const long orv = old_dm.FindShape(
            x1, y1, x1 + 15, y1 + 15, shape, 1.0, dir, &ox, &oy);
        const long nrv = new_dm.FindShape(
            x1, y1, x1 + 15, y1 + 15, shape, 1.0, dir, &nx, &ny);
        eq_num(("FindShape-ret-" + std::to_string(dir)).c_str(), orv, nrv);
        eq_num(("FindShape-x-" + std::to_string(dir)).c_str(), ox, nx);
        eq_num(("FindShape-y-" + std::to_string(dir)).c_str(), oy, ny);
    }
    {
        const char *a = old_dm.FindShapeE(
            x1, y1, x1 + 15, y1 + 15, shape, 1.0, 0);
        const char *b = new_dm.FindShapeE(
            x1, y1, x1 + 15, y1 + 15, shape, 1.0, 0);
        eq_str("FindShapeE", a ? std::string(a) : "<null>",
                             b ? std::string(b) : "<null>");
    }
    {
        const char *a = old_dm.FindShapeEx(
            x1, y1, x1 + 3, y1 + 3, shape, 1.0, 0);
        const char *b = new_dm.FindShapeEx(
            x1, y1, x1 + 3, y1 + 3, shape, 1.0, 0);
        eq_str("FindShapeEx", a ? std::string(a) : "<null>",
                              b ? std::string(b) : "<null>");
    }

    eq_num("FindMulColor-all-present",
           old_dm.FindMulColor(
               x1, y1, x2, y2,
               "ff0000-000000|00ff00-000000|0000ff-000000|ffffff-000000", 1.0),
           new_dm.FindMulColor(
               x1, y1, x2, y2,
               "ff0000-000000|00ff00-000000|0000ff-000000|ffffff-000000", 1.0));
    eq_num("FindMulColor-one-missing",
           old_dm.FindMulColor(
               x1, y1, x2, y2,
               "ff0000-000000|000000-000000", 1.0),
           new_dm.FindMulColor(
               x1, y1, x2, y2,
               "ff0000-000000|000000-000000", 1.0));

    ::DestroyWindow(hwnd);
    ::UnregisterClassA(cls, wc.hInstance);
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
        test_pure_extended(old_dm, new_dm);
        test_basic_settings(old_dm, new_dm);
        test_position_algorithms(old_dm, new_dm);
        test_word_result_and_input(old_dm, new_dm);
        test_system(old_dm, new_dm);
        test_system_identity(old_dm, new_dm);
        test_env(old_dm, new_dm);
        test_file_ini(old_dm, new_dm);
        test_memory(old_dm, new_dm);
        test_window(old_dm, new_dm);
        test_color_core(old_dm, new_dm);

        FreeDm();

        std::printf("\nSUMMARY passes=%d failures=%d\n", g_passes, g_failures);
        return g_failures == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::printf("PARITY PROBE ERROR: %s\n", e.what());
        return 201;
    }
#endif
}
