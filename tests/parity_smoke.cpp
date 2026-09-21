#include "legacy_rva_client.h"
#include "legacy_dm_x64.h"

#include <windows.h>
#include <dwmapi.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
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


bool parse_beijing_time(
    const std::string &text, __time64_t &out) {
    int y=0,m=0,d=0,hh=0,mm=0,ss=0;
    char tail = '\0';
    if (std::sscanf(
            text.c_str(),
            "%d-%d-%d %d:%d:%d%c",
            &y,&m,&d,&hh,&mm,&ss,&tail) != 6)
        return false;
    if (y < 1970 || m < 1 || m > 12 ||
        d < 1 || d > 31 ||
        hh < 0 || hh > 23 ||
        mm < 0 || mm > 59 ||
        ss < 0 || ss > 60)
        return false;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = m - 1;
    tm.tm_mday = d;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;
    out = _mkgmtime64(&tm);
    return out >= 0;
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




struct BmpMetaProbe {
    bool ok = false;
    long width = 0;
    long height = 0;
    unsigned bit_count = 0;
};

BmpMetaProbe read_bmp_meta(const std::filesystem::path &path) {
    BmpMetaProbe out{};
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    in.read(reinterpret_cast<char *>(&fh), sizeof(fh));
    in.read(reinterpret_cast<char *>(&ih), sizeof(ih));
    if (!in || fh.bfType != 0x4D42 ||
        ih.biSize < sizeof(BITMAPINFOHEADER))
        return out;
    out.ok = true;
    out.width = ih.biWidth;
    out.height = ih.biHeight;
    out.bit_count = ih.biBitCount;
    return out;
}

std::vector<unsigned char> read_prefix(
    const std::filesystem::path &path,
    size_t count) {
    std::vector<unsigned char> out(count, 0);
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    in.read(reinterpret_cast<char *>(out.data()),
            static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<size_t>(in.gcount()));
    return out;
}

std::vector<unsigned char> read_all_bytes(
    const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<unsigned char>(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
}

bool write_test_bmp24(
    const std::filesystem::path &path,
    long width,
    long height) {
    if (width <= 0 || height <= 0) return false;

    const DWORD row_bytes =
        static_cast<DWORD>(((static_cast<unsigned long long>(width) * 3ULL + 3ULL) / 4ULL) * 4ULL);
    const DWORD pixel_bytes = row_bytes * static_cast<DWORD>(height);

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + pixel_bytes;

    ih.biSize = sizeof(ih);
    ih.biWidth = width;
    ih.biHeight = height;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = pixel_bytes;

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(&fh), sizeof(fh));
    out.write(reinterpret_cast<const char *>(&ih), sizeof(ih));

    std::vector<unsigned char> row(row_bytes, 0);
    for (long file_y = 0; file_y < height; ++file_y) {
        std::fill(row.begin(), row.end(), 0);
        const long y = height - 1 - file_y;
        for (long x = 0; x < width; ++x) {
            // Four corners intentionally differ, so this fixture has no
            // transparent corner color.
            const unsigned char r =
                static_cast<unsigned char>((x * 53 + y * 29 + 17) & 0xff);
            const unsigned char g =
                static_cast<unsigned char>((x * 31 + y * 71 + 43) & 0xff);
            const unsigned char b =
                static_cast<unsigned char>((x * 97 + y * 11 + 83) & 0xff);
            row[static_cast<size_t>(x) * 3 + 0] = b;
            row[static_cast<size_t>(x) * 3 + 1] = g;
            row[static_cast<size_t>(x) * 3 + 2] = r;
        }
        out.write(reinterpret_cast<const char *>(row.data()), row.size());
    }
    return out.good();
}

bool write_silent_wav(const std::filesystem::path &path) {
    constexpr unsigned sample_rate = 8000;
    constexpr unsigned data_size = 800; // 100 ms, mono, unsigned 8-bit PCM
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    auto u16 = [&out](unsigned v) {
        const unsigned char b[2] = {
            static_cast<unsigned char>(v & 0xff),
            static_cast<unsigned char>((v >> 8) & 0xff)
        };
        out.write(reinterpret_cast<const char *>(b), sizeof(b));
    };
    auto u32 = [&out](unsigned v) {
        const unsigned char b[4] = {
            static_cast<unsigned char>(v & 0xff),
            static_cast<unsigned char>((v >> 8) & 0xff),
            static_cast<unsigned char>((v >> 16) & 0xff),
            static_cast<unsigned char>((v >> 24) & 0xff)
        };
        out.write(reinterpret_cast<const char *>(b), sizeof(b));
    };

    out.write("RIFF", 4);
    u32(36 + data_size);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(1);               // PCM
    u16(1);               // mono
    u32(sample_rate);
    u32(sample_rate);     // 1 byte per sample
    u16(1);
    u16(8);
    out.write("data", 4);
    u32(data_size);
    std::vector<unsigned char> silence(data_size, 0x80);
    out.write(reinterpret_cast<const char *>(silence.data()), silence.size());
    return out.good();
}


std::string current_layout_text_for_thread(DWORD tid) {
    const HKL hkl = ::GetKeyboardLayout(tid);
    if (!hkl) return {};

    char klid[16]{};
    std::snprintf(
        klid, sizeof(klid), "%08lX",
        static_cast<unsigned long>(
            reinterpret_cast<ULONG_PTR>(hkl) & 0xffffffffULL));

    HKEY key = nullptr;
    std::string path =
        "SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\";
    path += klid;

    if (::RegOpenKeyExA(
            HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key)
        != ERROR_SUCCESS) {
        std::snprintf(
            klid, sizeof(klid), "0000%04X",
            static_cast<unsigned>(
                LOWORD(reinterpret_cast<ULONG_PTR>(hkl))));
        path =
            "SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\";
        path += klid;
        if (::RegOpenKeyExA(
                HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key)
            != ERROR_SUCCESS)
            return {};
    }

    char text[512]{};
    DWORD type = 0;
    DWORD bytes = sizeof(text);
    const LSTATUS st = ::RegQueryValueExA(
        key, "Layout Text", nullptr, &type,
        reinterpret_cast<BYTE *>(text), &bytes);
    ::RegCloseKey(key);
    if (st != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ))
        return {};
    text[sizeof(text)-1] = '\0';
    return text;
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


    {
        unsigned char binary_source[16] = {
            0x10,0x21,0x32,0x43,0x54,0x65,0x76,0x87,
            0x98,0xA9,0xBA,0xCB,0xDC,0xED,0xFE,0x0F
        };

        const long old_ptr = old_dm.ReadDataAddrToBin(
            pid, reinterpret_cast<LONGLONG>(binary_source), 16);
        std::vector<unsigned char> old_copy(16, 0);
        if (old_ptr) {
            const auto *p = reinterpret_cast<const unsigned char *>(
                static_cast<ULONG_PTR>(static_cast<unsigned long>(old_ptr)));
            std::memcpy(old_copy.data(), p, old_copy.size());
        }

        const long new_ptr = new_dm.ReadDataAddrToBin(
            pid, reinterpret_cast<LONGLONG>(binary_source), 16);
        std::vector<unsigned char> new_copy(16, 0);
        if (new_ptr) {
            const auto *p = reinterpret_cast<const unsigned char *>(
                static_cast<ULONG_PTR>(static_cast<unsigned long>(new_ptr)));
            std::memcpy(new_copy.data(), p, new_copy.size());
        }

        eq_num("ReadDataAddrToBin-success", old_ptr != 0 ? 1 : 0, new_ptr != 0 ? 1 : 0);
        eq_num("ReadDataAddrToBin-bytes",
               std::memcmp(old_copy.data(), new_copy.data(), old_copy.size()), 0);

        auto addr_text = [](const void *ptr) {
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "%lX",
                static_cast<unsigned long>(reinterpret_cast<ULONG_PTR>(ptr)));
            return std::string(buf);
        };

        const std::string source_expr = addr_text(binary_source);
        const long old_ptr2 = old_dm.ReadDataToBin(pid, source_expr.c_str(), 16);
        std::fill(old_copy.begin(), old_copy.end(), 0);
        if (old_ptr2) {
            const auto *p = reinterpret_cast<const unsigned char *>(
                static_cast<ULONG_PTR>(static_cast<unsigned long>(old_ptr2)));
            std::memcpy(old_copy.data(), p, old_copy.size());
        }

        const long new_ptr2 = new_dm.ReadDataToBin(pid, source_expr.c_str(), 16);
        std::fill(new_copy.begin(), new_copy.end(), 0);
        if (new_ptr2) {
            const auto *p = reinterpret_cast<const unsigned char *>(
                static_cast<ULONG_PTR>(static_cast<unsigned long>(new_ptr2)));
            std::memcpy(new_copy.data(), p, new_copy.size());
        }
        eq_num("ReadDataToBin-success", old_ptr2 != 0 ? 1 : 0, new_ptr2 != 0 ? 1 : 0);
        eq_num("ReadDataToBin-bytes",
               std::memcmp(old_copy.data(), new_copy.data(), old_copy.size()), 0);

        unsigned char old_target[16]{};
        unsigned char new_target[16]{};
        const long source_ptr = static_cast<long>(
            reinterpret_cast<INT_PTR>(binary_source));

        eq_num("WriteDataAddrFromBin-ret",
            old_dm.WriteDataAddrFromBin(
                pid, reinterpret_cast<LONGLONG>(old_target), source_ptr, 16),
            new_dm.WriteDataAddrFromBin(
                pid, reinterpret_cast<LONGLONG>(new_target), source_ptr, 16));
        eq_num("WriteDataAddrFromBin-bytes",
            std::memcmp(old_target, new_target, sizeof(old_target)), 0);

        std::memset(old_target, 0, sizeof(old_target));
        std::memset(new_target, 0, sizeof(new_target));
        const std::string old_target_expr = addr_text(old_target);
        const std::string new_target_expr = addr_text(new_target);
        eq_num("WriteDataFromBin-ret",
            old_dm.WriteDataFromBin(pid, old_target_expr.c_str(), source_ptr, 16),
            new_dm.WriteDataFromBin(pid, new_target_expr.c_str(), source_ptr, 16));
        eq_num("WriteDataFromBin-bytes",
            std::memcmp(old_target, new_target, sizeof(old_target)), 0);
    }

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


    const DWORD input_tid = ::GetWindowThreadProcessId(hwnd, nullptr);
    const std::string layout_text =
        current_layout_text_for_thread(input_tid);
    if (!layout_text.empty()) {
        eq_num("FindInputMethod-current",
               old_dm.FindInputMethod(layout_text.c_str()),
               new_dm.FindInputMethod(layout_text.c_str()));
        eq_num("CheckInputMethod-current",
               old_dm.CheckInputMethod(h, layout_text.c_str()),
               new_dm.CheckInputMethod(h, layout_text.c_str()));
        eq_num("ActiveInputMethod-current",
               old_dm.ActiveInputMethod(h, layout_text.c_str()),
               new_dm.ActiveInputMethod(h, layout_text.c_str()));
        eq_num("CheckInputMethod-after-active",
               old_dm.CheckInputMethod(h, layout_text.c_str()),
               new_dm.CheckInputMethod(h, layout_text.c_str()));
    }

    const char *missing_input =
        "__hcbyj_parity_nonexistent_input_method__";
    eq_num("FindInputMethod-missing",
           old_dm.FindInputMethod(missing_input),
           new_dm.FindInputMethod(missing_input));
    eq_num("CheckInputMethod-missing",
           old_dm.CheckInputMethod(h, missing_input),
           new_dm.CheckInputMethod(h, missing_input));
    eq_num("ActiveInputMethod-missing",
           old_dm.ActiveInputMethod(h, missing_input),
           new_dm.ActiveInputMethod(h, missing_input));

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


void test_audio_aero(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const auto root = make_root();
    const auto wav = root / "silence.wav";
    const auto missing = root / "does-not-exist.wav";
    if (!write_silent_wav(wav)) {
        fail("write_silent_wav", "success", "failed");
        return;
    }

    eq_num("Play-missing",
           old_dm.Play(missing.string().c_str()),
           new_dm.Play(missing.string().c_str()));

    const long old_id = old_dm.Play(wav.string().c_str());
    const long new_id = new_dm.Play(wav.string().c_str());
    eq_num("Play-success-nonzero", old_id != 0 ? 1 : 0, new_id != 0 ? 1 : 0);
    eq_num("Play-id", old_id, new_id);

    if (old_id != 0 && new_id != 0)
        eq_num("Stop-valid", old_dm.Stop(old_id), new_dm.Stop(new_id));

    eq_num("Stop-invalid", old_dm.Stop(0), new_dm.Stop(0));

    BOOL enabled = FALSE;
    if (SUCCEEDED(::DwmIsCompositionEnabled(&enabled))) {
        const long current = enabled ? 1 : 0;
        eq_num("SetAero-current",
               old_dm.SetAero(current),
               new_dm.SetAero(current));
    }
    eq_num("SetAero-invalid-low", old_dm.SetAero(-1), new_dm.SetAero(-1));
    eq_num("SetAero-invalid-high", old_dm.SetAero(2), new_dm.SetAero(2));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}


void test_picture_cache_and_find(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const auto root = make_root();
    const auto a = root / "pic_a.bmp";
    const auto b = root / "pic_b.bmp";
    const auto screen = root / "screen_fixture.bmp";

    if (!write_test_bmp24(a, 4, 3) ||
        !write_test_bmp24(b, 5, 4)) {
        fail("write_test_bmp24", "success", "failed");
        return;
    }

    eq_num("EnablePicCache-1",
           old_dm.EnablePicCache(1),
           new_dm.EnablePicCache(1));

    const std::string wildcard =
        (root / "pic_?.bmp").string();
    {
        const char *oa = old_dm.MatchPicName(wildcard.c_str());
        const char *nb = new_dm.MatchPicName(wildcard.c_str());
        eq_str("MatchPicName",
               oa ? std::string(oa) : "<null>",
               nb ? std::string(nb) : "<null>");
    }

    eq_num("LoadPic",
           old_dm.LoadPic(a.string().c_str()),
           new_dm.LoadPic(a.string().c_str()));

    {
        const char *oa = old_dm.GetPicSize(a.string().c_str());
        const char *nb = new_dm.GetPicSize(a.string().c_str());
        eq_str("GetPicSize-loaded",
               oa ? std::string(oa) : "<null>",
               nb ? std::string(nb) : "<null>");
    }

    // Change the file while it is cached. Both implementations should still
    // see the cached dimensions until FreePic releases it.
    if (write_test_bmp24(a, 6, 2)) {
        const char *oa = old_dm.GetPicSize(a.string().c_str());
        const char *nb = new_dm.GetPicSize(a.string().c_str());
        eq_str("GetPicSize-cache-stale",
               oa ? std::string(oa) : "<null>",
               nb ? std::string(nb) : "<null>");
    }

    eq_num("FreePic",
           old_dm.FreePic(a.string().c_str()),
           new_dm.FreePic(a.string().c_str()));

    {
        const char *oa = old_dm.GetPicSize(a.string().c_str());
        const char *nb = new_dm.GetPicSize(a.string().c_str());
        eq_str("GetPicSize-after-free",
               oa ? std::string(oa) : "<null>",
               nb ? std::string(nb) : "<null>");
    }

    eq_num("EnablePicCache-0",
           old_dm.EnablePicCache(0),
           new_dm.EnablePicCache(0));
    eq_num("EnablePicCache-1-restore",
           old_dm.EnablePicCache(1),
           new_dm.EnablePicCache(1));

    // Build a deterministic find-picture fixture from the current display.
    // The fixture is exactly the search rectangle, so an unchanged screen has
    // only one possible full-template placement: (0,0).
    if (new_dm.Capture(0, 0, 31, 31, screen.string().c_str()) == 1) {
        long ox=-1, oy=-1, nx=-1, ny=-1;
        const long orv = old_dm.FindPic(
            0, 0, 31, 31, screen.string().c_str(), "000000",
            1.0, 0, &ox, &oy);
        const long nrv = new_dm.FindPic(
            0, 0, 31, 31, screen.string().c_str(), "000000",
            1.0, 0, &nx, &ny);
        eq_num("FindPic-ret", orv, nrv);
        eq_num("FindPic-x", ox, nx);
        eq_num("FindPic-y", oy, ny);

        {
            const char *oa = old_dm.FindPicE(
                0,0,31,31,screen.string().c_str(),"000000",1.0,0);
            const char *nb = new_dm.FindPicE(
                0,0,31,31,screen.string().c_str(),"000000",1.0,0);
            eq_str("FindPicE",
                   oa ? std::string(oa) : "<null>",
                   nb ? std::string(nb) : "<null>");
        }
        {
            const char *oa = old_dm.FindPicEx(
                0,0,31,31,screen.string().c_str(),"000000",1.0,0);
            const char *nb = new_dm.FindPicEx(
                0,0,31,31,screen.string().c_str(),"000000",1.0,0);
            eq_str("FindPicEx",
                   oa ? std::string(oa) : "<null>",
                   nb ? std::string(nb) : "<null>");
        }

        ox=oy=nx=ny=-1;
        const char *os = old_dm.FindPicS(
            0,0,31,31,screen.string().c_str(),"000000",1.0,0,&ox,&oy);
        const std::string old_s = os ? os : "<null>";
        const char *ns = new_dm.FindPicS(
            0,0,31,31,screen.string().c_str(),"000000",1.0,0,&nx,&ny);
        const std::string new_s = ns ? ns : "<null>";
        eq_str("FindPicS-name", old_s, new_s);
        eq_num("FindPicS-x", ox, nx);
        eq_num("FindPicS-y", oy, ny);

        {
            const char *oa = old_dm.FindPicExS(
                0,0,31,31,screen.string().c_str(),"000000",1.0,0);
            const char *nb = new_dm.FindPicExS(
                0,0,31,31,screen.string().c_str(),"000000",1.0,0);
            eq_str("FindPicExS",
                   oa ? std::string(oa) : "<null>",
                   nb ? std::string(nb) : "<null>");
        }

        ox=oy=nx=ny=-1;
        eq_num("FindPicSim-ret",
               old_dm.FindPicSim(
                   0,0,31,31,screen.string().c_str(),"000000",100,0,&ox,&oy),
               new_dm.FindPicSim(
                   0,0,31,31,screen.string().c_str(),"000000",100,0,&nx,&ny));
        eq_num("FindPicSim-x", ox, nx);
        eq_num("FindPicSim-y", oy, ny);

        {
            const char *oa = old_dm.FindPicSimE(
                0,0,31,31,screen.string().c_str(),"000000",100,0);
            const char *nb = new_dm.FindPicSimE(
                0,0,31,31,screen.string().c_str(),"000000",100,0);
            eq_str("FindPicSimE",
                   oa ? std::string(oa) : "<null>",
                   nb ? std::string(nb) : "<null>");
        }
        {
            const char *oa = old_dm.FindPicSimEx(
                0,0,31,31,screen.string().c_str(),"000000",100,0);
            const char *nb = new_dm.FindPicSimEx(
                0,0,31,31,screen.string().c_str(),"000000",100,0);
            eq_str("FindPicSimEx",
                   oa ? std::string(oa) : "<null>",
                   nb ? std::string(nb) : "<null>");
        }


        const auto bmp_bytes = read_all_bytes(screen);
        if (!bmp_bytes.empty() &&
            bmp_bytes.size() <= static_cast<size_t>(LONG_MAX)) {
            const long bmp_addr = static_cast<long>(
                reinterpret_cast<INT_PTR>(bmp_bytes.data()));
            const long bmp_size = static_cast<long>(bmp_bytes.size());

            const char *old_info_ptr =
                old_dm.AppendPicAddr("", bmp_addr, bmp_size);
            const std::string old_info =
                old_info_ptr ? old_info_ptr : "<null>";
            const char *new_info_ptr =
                new_dm.AppendPicAddr("", bmp_addr, bmp_size);
            const std::string new_info =
                new_info_ptr ? new_info_ptr : "<null>";
            eq_str("AppendPicAddr", old_info, new_info);

            const char *old_info2_ptr =
                old_dm.AppendPicAddr(old_info.c_str(), bmp_addr, bmp_size);
            const std::string old_info2 =
                old_info2_ptr ? old_info2_ptr : "<null>";
            const char *new_info2_ptr =
                new_dm.AppendPicAddr(new_info.c_str(), bmp_addr, bmp_size);
            const std::string new_info2 =
                new_info2_ptr ? new_info2_ptr : "<null>";
            eq_str("AppendPicAddr-second", old_info2, new_info2);

            eq_num("LoadPicByte",
                   old_dm.LoadPicByte(bmp_addr, bmp_size, "memory_fixture.bmp"),
                   new_dm.LoadPicByte(bmp_addr, bmp_size, "memory_fixture.bmp"));
            {
                const char *oa =
                    old_dm.GetPicSize("memory_fixture.bmp");
                const char *nb =
                    new_dm.GetPicSize("memory_fixture.bmp");
                eq_str("LoadPicByte-GetPicSize",
                       oa ? std::string(oa) : "<null>",
                       nb ? std::string(nb) : "<null>");
            }

            ox=oy=nx=ny=-1;
            eq_num("FindPicMem-ret",
                   old_dm.FindPicMem(
                       0,0,31,31,old_info.c_str(),"000000",1.0,0,&ox,&oy),
                   new_dm.FindPicMem(
                       0,0,31,31,new_info.c_str(),"000000",1.0,0,&nx,&ny));
            eq_num("FindPicMem-x", ox, nx);
            eq_num("FindPicMem-y", oy, ny);

            {
                const char *oa = old_dm.FindPicMemE(
                    0,0,31,31,old_info.c_str(),"000000",1.0,0);
                const char *nb = new_dm.FindPicMemE(
                    0,0,31,31,new_info.c_str(),"000000",1.0,0);
                eq_str("FindPicMemE",
                       oa ? std::string(oa) : "<null>",
                       nb ? std::string(nb) : "<null>");
            }
            {
                const char *oa = old_dm.FindPicMemEx(
                    0,0,31,31,old_info.c_str(),"000000",1.0,0);
                const char *nb = new_dm.FindPicMemEx(
                    0,0,31,31,new_info.c_str(),"000000",1.0,0);
                eq_str("FindPicMemEx",
                       oa ? std::string(oa) : "<null>",
                       nb ? std::string(nb) : "<null>");
            }

            ox=oy=nx=ny=-1;
            eq_num("FindPicSimMem-ret",
                   old_dm.FindPicSimMem(
                       0,0,31,31,old_info.c_str(),"000000",100,0,&ox,&oy),
                   new_dm.FindPicSimMem(
                       0,0,31,31,new_info.c_str(),"000000",100,0,&nx,&ny));
            eq_num("FindPicSimMem-x", ox, nx);
            eq_num("FindPicSimMem-y", oy, ny);

            {
                const char *oa = old_dm.FindPicSimMemE(
                    0,0,31,31,old_info.c_str(),"000000",100,0);
                const char *nb = new_dm.FindPicSimMemE(
                    0,0,31,31,new_info.c_str(),"000000",100,0);
                eq_str("FindPicSimMemE",
                       oa ? std::string(oa) : "<null>",
                       nb ? std::string(nb) : "<null>");
            }
            {
                const char *oa = old_dm.FindPicSimMemEx(
                    0,0,31,31,old_info.c_str(),"000000",100,0);
                const char *nb = new_dm.FindPicSimMemEx(
                    0,0,31,31,new_info.c_str(),"000000",100,0);
                eq_str("FindPicSimMemEx",
                       oa ? std::string(oa) : "<null>",
                       nb ? std::string(nb) : "<null>");
            }

            eq_num("FreePic-memory",
                   old_dm.FreePic("memory_fixture.bmp"),
                   new_dm.FreePic("memory_fixture.bmp"));
        } else {
            fail("memory-picture-fixture", "valid BMP bytes", "empty");
        }
    } else {
        fail("FindPic-fixture-capture", "1", "0");
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}


void test_encoded_capture(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const auto root = make_root();
    const auto old_png = root / "old.png";
    const auto new_png = root / "new.png";
    const auto old_jpg = root / "old.jpg";
    const auto new_jpg = root / "new.jpg";
    const auto old_bmp = root / "old_from_png.bmp";
    const auto new_bmp = root / "new_from_png.bmp";

    eq_num("CapturePng-ret",
           old_dm.CapturePng(0,0,15,15,old_png.string().c_str()),
           new_dm.CapturePng(0,0,15,15,new_png.string().c_str()));
    eq_num("CaptureJpg-ret",
           old_dm.CaptureJpg(0,0,15,15,old_jpg.string().c_str(),80),
           new_dm.CaptureJpg(0,0,15,15,new_jpg.string().c_str(),80));

    const auto op = read_prefix(old_png, 8);
    const auto np = read_prefix(new_png, 8);
    const std::vector<unsigned char> png_sig{137,80,78,71,13,10,26,10};
    eq_num("CapturePng-old-format", op == png_sig ? 1 : 0, 1);
    eq_num("CapturePng-new-format", np == png_sig ? 1 : 0, 1);

    const auto oj = read_prefix(old_jpg, 2);
    const auto nj = read_prefix(new_jpg, 2);
    eq_num("CaptureJpg-old-format",
           oj.size()==2 && oj[0]==0xff && oj[1]==0xd8 ? 1 : 0, 1);
    eq_num("CaptureJpg-new-format",
           nj.size()==2 && nj[0]==0xff && nj[1]==0xd8 ? 1 : 0, 1);

    eq_num("CaptureJpg-quality0",
           old_dm.CaptureJpg(0,0,15,15,(root/"bad_old.jpg").string().c_str(),0),
           new_dm.CaptureJpg(0,0,15,15,(root/"bad_new.jpg").string().c_str(),0));
    eq_num("CaptureJpg-quality101",
           old_dm.CaptureJpg(0,0,15,15,(root/"bad_old2.jpg").string().c_str(),101),
           new_dm.CaptureJpg(0,0,15,15,(root/"bad_new2.jpg").string().c_str(),101));

    eq_num("ImageToBmp-ret",
           old_dm.ImageToBmp(old_png.string().c_str(), old_bmp.string().c_str()),
           new_dm.ImageToBmp(new_png.string().c_str(), new_bmp.string().c_str()));

    const auto om = read_bmp_meta(old_bmp);
    const auto nm = read_bmp_meta(new_bmp);
    eq_num("ImageToBmp-old-valid", om.ok ? 1 : 0, 1);
    eq_num("ImageToBmp-new-valid", nm.ok ? 1 : 0, 1);
    if (om.ok && nm.ok) {
        eq_num("ImageToBmp-width", om.width, nm.width);
        eq_num("ImageToBmp-height", om.height, nm.height);
        eq_num("ImageToBmp-bitcount", om.bit_count, nm.bit_count);
        eq_num("ImageToBmp-24bit", nm.bit_count, 24u);
    }

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}


void test_ocr_state(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    eq_num("GetNowDict-default", old_dm.GetNowDict(), new_dm.GetNowDict());

    for (long index : {-1L, 0L, 1L, 42L, 99L, 100L}) {
        const long old_ret = old_dm.UseDict(index);
        const long new_ret = new_dm.UseDict(index);
        eq_num(("UseDict-ret-" + std::to_string(index)).c_str(), old_ret, new_ret);
        eq_num(("UseDict-state-" + std::to_string(index)).c_str(),
               old_dm.GetNowDict(), new_dm.GetNowDict());
    }

    for (long en : {-1L, 0L, 1L, 2L}) {
        eq_num(("EnableShareDict-" + std::to_string(en)).c_str(),
               old_dm.EnableShareDict(en), new_dm.EnableShareDict(en));
        eq_num(("SetExactOcr-" + std::to_string(en)).c_str(),
               old_dm.SetExactOcr(en), new_dm.SetExactOcr(en));
    }

    for (long v : {-1L, 0L, 1L, 5L, 10L, 100L}) {
        eq_num(("SetMinRowGap-" + std::to_string(v)).c_str(),
               old_dm.SetMinRowGap(v), new_dm.SetMinRowGap(v));
        eq_num(("SetMinColGap-" + std::to_string(v)).c_str(),
               old_dm.SetMinColGap(v), new_dm.SetMinColGap(v));
        eq_num(("SetWordGap-" + std::to_string(v)).c_str(),
               old_dm.SetWordGap(v), new_dm.SetWordGap(v));
        eq_num(("SetWordLineHeight-" + std::to_string(v)).c_str(),
               old_dm.SetWordLineHeight(v), new_dm.SetWordLineHeight(v));
        eq_num(("SetRowGapNoDict-" + std::to_string(v)).c_str(),
               old_dm.SetRowGapNoDict(v), new_dm.SetRowGapNoDict(v));
        eq_num(("SetColGapNoDict-" + std::to_string(v)).c_str(),
               old_dm.SetColGapNoDict(v), new_dm.SetColGapNoDict(v));
        eq_num(("SetWordGapNoDict-" + std::to_string(v)).c_str(),
               old_dm.SetWordGapNoDict(v), new_dm.SetWordGapNoDict(v));
        eq_num(("SetWordLineHeightNoDict-" + std::to_string(v)).c_str(),
               old_dm.SetWordLineHeightNoDict(v), new_dm.SetWordLineHeightNoDict(v));
    }
}


void test_critical_and_password(
    const char *legacy_path, LegacyRvaClient &old_dm, dmsoft &new_dm) {
    eq_num("SetPicPwd-empty",
           old_dm.SetPicPwd(""), new_dm.SetPicPwd(""));
    eq_num("SetPicPwd-value",
           old_dm.SetPicPwd("pic-pass"), new_dm.SetPicPwd("pic-pass"));
    eq_num("SetDictPwd-empty",
           old_dm.SetDictPwd(""), new_dm.SetDictPwd(""));
    eq_num("SetDictPwd-value",
           old_dm.SetDictPwd("dict-pass"), new_dm.SetDictPwd("dict-pass"));
    eq_num("SetParam64ToPointer",
           old_dm.SetParam64ToPointer(), new_dm.SetParam64ToPointer());

    LegacyRvaClient old2(legacy_path);
    dmsoft new2;

    eq_num("InitCri", old_dm.InitCri(), new_dm.InitCri());

    const long old_a1 = old_dm.EnterCri();
    const long new_a1 = new_dm.EnterCri();
    eq_num("EnterCri-A-first", old_a1, new_a1);

    const long old_a2 = old_dm.EnterCri();
    const long new_a2 = new_dm.EnterCri();
    eq_num("EnterCri-A-second", old_a2, new_a2);

    const long old_b1 = old2.EnterCri();
    const long new_b1 = new2.EnterCri();
    eq_num("EnterCri-B-while-A-owned", old_b1, new_b1);

    eq_num("LeaveCri-B-not-owner", old2.LeaveCri(), new2.LeaveCri());

    const long old_b2 = old2.EnterCri();
    const long new_b2 = new2.EnterCri();
    eq_num("EnterCri-B-after-foreign-leave", old_b2, new_b2);

    eq_num("LeaveCri-A", old_dm.LeaveCri(), new_dm.LeaveCri());

    const long old_b3 = old2.EnterCri();
    const long new_b3 = new2.EnterCri();
    eq_num("EnterCri-B-after-A-leave", old_b3, new_b3);

    eq_num("LeaveCri-B", old2.LeaveCri(), new2.LeaveCri());

    // InitCri forcibly resets the shared signal even if an object currently owns it.
    eq_num("EnterCri-A-before-reset", old_dm.EnterCri(), new_dm.EnterCri());
    eq_num("InitCri-force-reset", old2.InitCri(), new2.InitCri());
    eq_num("EnterCri-B-after-reset", old2.EnterCri(), new2.EnterCri());
    eq_num("LeaveCri-B-after-reset", old2.LeaveCri(), new2.LeaveCri());
}


void test_system_paths_and_commandline(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    for (long type = 0; type <= 4; ++type) {
        const char *a = old_dm.GetDir(type);
        const char *b = new_dm.GetDir(type);
        eq_str(("GetDir-" + std::to_string(type)).c_str(),
               a ? std::string(a) : "<null>",
               b ? std::string(b) : "<null>");
    }

    eq_num("IsSurrpotVt", old_dm.IsSurrpotVt(), new_dm.IsSurrpotVt());

    const long pid = static_cast<long>(::GetCurrentProcessId());
    old_dm.SetMemoryHwndAsProcessId(1);
    new_dm.SetMemoryHwndAsProcessId(1);
    {
        const char *a = old_dm.GetCommandLine(pid);
        const char *b = new_dm.GetCommandLine(pid);
        eq_str("GetCommandLine-pid",
               a ? std::string(a) : "<null>",
               b ? std::string(b) : "<null>");
    }
}


void test_memory_search(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const long pid = static_cast<long>(::GetCurrentProcessId());
    old_dm.SetMemoryHwndAsProcessId(1);
    new_dm.SetMemoryHwndAsProcessId(1);

    constexpr SIZE_T kSize = 0x10000;
    auto *mem = static_cast<unsigned char *>(
        ::VirtualAlloc(nullptr, kSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!mem) {
        fail("memory-search-alloc", "success", "failed");
        return;
    }
    std::memset(mem, 0xCC, kSize);

    auto range_string = [&](SIZE_T begin_off, SIZE_T end_off) {
        char buf[96]{};
        std::snprintf(
            buf, sizeof(buf), "%llX-%llX",
            static_cast<unsigned long long>(
                reinterpret_cast<ULONG_PTR>(mem + begin_off)),
            static_cast<unsigned long long>(
                reinterpret_cast<ULONG_PTR>(mem + end_off)));
        return std::string(buf);
    };

    const std::string whole = range_string(0, kSize - 1);

    *reinterpret_cast<std::int32_t *>(mem + 0x100) = 0x12345678;
    *reinterpret_cast<std::int32_t *>(mem + 0x240) = 0x12345678;
    *reinterpret_cast<std::int32_t *>(mem + 0x381) = 0x12345678;

    *reinterpret_cast<float *>(mem + 0x500) = 12345.75f;
    *reinterpret_cast<float *>(mem + 0x620) = 12345.75f;

    *reinterpret_cast<double *>(mem + 0x800) = 98765.125;
    *reinterpret_cast<double *>(mem + 0x940) = 98765.125;

    const unsigned char pattern1[] = {0x12,0x34,0x56,0x78,0x9A,0xBC};
    const unsigned char pattern2[] = {0x12,0x34,0xA1,0xB2,0x9A,0xBC};
    std::memcpy(mem + 0xB00, pattern1, sizeof(pattern1));
    std::memcpy(mem + 0xC20, pattern2, sizeof(pattern2));

    const char ascii_text[] = "HCByj-Memory-Unique-ASCII";
    std::memcpy(mem + 0xD00, ascii_text, sizeof(ascii_text));

    const wchar_t wide_text[] = L"HCByjWideUnique";
    std::memcpy(mem + 0xE00, wide_text, sizeof(wide_text));

    auto cmp_call = [&](const char *name, const char *a, const char *b) {
        eq_str(name,
               a ? std::string(a) : "<null>",
               b ? std::string(b) : "<null>");
    };

    {
        const char *a = old_dm.FindInt(pid, whole.c_str(), 0x12345678, 0x12345678, 0);
        const std::string old_result = a ? a : "";
        const char *b = new_dm.FindInt(pid, whole.c_str(), 0x12345678, 0x12345678, 0);
        const std::string new_result = b ? b : "";
        eq_str("FindInt", old_result, new_result);

        const char *a2 = old_dm.FindInt(pid, old_result.c_str(), 0x12345678, 0x12345678, 0);
        const char *b2 = new_dm.FindInt(pid, new_result.c_str(), 0x12345678, 0x12345678, 0);
        cmp_call("FindInt-second-scan", a2, b2);
    }

    {
        const char *a = old_dm.FindIntEx(
            pid, whole.c_str(), 0x12345678, 0x12345678, 0, 4, 0, 1);
        const char *b = new_dm.FindIntEx(
            pid, whole.c_str(), 0x12345678, 0x12345678, 0, 4, 0, 1);
        cmp_call("FindIntEx-step-mode", a, b);
    }

    {
        const char *a = old_dm.FindFloat(
            pid, whole.c_str(), 12345.75f, 12345.75f);
        const char *b = new_dm.FindFloat(
            pid, whole.c_str(), 12345.75f, 12345.75f);
        cmp_call("FindFloat", a, b);

        a = old_dm.FindFloatEx(
            pid, whole.c_str(), 12345.74f, 12345.76f, 2, 0, 0);
        b = new_dm.FindFloatEx(
            pid, whole.c_str(), 12345.74f, 12345.76f, 2, 0, 0);
        cmp_call("FindFloatEx", a, b);
    }

    {
        const char *a = old_dm.FindDouble(
            pid, whole.c_str(), 98765.125, 98765.125);
        const char *b = new_dm.FindDouble(
            pid, whole.c_str(), 98765.125, 98765.125);
        cmp_call("FindDouble", a, b);

        a = old_dm.FindDoubleEx(
            pid, whole.c_str(), 98765.0, 98766.0, 8, 0, 16);
        b = new_dm.FindDoubleEx(
            pid, whole.c_str(), 98765.0, 98766.0, 8, 0, 16);
        cmp_call("FindDoubleEx", a, b);
    }

    {
        const char *a = old_dm.FindData(
            pid, whole.c_str(), "12 34 ?? ?? 9A BC");
        const char *b = new_dm.FindData(
            pid, whole.c_str(), "12 34 ?? ?? 9A BC");
        cmp_call("FindData-wildcard", a, b);

        a = old_dm.FindDataEx(
            pid, whole.c_str(), "12 34 56 78 9A BC", 2, 0, 0);
        b = new_dm.FindDataEx(
            pid, whole.c_str(), "12 34 56 78 9A BC", 2, 0, 0);
        cmp_call("FindDataEx", a, b);
    }

    {
        const char *a = old_dm.FindString(
            pid, whole.c_str(), ascii_text, 0);
        const char *b = new_dm.FindString(
            pid, whole.c_str(), ascii_text, 0);
        cmp_call("FindString-ascii", a, b);

        a = old_dm.FindStringEx(
            pid, whole.c_str(), "HCByjWideUnique", 1, 2, 0, 0);
        b = new_dm.FindStringEx(
            pid, whole.c_str(), "HCByjWideUnique", 1, 2, 0, 0);
        cmp_call("FindStringEx-unicode", a, b);
    }

    {
        const auto root = make_root();
        const auto old_file = root / "old_memory_result.dat";
        const auto new_file = root / "new_memory_result.dat";

        eq_num("SetMemoryFindResultToFile-old-new",
               old_dm.SetMemoryFindResultToFile(old_file.string().c_str()),
               new_dm.SetMemoryFindResultToFile(new_file.string().c_str()));

        const char *a = old_dm.FindInt(
            pid, whole.c_str(), 0x12345678, 0x12345678, 0);
        const std::string old_return = a ? a : "";
        const char *b = new_dm.FindInt(
            pid, whole.c_str(), 0x12345678, 0x12345678, 0);
        const std::string new_return = b ? b : "";
        eq_str("FindInt-result-file-return", old_return, new_return);

        auto read_file = [](const std::filesystem::path &path) {
            std::ifstream in(path, std::ios::binary);
            return std::string(
                std::istreambuf_iterator<char>(in),
                std::istreambuf_iterator<char>());
        };
        eq_str("FindInt-result-file-content",
               read_file(old_file), read_file(new_file));

        const char *a2 = old_dm.FindInt(
            pid, "ignored-address-list", 0x12345678, 0x12345678, 0);
        const char *b2 = new_dm.FindInt(
            pid, "ignored-address-list", 0x12345678, 0x12345678, 0);
        cmp_call("FindInt-result-file-second-scan", a2, b2);

        eq_num("SetMemoryFindResultToFile-disable",
               old_dm.SetMemoryFindResultToFile(""),
               new_dm.SetMemoryFindResultToFile(""));

        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    ::VirtualFree(mem, 0, MEM_RELEASE);
}


void test_screen_buffers(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const long ox1 = 0, oy1 = 0, ox2 = 1, oy2 = 1;

    const long old_ptr = old_dm.GetScreenData(ox1, oy1, ox2, oy2);
    const long new_ptr = new_dm.GetScreenData(ox1, oy1, ox2, oy2);
    eq_num("GetScreenData-nonzero",
           old_ptr != 0 ? 1 : 0,
           new_ptr != 0 ? 1 : 0);
    if (old_ptr && new_ptr) {
        DWORD old_pixels[4]{};
        DWORD new_pixels[4]{};
        SIZE_T got1 = 0, got2 = 0;
        const BOOL r1 = ::ReadProcessMemory(
            ::GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(
                static_cast<ULONG_PTR>(
                    static_cast<unsigned long>(old_ptr))),
            old_pixels, sizeof(old_pixels), &got1);
        const BOOL r2 = ::ReadProcessMemory(
            ::GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(
                static_cast<ULONG_PTR>(
                    static_cast<unsigned long>(new_ptr))),
            new_pixels, sizeof(new_pixels), &got2);
        eq_num("GetScreenData-readable", r1 ? 1 : 0, r2 ? 1 : 0);
        if (r1 && r2 && got1 == sizeof(old_pixels) &&
            got2 == sizeof(new_pixels)) {
            eq_num("GetScreenData-pixels",
                   std::memcmp(old_pixels,new_pixels,sizeof(old_pixels)),0);
        }
    }

    long old_bmp = 0, old_size = 0;
    long new_bmp = 0, new_size = 0;
    eq_num("GetScreenDataBmp-ret",
           old_dm.GetScreenDataBmp(
               ox1,oy1,ox2,oy2,&old_bmp,&old_size),
           new_dm.GetScreenDataBmp(
               ox1,oy1,ox2,oy2,&new_bmp,&new_size));
    eq_num("GetScreenDataBmp-size", old_size, new_size);
    eq_num("GetScreenDataBmp-nonzero",
           old_bmp != 0 ? 1 : 0,
           new_bmp != 0 ? 1 : 0);

    if (old_bmp && new_bmp && old_size > 0 &&
        old_size == new_size) {
        std::vector<unsigned char> old_bytes(
            static_cast<size_t>(old_size));
        std::vector<unsigned char> new_bytes(
            static_cast<size_t>(new_size));
        SIZE_T got1=0, got2=0;
        const BOOL r1=::ReadProcessMemory(
            ::GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(
                static_cast<ULONG_PTR>(
                    static_cast<unsigned long>(old_bmp))),
            old_bytes.data(),old_bytes.size(),&got1);
        const BOOL r2=::ReadProcessMemory(
            ::GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(
                static_cast<ULONG_PTR>(
                    static_cast<unsigned long>(new_bmp))),
            new_bytes.data(),new_bytes.size(),&got2);
        eq_num("GetScreenDataBmp-readable",r1?1:0,r2?1:0);
        if(r1&&r2&&got1==old_bytes.size()&&got2==new_bytes.size()) {
            // Compare BMP structure and dimensions first; pixel bytes should
            // also match for the same stable two-by-two desktop area.
            eq_num("GetScreenDataBmp-bytes",
                   std::memcmp(
                       old_bytes.data(),new_bytes.data(),old_bytes.size()),0);
        }
    }

    eq_num("FreeScreenData",
           old_dm.FreeScreenData(old_bmp),
           new_dm.FreeScreenData(new_bmp));
}


void test_cpu_cursor_display_state(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    eq_num("GetCpuType", old_dm.GetCpuType(), new_dm.GetCpuType());

    {
        const char *a = old_dm.GetCursorSpot();
        const std::string old_spot = a ? a : "<null>";
        const char *b = new_dm.GetCursorSpot();
        const std::string new_spot = b ? b : "<null>";
        eq_str("GetCursorSpot", old_spot, new_spot);
    }

    for (long en : {-1L, 0L, 1L, 2L}) {
        eq_num(
            ("SpeedNormalGraphic-" + std::to_string(en)).c_str(),
            old_dm.SpeedNormalGraphic(en),
            new_dm.SpeedNormalGraphic(en));
    }

    eq_num(
        "LockDisplay-unbound-lock",
        old_dm.LockDisplay(1),
        new_dm.LockDisplay(1));
    eq_num(
        "LockDisplay-unbound-unlock",
        old_dm.LockDisplay(0),
        new_dm.LockDisplay(0));

    const char *cls = "hcbyj_display_state_window";
    WNDCLASSA wc{};
    wc.lpfnWndProc = ParityWndProc;
    wc.hInstance = ::GetModuleHandleA(nullptr);
    wc.lpszClassName = cls;
    ::RegisterClassA(&wc);

    HWND hwnd = ::CreateWindowExA(
        0, cls, "display-state", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        80, 90, 240, 180, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        fail("display-state-window", "created", "failed");
        return;
    }
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);
    ::Sleep(50);

    const long h = static_cast<long>(reinterpret_cast<INT_PTR>(hwnd));

    const long old_bind =
        old_dm.BindWindow(h, "gdi", "normal", "normal", 0);
    const long new_bind =
        new_dm.BindWindow(h, "gdi", "normal", "normal", 0);
    eq_num("display-state-BindWindow", old_bind, new_bind);

    if (old_bind && new_bind) {
        eq_num(
            "LockDisplay-bound-lock",
            old_dm.LockDisplay(1),
            new_dm.LockDisplay(1));
        eq_num(
            "LockDisplay-bound-unlock",
            old_dm.LockDisplay(0),
            new_dm.LockDisplay(0));

        eq_num(
            "IsDisplayDead-t0",
            old_dm.IsDisplayDead(0, 0, 3, 3, 0),
            new_dm.IsDisplayDead(0, 0, 3, 3, 0));

        // A one-second stable region checks timeout behavior without touching
        // any external window.
        eq_num(
            "IsDisplayDead-stable",
            old_dm.IsDisplayDead(0, 0, 3, 3, 1),
            new_dm.IsDisplayDead(0, 0, 3, 3, 1));
    }

    old_dm.UnBindWindow();
    new_dm.UnBindWindow();
    ::DestroyWindow(hwnd);
    ::UnregisterClassA(cls, wc.hInstance);
}


void test_dictionary_core(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const auto root = make_root();
    const auto input = root / "dict_input.txt";
    const auto old_saved = root / "dict_old_saved.txt";
    const auto new_saved = root / "dict_new_saved.txt";

    const std::string e1 =
        "081101BF8020089FD10A21443F85038$记$0.0.33$11";
    const std::string e2 =
        "FFF00A7D49292524A7D402805FFC$回$0.0.29$11";
    const std::string e3 =
        "3F0020087FF08270B9A108268708808$收$0.0.31$11";

    {
        std::ofstream out(input, std::ios::binary | std::ios::trunc);
        out << e1 << "\r\n" << e2;
    }

    eq_num(
        "SetDict",
        old_dm.SetDict(8, input.string().c_str()),
        new_dm.SetDict(8, input.string().c_str()));

    eq_num(
        "GetDictCount-after-set",
        old_dm.GetDictCount(8),
        new_dm.GetDictCount(8));

    for (long i : {-1L, 0L, 1L, 2L}) {
        const char *a = old_dm.GetDict(8, i);
        const std::string oa = a ? a : "<null>";
        const char *b = new_dm.GetDict(8, i);
        const std::string nb = b ? b : "<null>";
        eq_str(
            ("GetDict-" + std::to_string(i)).c_str(),
            oa, nb);
    }

    eq_num(
        "AddDict-valid",
        old_dm.AddDict(8, e3.c_str()),
        new_dm.AddDict(8, e3.c_str()));
    eq_num(
        "GetDictCount-after-add",
        old_dm.GetDictCount(8),
        new_dm.GetDictCount(8));

    eq_num(
        "AddDict-invalid",
        old_dm.AddDict(8, "not-a-dictionary-entry"),
        new_dm.AddDict(8, "not-a-dictionary-entry"));

    eq_num(
        "SaveDict",
        old_dm.SaveDict(8, old_saved.string().c_str()),
        new_dm.SaveDict(8, new_saved.string().c_str()));

    {
        std::ifstream oa(old_saved, std::ios::binary);
        std::ifstream nb(new_saved, std::ios::binary);
        const std::string old_bytes{
            std::istreambuf_iterator<char>(oa),
            std::istreambuf_iterator<char>()};
        const std::string new_bytes{
            std::istreambuf_iterator<char>(nb),
            std::istreambuf_iterator<char>()};
        eq_str("SaveDict-bytes", old_bytes, new_bytes);
    }

    eq_num(
        "ClearDict",
        old_dm.ClearDict(8),
        new_dm.ClearDict(8));
    eq_num(
        "GetDictCount-after-clear",
        old_dm.GetDictCount(8),
        new_dm.GetDictCount(8));

    std::string mem_dict = e2 + "\r\n" + e1;
    eq_num(
        "SetDictMem",
        old_dm.SetDictMem(
            8,
            static_cast<long>(
                reinterpret_cast<INT_PTR>(mem_dict.data())),
            static_cast<long>(mem_dict.size())),
        new_dm.SetDictMem(
            8,
            static_cast<long>(
                reinterpret_cast<INT_PTR>(mem_dict.data())),
            static_cast<long>(mem_dict.size())));

    eq_num(
        "GetDictCount-after-mem",
        old_dm.GetDictCount(8),
        new_dm.GetDictCount(8));

    for (long i = 0; i < 2; ++i) {
        const char *a = old_dm.GetDict(8, i);
        const std::string oa = a ? a : "<null>";
        const char *b = new_dm.GetDict(8, i);
        const std::string nb = b ? b : "<null>";
        eq_str(
            ("GetDict-after-mem-" + std::to_string(i)).c_str(),
            oa, nb);
    }

    old_dm.ClearDict(8);
    new_dm.ClearDict(8);

    std::error_code ec;
    std::filesystem::remove(old_saved, ec);
    std::filesystem::remove(new_saved, ec);
    std::filesystem::remove(input, ec);
}


void test_mac_address(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const char *a = old_dm.GetMac();
    const std::string old_mac = a ? a : "<null>";
    const char *b = new_dm.GetMac();
    const std::string new_mac = b ? b : "<null>";
    eq_str("GetMac", old_mac, new_mac);
}


void test_network_time(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    {
        const char *a = old_dm.GetNetTimeByIp("127.0.0.1");
        const std::string old_fail = a ? a : "<null>";
        const char *b = new_dm.GetNetTimeByIp("127.0.0.1");
        const std::string new_fail = b ? b : "<null>";
        eq_str("GetNetTimeByIp-failure", old_fail, new_fail);
    }

    {
        const char *a = old_dm.GetNetTimeSafe();
        const std::string old_safe = a ? a : "<null>";
        const char *b = new_dm.GetNetTimeSafe();
        const std::string new_safe = b ? b : "<null>";
        eq_str("GetNetTimeSafe", old_safe, new_safe);
    }

    const char *a = old_dm.GetNetTimeByIp(
        "ntp.aliyun.com|ntp.tencent.com|time.windows.com");
    const std::string old_time = a ? a : "<null>";
    const char *b = new_dm.GetNetTimeByIp(
        "ntp.aliyun.com|ntp.tencent.com|time.windows.com");
    const std::string new_time = b ? b : "<null>";

    const std::string failure = "0000-00-00 00:00:00";
    if (old_time == failure || new_time == failure) {
        eq_str("GetNetTimeByIp-online-failure-shape",
               old_time, new_time);
    } else {
        __time64_t old_epoch = 0, new_epoch = 0;
        const bool old_ok =
            parse_beijing_time(old_time, old_epoch);
        const bool new_ok =
            parse_beijing_time(new_time, new_epoch);
        eq_num(
            "GetNetTimeByIp-online-format",
            old_ok ? 1 : 0,
            new_ok ? 1 : 0);
        if (old_ok && new_ok) {
            const __time64_t delta =
                old_epoch > new_epoch
                    ? old_epoch - new_epoch
                    : new_epoch - old_epoch;
            eq_num(
                "GetNetTimeByIp-online-delta<=10s",
                delta <= 10 ? 1 : 0,
                1);
        }
    }

    {
        const char *oa = old_dm.GetNetTime();
        const std::string old_default = oa ? oa : "<null>";
        const char *nb = new_dm.GetNetTime();
        const std::string new_default = nb ? nb : "<null>";

        if (old_default == failure || new_default == failure) {
            eq_num(
                "GetNetTime-valid-or-failure",
                old_default == failure ? 0 : 1,
                new_default == failure ? 0 : 1);
        } else {
            __time64_t old_epoch = 0, new_epoch = 0;
            const bool old_ok =
                parse_beijing_time(old_default, old_epoch);
            const bool new_ok =
                parse_beijing_time(new_default, new_epoch);
            eq_num(
                "GetNetTime-format",
                old_ok ? 1 : 0,
                new_ok ? 1 : 0);
            if (old_ok && new_ok) {
                const __time64_t delta =
                    old_epoch > new_epoch
                        ? old_epoch - new_epoch
                        : new_epoch - old_epoch;
                eq_num(
                    "GetNetTime-delta<=10s",
                    delta <= 10 ? 1 : 0,
                    1);
            }
        }
    }
}


void test_binding_options(LegacyRvaClient &old_dm, dmsoft &new_dm) {
    eq_num("EnableMouseMsg-unbound",
           old_dm.EnableMouseMsg(1),
           new_dm.EnableMouseMsg(1));
    eq_num("EnableKeypadMsg-unbound",
           old_dm.EnableKeypadMsg(1),
           new_dm.EnableKeypadMsg(1));
    eq_num("EnableKeypadPatch-unbound",
           old_dm.EnableKeypadPatch(1),
           new_dm.EnableKeypadPatch(1));
    eq_num("EnableKeypadSync-unbound",
           old_dm.EnableKeypadSync(1, 200),
           new_dm.EnableKeypadSync(1, 200));
    eq_num("EnableMouseSync-unbound",
           old_dm.EnableMouseSync(1, 200),
           new_dm.EnableMouseSync(1, 200));
    eq_num("EnableFakeActive-unbound",
           old_dm.EnableFakeActive(1),
           new_dm.EnableFakeActive(1));
    eq_num("EnableSpeedDx-unbound",
           old_dm.EnableSpeedDx(1),
           new_dm.EnableSpeedDx(1));

    for (long en : {-1L, 0L, 1L, 2L}) {
        eq_num(
            ("SetExitThread-" + std::to_string(en)).c_str(),
            old_dm.SetExitThread(en),
            new_dm.SetExitThread(en));
    }
    old_dm.SetExitThread(0);
    new_dm.SetExitThread(0);

    const char *cls = "hcbyj_binding_options_window";
    WNDCLASSA wc{};
    wc.lpfnWndProc = ParityWndProc;
    wc.hInstance = ::GetModuleHandleA(nullptr);
    wc.lpszClassName = cls;
    ::RegisterClassA(&wc);

    HWND hwnd = ::CreateWindowExA(
        0, cls, "binding-options",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        120, 130, 260, 180,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        fail("binding-options-window", "created", "failed");
        return;
    }
    const long h =
        static_cast<long>(reinterpret_cast<INT_PTR>(hwnd));

    const long old_bind =
        old_dm.BindWindow(h, "gdi", "windows", "windows", 0);
    const long new_bind =
        new_dm.BindWindow(h, "gdi", "windows", "windows", 0);
    eq_num("binding-options-BindWindow", old_bind, new_bind);

    if (old_bind && new_bind) {
        for (long en : {-1L, 0L, 1L, 2L}) {
            eq_num(
                ("EnableMouseMsg-" + std::to_string(en)).c_str(),
                old_dm.EnableMouseMsg(en),
                new_dm.EnableMouseMsg(en));
            eq_num(
                ("EnableKeypadMsg-" + std::to_string(en)).c_str(),
                old_dm.EnableKeypadMsg(en),
                new_dm.EnableKeypadMsg(en));
            eq_num(
                ("EnableKeypadPatch-" + std::to_string(en)).c_str(),
                old_dm.EnableKeypadPatch(en),
                new_dm.EnableKeypadPatch(en));
            eq_num(
                ("EnableFakeActive-" + std::to_string(en)).c_str(),
                old_dm.EnableFakeActive(en),
                new_dm.EnableFakeActive(en));
            eq_num(
                ("EnableSpeedDx-" + std::to_string(en)).c_str(),
                old_dm.EnableSpeedDx(en),
                new_dm.EnableSpeedDx(en));
        }

        const struct SyncCase {
            long enable;
            long timeout;
        } sync_cases[] = {
            {-1, 100}, {0, 0}, {0, 200},
            {1, -1}, {1, 0}, {1, 200}, {2, 200}
        };
        for (const auto &tc : sync_cases) {
            eq_num(
                "EnableKeypadSync",
                old_dm.EnableKeypadSync(tc.enable, tc.timeout),
                new_dm.EnableKeypadSync(tc.enable, tc.timeout));
            eq_num(
                "EnableMouseSync",
                old_dm.EnableMouseSync(tc.enable, tc.timeout),
                new_dm.EnableMouseSync(tc.enable, tc.timeout));
        }

        // Restore neutral defaults before unbind.
        old_dm.EnableMouseMsg(1);
        new_dm.EnableMouseMsg(1);
        old_dm.EnableKeypadMsg(1);
        new_dm.EnableKeypadMsg(1);
        old_dm.EnableKeypadPatch(0);
        new_dm.EnableKeypadPatch(0);
        old_dm.EnableKeypadSync(0, 0);
        new_dm.EnableKeypadSync(0, 0);
        old_dm.EnableMouseSync(0, 0);
        new_dm.EnableMouseSync(0, 0);
        old_dm.EnableFakeActive(0);
        new_dm.EnableFakeActive(0);
        old_dm.EnableSpeedDx(0);
        new_dm.EnableSpeedDx(0);
    }

    old_dm.UnBindWindow();
    new_dm.UnBindWindow();
    ::DestroyWindow(hwnd);
    ::UnregisterClassA(cls, wc.hInstance);
}


void test_real_input_options(
    LegacyRvaClient &old_dm, dmsoft &new_dm) {
    for (long en : {-1L, 0L, 1L, 2L}) {
        eq_num(
            ("EnableRealKeypad-" + std::to_string(en)).c_str(),
            old_dm.EnableRealKeypad(en),
            new_dm.EnableRealKeypad(en));
    }
    old_dm.EnableRealKeypad(0);
    new_dm.EnableRealKeypad(0);

    const struct MouseCase {
        long en;
        long delay;
        long step;
    } cases[] = {
        {-1, 20, 30},
        {0, 0, 0},
        {0, -1, -1},
        {1, 0, 30},
        {1, 20, 0},
        {1, 20, 30},
        {2, 20, 30},
        {3, 20, 30},
        {4, 20, 30},
        {5, 20, 30},
    };

    for (const auto &tc : cases) {
        eq_num(
            ("EnableRealMouse-" +
             std::to_string(tc.en) + "-" +
             std::to_string(tc.delay) + "-" +
             std::to_string(tc.step)).c_str(),
            old_dm.EnableRealMouse(
                tc.en, tc.delay, tc.step),
            new_dm.EnableRealMouse(
                tc.en, tc.delay, tc.step));
    }
    old_dm.EnableRealMouse(0, 0, 0);
    new_dm.EnableRealMouse(0, 0, 0);
}


void test_remote_api_address(
    LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const long pid =
        static_cast<long>(::GetCurrentProcessId());
    old_dm.SetMemoryHwndAsProcessId(1);
    new_dm.SetMemoryHwndAsProcessId(1);

    HMODULE ntdll = ::GetModuleHandleA("ntdll.dll");
    const auto local_ntclose =
        reinterpret_cast<ULONG_PTR>(
            ::GetProcAddress(ntdll, "NtClose"));

    const LONGLONG old_base =
        old_dm.GetModuleBaseAddr(pid, "ntdll.dll");
    const LONGLONG new_base =
        new_dm.GetModuleBaseAddr(pid, "ntdll.dll");
    eq_num(
        "GetRemoteApiAddress-base",
        old_base, new_base);

    const LONGLONG old_addr =
        old_dm.GetRemoteApiAddress(
            pid, old_base, "NtClose");
    const LONGLONG new_addr =
        new_dm.GetRemoteApiAddress(
            pid, new_base, "NtClose");
    eq_num(
        "GetRemoteApiAddress",
        old_addr, new_addr);
    eq_num(
        "GetRemoteApiAddress-actual",
        new_addr,
        static_cast<LONGLONG>(
            local_ntclose));

    eq_num(
        "GetRemoteApiAddress-missing",
        old_dm.GetRemoteApiAddress(
            pid, old_base,
            "__hcbyj_missing_export__"),
        new_dm.GetRemoteApiAddress(
            pid, new_base,
            "__hcbyj_missing_export__"));
}


void test_ocr_core(
    LegacyRvaClient &old_dm, dmsoft &new_dm) {
    const auto root = make_root();
    const auto dict_file = root / "ocr_core_dict.txt";
    const std::string entry =
        "FFFFFFFF8$A$0.0.33$11";
    {
        std::ofstream out(
            dict_file,
            std::ios::binary | std::ios::trunc);
        out << entry;
    }

    eq_num(
        "ocr-core-SetDict",
        old_dm.SetDict(9, dict_file.string().c_str()),
        new_dm.SetDict(9, dict_file.string().c_str()));
    eq_num(
        "ocr-core-UseDict",
        old_dm.UseDict(9),
        new_dm.UseDict(9));

    const char *cls = "hcbyj_ocr_core_window";
    WNDCLASSA wc{};
    wc.lpfnWndProc = ParityWndProc;
    wc.hInstance = ::GetModuleHandleA(nullptr);
    wc.lpszClassName = cls;
    wc.hbrBackground =
        reinterpret_cast<HBRUSH>(
            ::GetStockObject(WHITE_BRUSH));
    ::RegisterClassA(&wc);

    HWND hwnd = ::CreateWindowExA(
        0, cls, "ocr-core",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        180, 180, 180, 120,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        fail("ocr-core-window", "created", "failed");
        return;
    }
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    {
        HDC dc = ::GetDC(hwnd);
        RECT client{};
        ::GetClientRect(hwnd, &client);
        ::FillRect(
            dc, &client,
            reinterpret_cast<HBRUSH>(
                ::GetStockObject(WHITE_BRUSH)));

        HBRUSH black =
            reinterpret_cast<HBRUSH>(
                ::GetStockObject(BLACK_BRUSH));
        RECT a1{10, 10, 13, 21};
        RECT a2{20, 10, 23, 21};
        ::FillRect(dc, &a1, black);
        ::FillRect(dc, &a2, black);
        ::ReleaseDC(hwnd, dc);
    }

    const long h =
        static_cast<long>(
            reinterpret_cast<INT_PTR>(hwnd));
    const long old_bind =
        old_dm.BindWindow(
            h, "gdi", "normal", "normal", 0);
    const long new_bind =
        new_dm.BindWindow(
            h, "gdi", "normal", "normal", 0);
    eq_num(
        "ocr-core-BindWindow",
        old_bind, new_bind);

    if (old_bind && new_bind) {
        const char *oa =
            old_dm.Ocr(
                0, 0, 79, 39,
                "000000-000000", 1.0);
        const std::string old_ocr =
            oa ? oa : "<null>";
        const char *na =
            new_dm.Ocr(
                0, 0, 79, 39,
                "000000-000000", 1.0);
        const std::string new_ocr =
            na ? na : "<null>";
        eq_str(
            "Ocr-controlled",
            old_ocr, new_ocr);

        const char *ob =
            old_dm.OcrEx(
                0, 0, 79, 39,
                "000000-000000", 1.0);
        const std::string old_ex =
            ob ? ob : "<null>";
        const char *nb =
            new_dm.OcrEx(
                0, 0, 79, 39,
                "000000-000000", 1.0);
        const std::string new_ex =
            nb ? nb : "<null>";
        eq_str(
            "OcrEx-controlled",
            old_ex, new_ex);

        long ox = 777, oy = 888;
        long nx = 777, ny = 888;
        const long old_find =
            old_dm.FindStr(
                0, 0, 79, 39,
                "AA|A",
                "000000-000000", 1.0,
                &ox, &oy);
        const long new_find =
            new_dm.FindStr(
                0, 0, 79, 39,
                "AA|A",
                "000000-000000", 1.0,
                &nx, &ny);
        eq_num(
            "FindStr-controlled-ret",
            old_find, new_find);
        eq_num(
            "FindStr-controlled-x",
            ox, nx);
        eq_num(
            "FindStr-controlled-y",
            oy, ny);

        const char *oc =
            old_dm.FindStrEx(
                0, 0, 79, 39,
                "A|AA",
                "000000-000000", 1.0);
        const std::string old_find_ex =
            oc ? oc : "<null>";
        const char *nc =
            new_dm.FindStrEx(
                0, 0, 79, 39,
                "A|AA",
                "000000-000000", 1.0);
        const std::string new_find_ex =
            nc ? nc : "<null>";
        eq_str(
            "FindStrEx-controlled",
            old_find_ex, new_find_ex);
    }

    old_dm.UnBindWindow();
    new_dm.UnBindWindow();
    old_dm.ClearDict(9);
    new_dm.ClearDict(9);

    ::DestroyWindow(hwnd);
    ::UnregisterClassA(cls, wc.hInstance);

    std::error_code ec;
    std::filesystem::remove(dict_file, ec);
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
        if (old_dm.DumpRuntimeVtable("legacy_runtime_vtable_before.csv"))
            std::puts("Wrote legacy_runtime_vtable_before.csv");
        else
            std::puts("Failed to dump legacy runtime vtable before tests.");

        LoadDm(nullptr);
        dmsoft new_dm;

        test_pure(old_dm, new_dm);
        test_pure_extended(old_dm, new_dm);
        test_basic_settings(old_dm, new_dm);
        test_system_paths_and_commandline(old_dm, new_dm);
        test_position_algorithms(old_dm, new_dm);
        test_picture_cache_and_find(old_dm, new_dm);
        test_screen_buffers(old_dm, new_dm);
        test_encoded_capture(old_dm, new_dm);
        test_ocr_state(old_dm, new_dm);
        test_dictionary_core(old_dm, new_dm);
        test_ocr_core(old_dm, new_dm);
        test_critical_and_password(legacy_path, old_dm, new_dm);
        test_word_result_and_input(old_dm, new_dm);
        test_system(old_dm, new_dm);
        test_cpu_cursor_display_state(old_dm, new_dm);
        test_audio_aero(old_dm, new_dm);
        test_system_identity(old_dm, new_dm);
        test_mac_address(old_dm, new_dm);
        test_network_time(old_dm, new_dm);
        test_env(old_dm, new_dm);
        test_file_ini(old_dm, new_dm);
        test_memory(old_dm, new_dm);
        test_remote_api_address(old_dm, new_dm);
        test_memory_search(old_dm, new_dm);
        test_window(old_dm, new_dm);
        test_color_core(old_dm, new_dm);

        FreeDm();

        if (old_dm.DumpRuntimeVtable("legacy_runtime_vtable_after.csv"))
            std::puts("Wrote legacy_runtime_vtable_after.csv");
        else
            std::puts("Failed to dump legacy runtime vtable after tests.");

        std::printf("\nSUMMARY passes=%d failures=%d\n", g_passes, g_failures);
        return g_failures == 0 ? 0 : 1;
    } catch (const std::exception &e) {
        std::printf("PARITY PROBE ERROR: %s\n", e.what());
        return 201;
    }
#endif
}
