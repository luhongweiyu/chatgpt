#include "legacy_rva_client.h"
#include "legacy_dm_x64.h"

#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
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
        test_system(old_dm, new_dm);
        test_env(old_dm, new_dm);
        test_file_ini(old_dm, new_dm);
        test_memory(old_dm, new_dm);
        test_window(old_dm, new_dm);

        FreeDm();

        std::printf("\nSUMMARY passes=%d failures=%d\n", g_passes, g_failures);
        return g_failures == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::printf("PARITY PROBE ERROR: %s\n", e.what());
        return 201;
    }
#endif
}
