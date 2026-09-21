#include "legacy_dm_x64.h"
#include "op_dispatch.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <wininet.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <winioctl.h>
#include <mmsystem.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <fstream>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <optional>
#include <limits>
#include <random>
#include <sstream>
#include <thread>
#include <string>
#include <vector>

using hcbyj64::A;

namespace {

struct ScreenImageCompat;

struct ExcludeRegionCompat {
    long x1 = 0;
    long y1 = 0;
    long x2 = 0;
    long y2 = 0;
};


struct LegacyDictEntryCompat {
    std::string raw;
    std::string bitmap;
    std::string word;
    long left = 0;
    long right = 0;
    long declared_count = 0;
    long height = 0;
};

bool ParseLegacyDictEntryCompat(PCSTR text, LegacyDictEntryCompat &out) {
    out = {};
    if (!text || !*text) return false;
    const std::string source(text);
    const auto parts = SplitCompat(source, '$');
    if (parts.size() != 4 ||
        parts[0].empty() || parts[1].empty() ||
        parts[2].empty() || parts[3].empty())
        return false;

    for (char ch : parts[0]) {
        if (!std::isxdigit(static_cast<unsigned char>(ch)))
            return false;
    }

    long left = 0, right = 0, count = 0;
    {
        const auto metrics = SplitCompat(parts[2], '.');
        if (metrics.size() != 3 ||
            !ParseLongCompat(metrics[0], left) ||
            !ParseLongCompat(metrics[1], right) ||
            !ParseLongCompat(metrics[2], count))
            return false;
    }

    long height = 0;
    if (!ParseLongCompat(parts[3], height) ||
        height <= 0 || height > 255)
        return false;

    std::string bitmap = parts[0];
    std::transform(
        bitmap.begin(), bitmap.end(), bitmap.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });

    out.raw = source;
    out.bitmap = std::move(bitmap);
    out.word = parts[1];
    out.left = left;
    out.right = right;
    out.declared_count = count;
    out.height = height;
    return true;
}

std::string LegacyDictKeyCompat(const LegacyDictEntryCompat &entry) {
    return entry.bitmap + "$" + std::to_string(entry.height);
}

bool LoadLegacyDictTextCompat(
    const char *data, size_t size,
    std::vector<LegacyDictEntryCompat> &out) {
    out.clear();
    if (!data || size == 0) return false;

    std::string text(data, size);
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);

    std::unordered_map<std::string, size_t> seen;
    size_t begin = 0;
    while (begin <= text.size()) {
        size_t end = text.find_first_of("\r\n", begin);
        if (end == std::string::npos) end = text.size();

        std::string line = text.substr(begin, end - begin);
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        if (!line.empty()) {
            LegacyDictEntryCompat entry;
            if (ParseLegacyDictEntryCompat(line.c_str(), entry)) {
                const std::string key = LegacyDictKeyCompat(entry);
                const auto it = seen.find(key);
                if (it == seen.end()) {
                    seen.emplace(key, out.size());
                    out.push_back(std::move(entry));
                } else {
                    out[it->second] = std::move(entry);
                }
            }
        }

        if (end == text.size()) break;
        begin = end + 1;
        if (begin < text.size() &&
            text[end] == '\r' && text[begin] == '\n')
            ++begin;
    }
    return !out.empty();
}

bool ReadLegacyDictFileCompat(
    const std::filesystem::path &path,
    std::vector<LegacyDictEntryCompat> &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.clear();
        return false;
    }
    const std::string bytes(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
    return LoadLegacyDictTextCompat(bytes.data(), bytes.size(), out);
}

bool SaveLegacyDictFileCompat(
    const std::filesystem::path &path,
    const std::vector<LegacyDictEntryCompat> &entries) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    for (size_t i = 0; i < entries.size(); ++i) {
        out.write(
            entries[i].raw.data(),
            static_cast<std::streamsize>(entries[i].raw.size()));
        if (i + 1 < entries.size())
            out.write("\r\n", 2);
    }
    return out.good();
}

long UpsertLegacyDictEntryCompat(
    std::vector<LegacyDictEntryCompat> &entries,
    PCSTR dict_info) {
    LegacyDictEntryCompat entry;
    if (!ParseLegacyDictEntryCompat(dict_info, entry))
        return 0;

    const std::string key = LegacyDictKeyCompat(entry);
    for (auto &existing : entries) {
        if (LegacyDictKeyCompat(existing) == key) {
            existing = std::move(entry);
            return 1;
        }
    }
    entries.push_back(std::move(entry));
    return 1;
}

struct DmImpl {
    hcbyj64::OpObject op;
    bool hwnd_is_pid = false;
    HWND bound_hwnd = nullptr;
    std::string bind_display = "normal";
    std::string bind_mouse = "normal";
    std::string bind_keypad = "normal";
    std::string bind_public;
    long bind_mode = 0;
    long bind_enable = 1;
    long virtual_mouse_x = 0;
    long virtual_mouse_y = 0;
    long native_error = 0;
    std::string scratch;
    std::map<std::pair<long, std::string>, std::string> env;
    std::mutex state_mutex;
    ULONGLONG prev_idle = 0;
    ULONGLONG prev_kernel = 0;
    ULONGLONG prev_user = 0;
    bool cpu_sample_valid = false;
    long id = 0;
    long enum_window_delay = 10000;
    bool show_error_msg = true;
    std::string global_path;
    long keypad_delay_normal = 30;
    long keypad_delay_windows = 10;
    long keypad_delay_dx = 50;
    long mouse_delay_normal = 30;
    long mouse_delay_windows = 10;
    long mouse_delay_dx = 40;
    bool get_color_by_capture = true;
    bool speed_normal_graphic = false;
    bool display_locked = false;
    std::shared_ptr<ScreenImageCompat> locked_display;
    long next_play_id = 1;
    std::map<long, std::string> play_aliases;
    bool pic_cache_enabled = true;
    std::map<std::string, std::shared_ptr<ScreenImageCompat>> pic_cache;
    std::map<std::string, std::shared_ptr<ScreenImageCompat>> memory_pic_cache;
    bool display_debug_enabled = false;
    long display_delay = 3000;
    long display_refresh_delay = 400;
    bool show_asm_error_msg = true;
    bool find_pic_multithread_enabled = true;
    long find_pic_multithread_count = 4;
    long find_pic_multithread_limit = 0;
    std::vector<ExcludeRegionCompat> exclude_regions;
    unsigned int exclude_region_rgb = 0xFF00FF;

    // OCR / dictionary state. Keep the legacy defaults so x86 parity can
    // validate behavior before the recognition engine itself is migrated.
    long current_dict = 0;
    std::array<std::vector<LegacyDictEntryCompat>, 100> dictionaries;
    std::array<std::string, 100> dictionary_sources;
    bool share_dict_enabled = false;
    bool exact_ocr_enabled = false;
    long min_row_gap = 0;
    long min_col_gap = 0;
    long word_gap = 5;
    long word_line_height = 10;
    long nodict_row_gap = 1;
    long nodict_col_gap = 1;
    long nodict_word_gap = 5;
    long nodict_word_line_height = 10;
    std::string pic_password;
    std::string dict_password;
    bool param64_to_pointer = false;
    std::string memory_find_result_file;
    void *legacy_bin_buffer = nullptr;
    SIZE_T legacy_bin_size = 0;
};


std::atomic<long> g_next_dm_id{1};
std::atomic<long> g_dm_object_count{0};
std::mutex g_cri_mutex;
DmImpl *g_cri_owner = nullptr;
std::mutex g_bind_mutex;
std::unordered_map<ULONG_PTR, long> g_bound_window_counts;

void RegisterBoundWindowCompat(HWND hwnd) {
    if (!hwnd) return;
    std::lock_guard<std::mutex> lock(g_bind_mutex);
    ++g_bound_window_counts[reinterpret_cast<ULONG_PTR>(hwnd)];
}

void UnregisterBoundWindowCompat(HWND hwnd) {
    if (!hwnd) return;
    std::lock_guard<std::mutex> lock(g_bind_mutex);
    const ULONG_PTR key = reinterpret_cast<ULONG_PTR>(hwnd);
    const auto it = g_bound_window_counts.find(key);
    if (it == g_bound_window_counts.end()) return;
    if (--it->second <= 0) g_bound_window_counts.erase(it);
}

bool IsWindowBoundCompat(HWND hwnd) {
    if (!hwnd) return false;
    std::lock_guard<std::mutex> lock(g_bind_mutex);
    const auto it =
        g_bound_window_counts.find(reinterpret_cast<ULONG_PTR>(hwnd));
    return it != g_bound_window_counts.end() && it->second > 0;
}

void ClearObjectBindingCompat(DmImpl *p) {
    if (!p) return;
    HWND old = nullptr;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        old = p->bound_hwnd;
        p->bound_hwnd = nullptr;
        p->bind_display = "normal";
        p->bind_mouse = "normal";
        p->bind_keypad = "normal";
        p->bind_public.clear();
        p->bind_mode = 0;
        p->bind_enable = 1;
        p->virtual_mouse_x = 0;
        p->virtual_mouse_y = 0;
        p->display_locked = false;
        p->locked_display.reset();
    }
    UnregisterBoundWindowCompat(old);
}

std::string ModuleDirectoryCompat(bool current_dll) {
    HMODULE module = nullptr;
    if (current_dll) {
        ::GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&ModuleDirectoryCompat),
            &module);
    } else {
        module = ::GetModuleHandleA(nullptr);
    }
    if (!module) return {};

    std::vector<char> buf(32768, 0);
    const DWORD n = ::GetModuleFileNameA(module, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) return {};
    std::filesystem::path p(std::string(buf.data(), n));
    return p.parent_path().string();
}

std::string NormalizeGlobalPathCompat(PCSTR input) {
    if (!input || !*input) return {};
    std::filesystem::path p(input);
    if (p.is_relative())
        p = std::filesystem::path(ModuleDirectoryCompat(false)) / p;
    std::error_code ec;
    p = std::filesystem::absolute(p, ec);
    if (ec) return {};
    p = p.lexically_normal();
    return p.string();
}

DmImpl *P(void *p) { return static_cast<DmImpl *>(p); }
const DmImpl *P(const void *p) { return static_cast<const DmImpl *>(p); }
void SetNativeError(DmImpl *p, long e) { if (p) p->native_error = e; }

DWORD ResolvePid(DmImpl *p, long hwnd_or_pid) {
    if (!p) return 0;
    if (p->hwnd_is_pid) return static_cast<DWORD>(hwnd_or_pid);
    DWORD pid = 0;
    HWND hwnd = reinterpret_cast<HWND>(static_cast<INT_PTR>(hwnd_or_pid));
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) SetNativeError(p, static_cast<long>(::GetLastError()));
    return pid;
}

HANDLE OpenTarget(DmImpl *p, long hwnd_or_pid, DWORD access) {
    DWORD pid = ResolvePid(p, hwnd_or_pid);
    if (!pid) return nullptr;
    HANDLE h = ::OpenProcess(access, FALSE, pid);
    if (!h) SetNativeError(p, static_cast<long>(::GetLastError()));
    return h;
}

template <class T>
bool ReadValue(DmImpl *p, long hwnd_or_pid, LONGLONG address, T &out) {
    HANDLE h = OpenTarget(p, hwnd_or_pid, PROCESS_VM_READ | PROCESS_QUERY_INFORMATION);
    if (!h) return false;
    SIZE_T got = 0;
    BOOL ok = ::ReadProcessMemory(h, reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(address)), &out, sizeof(out), &got);
    if (!ok || got != sizeof(out)) {
        SetNativeError(p, static_cast<long>(::GetLastError()));
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    SetNativeError(p, 0);
    return true;
}

template <class T>
bool WriteValue(DmImpl *p, long hwnd_or_pid, LONGLONG address, const T &value) {
    HANDLE h = OpenTarget(p, hwnd_or_pid, PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION);
    if (!h) return false;
    SIZE_T put = 0;
    BOOL ok = ::WriteProcessMemory(h, reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(address)), &value, sizeof(value), &put);
    if (!ok || put != sizeof(value)) {
        SetNativeError(p, static_cast<long>(::GetLastError()));
        CloseHandle(h);
        return false;
    }
    CloseHandle(h);
    SetNativeError(p, 0);
    return true;
}

bool ParseHexBytes(PCSTR text, std::vector<unsigned char> &out) {
    out.clear();
    if (!text) return false;
    const char *s = text;
    while (*s) {
        while (*s && std::isspace(static_cast<unsigned char>(*s))) ++s;
        if (!*s) break;
        char *end = nullptr;
        unsigned long v = std::strtoul(s, &end, 16);
        if (end == s || v > 0xff) return false;
        out.push_back(static_cast<unsigned char>(v));
        s = end;
        if (*s && !std::isspace(static_cast<unsigned char>(*s))) return false;
    }
    return !out.empty();
}

DWORD ProtectForType(long type) {
    switch (type) {
    case 0: return PAGE_EXECUTE_READWRITE;
    case 1: return PAGE_EXECUTE_READ;
    case 2: return PAGE_READWRITE;
    default: return PAGE_EXECUTE_READWRITE;
    }
}

ULONGLONG FileTime64(const FILETIME &ft) {
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

HWND HwndFromLong(long v) { return reinterpret_cast<HWND>(static_cast<INT_PTR>(v)); }

std::string HexBytesCompat(const void *ptr, size_t size) {
    const auto *p = static_cast<const unsigned char *>(ptr);
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    if (size) out.reserve(size * 3 - 1);
    for (size_t i = 0; i < size; ++i) {
        if (i) out.push_back(' ');
        out.push_back(kHex[(p[i] >> 4) & 0x0f]);
        out.push_back(kHex[p[i] & 0x0f]);
    }
    return out;
}

std::vector<std::string> SplitPipeCompat(PCSTR text) {
    std::vector<std::string> out;
    if (!text || !*text) return out;
    const char *begin = text;
    const char *p = text;
    for (;; ++p) {
        if (*p == '|' || *p == '\\0') {
            out.emplace_back(begin, p);
            if (*p == '\\0') break;
            begin = p + 1;
        }
    }
    return out;
}

bool ParseResultXYCompat(const std::string &item, long *x, long *y) {
    if (!x || !y) return false;
    const char *p = item.c_str();
    char *end = nullptr;
    long vx = std::strtol(p, &end, 10);
    if (end == p || *end != ',') return false;
    p = end + 1;
    long vy = std::strtol(p, &end, 10);
    if (end == p) return false;
    *x = vx;
    *y = vy;
    return true;
}

std::string JoinMultiSz(const char *buf, size_t cap) {
    std::string out;
    if (!buf || cap == 0) return out;
    size_t i = 0;
    while (i < cap && buf[i]) {
        size_t n = strnlen(buf + i, cap - i);
        if (n == 0 || i + n >= cap) break;
        if (!out.empty()) out.push_back('|');
        out.append(buf + i, n);
        i += n + 1;
    }
    return out;
}


bool ProcessIs64BitCompat(DWORD pid) {
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    using IsWow64Process2Fn = BOOL (WINAPI *)(HANDLE, USHORT *, USHORT *);
    auto kernel = ::GetModuleHandleW(L"kernel32.dll");
    auto is_wow64_2 = kernel
        ? reinterpret_cast<IsWow64Process2Fn>(::GetProcAddress(kernel, "IsWow64Process2"))
        : nullptr;

    bool result = false;
    if (is_wow64_2) {
        USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
        if (is_wow64_2(process, &process_machine, &native_machine)) {
            result = process_machine == IMAGE_FILE_MACHINE_UNKNOWN &&
                     (native_machine == IMAGE_FILE_MACHINE_AMD64 ||
                      native_machine == IMAGE_FILE_MACHINE_ARM64 ||
                      native_machine == IMAGE_FILE_MACHINE_IA64);
        }
    } else {
        BOOL target_wow64 = FALSE;
        BOOL self_wow64 = FALSE;
        if (::IsWow64Process(process, &target_wow64) &&
            ::IsWow64Process(::GetCurrentProcess(), &self_wow64)) {
#if defined(_WIN64)
            result = !target_wow64;
#else
            result = self_wow64 && !target_wow64;
#endif
        }
    }

    ::CloseHandle(process);
    return result;
}

bool ContainsNoCaseCompat(const std::string &haystack, const std::string &needle) {
    if (needle.empty()) return true;
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string h(haystack), n(needle);
    std::transform(h.begin(), h.end(), h.begin(), lower);
    std::transform(n.begin(), n.end(), n.begin(), lower);
    return h.find(n) != std::string::npos;
}

std::string WindowTextCompat(HWND hwnd) {
    const int n = ::GetWindowTextLengthA(hwnd);
    if (n <= 0) return {};
    std::vector<char> buf(static_cast<size_t>(n) + 1, 0);
    ::GetWindowTextA(hwnd, buf.data(), static_cast<int>(buf.size()));
    return buf.data();
}

std::string WindowClassCompat(HWND hwnd) {
    char buf[512]{};
    if (::GetClassNameA(hwnd, buf, static_cast<int>(sizeof(buf))) <= 0) return {};
    return buf;
}

bool WindowMatchesCompat(HWND hwnd, PCSTR title, PCSTR class_name, long filter) {
    if ((filter & 16) && !::IsWindowVisible(hwnd)) return false;

    if ((filter & 1) && title && *title) {
        if (!ContainsNoCaseCompat(WindowTextCompat(hwnd), title)) return false;
    }
    if ((filter & 2) && class_name && *class_name) {
        if (!ContainsNoCaseCompat(WindowClassCompat(hwnd), class_name)) return false;
    }
    return true;
}

std::string JoinHwndsCompat(const std::vector<HWND> &windows) {
    std::ostringstream oss;
    for (size_t i = 0; i < windows.size(); ++i) {
        if (i) oss << ',';
        oss << static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(windows[i]));
    }
    return oss.str();
}

struct EnumWindowCompatContext {
    DWORD pid = 0;
    PCSTR title = nullptr;
    PCSTR class_name = nullptr;
    long filter = 0;
    std::vector<HWND> *out = nullptr;
};

BOOL CALLBACK EnumWindowCompatProc(HWND hwnd, LPARAM lp) {
    auto *ctx = reinterpret_cast<EnumWindowCompatContext *>(lp);
    if (!ctx || !ctx->out) return FALSE;

    if (ctx->pid != 0) {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid) return TRUE;
    }

    if (WindowMatchesCompat(hwnd, ctx->title, ctx->class_name, ctx->filter))
        ctx->out->push_back(hwnd);
    return TRUE;
}

void EnumDirectChildrenCompat(HWND parent, EnumWindowCompatContext &ctx) {
    for (HWND child = ::FindWindowExA(parent, nullptr, nullptr, nullptr);
         child != nullptr;
         child = ::FindWindowExA(parent, child, nullptr, nullptr)) {
        if (ctx.pid != 0) {
            DWORD pid = 0;
            ::GetWindowThreadProcessId(child, &pid);
            if (pid != ctx.pid) continue;
        }
        if (WindowMatchesCompat(child, ctx.title, ctx.class_name, ctx.filter))
            ctx.out->push_back(child);
    }
}

std::vector<HWND> EnumWindowsCompat(
    HWND parent, DWORD pid, PCSTR title, PCSTR class_name, long filter) {
    std::vector<HWND> out;
    EnumWindowCompatContext ctx{pid, title, class_name, filter, &out};

    if (parent) {
        if (filter & 4) EnumDirectChildrenCompat(parent, ctx);
        else ::EnumChildWindows(parent, EnumWindowCompatProc, reinterpret_cast<LPARAM>(&ctx));
    } else if (filter & 8) {
        ::EnumWindows(EnumWindowCompatProc, reinterpret_cast<LPARAM>(&ctx));
    } else {
        ::EnumChildWindows(::GetDesktopWindow(), EnumWindowCompatProc, reinterpret_cast<LPARAM>(&ctx));
    }

    if (filter & 32) {
        std::stable_sort(out.begin(), out.end(), [](HWND a, HWND b) {
            return reinterpret_cast<ULONG_PTR>(a) < reinterpret_cast<ULONG_PTR>(b);
        });
    }
    return out;
}

ULONGLONG ProcessCreateTimeCompat(DWORD pid) {
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::numeric_limits<ULONGLONG>::max();
    FILETIME create{}, exit{}, kernel{}, user{};
    ULONGLONG value = std::numeric_limits<ULONGLONG>::max();
    if (::GetProcessTimes(process, &create, &exit, &kernel, &user))
        value = FileTime64(create);
    ::CloseHandle(process);
    return value;
}

std::vector<DWORD> EnumProcessIdsCompat(PCSTR process_name) {
    std::vector<std::pair<ULONGLONG, DWORD>> matches;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};

    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32First(snap, &pe)) {
        do {
            const bool name_ok =
                !process_name || !*process_name || _stricmp(pe.szExeFile, process_name) == 0;
            if (name_ok)
                matches.emplace_back(ProcessCreateTimeCompat(pe.th32ProcessID), pe.th32ProcessID);
        } while (::Process32Next(snap, &pe));
    }
    ::CloseHandle(snap);

    std::stable_sort(matches.begin(), matches.end(),
        [](const auto &a, const auto &b) {
            if (a.first != b.first) return a.first < b.first;
            return a.second < b.second;
        });

    std::vector<DWORD> out;
    out.reserve(matches.size());
    for (const auto &v : matches) out.push_back(v.second);
    return out;
}

std::string JoinPidsCompat(const std::vector<DWORD> &pids) {
    std::ostringstream oss;
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i) oss << ',';
        oss << pids[i];
    }
    return oss.str();
}

long MouseSpeedLevelFromWindowsCompat(long speed) {
    static const int map[11] = {1,2,4,6,8,10,12,14,16,18,20};
    long best = 1;
    long best_delta = LONG_MAX;
    for (long i = 0; i < 11; ++i) {
        const long delta = std::labs(speed - map[i]);
        if (delta < best_delta) {
            best_delta = delta;
            best = i + 1;
        }
    }
    return best;
}

long WindowsMouseSpeedFromLevelCompat(long level) {
    static const int map[11] = {1,2,4,6,8,10,12,14,16,18,20};
    if (level < 1 || level > 11) return 0;
    return map[level - 1];
}


LONGLONG ModuleBaseForPidCompat(DWORD pid, const std::string &module_name) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    MODULEENTRY32 me{};
    me.dwSize = sizeof(me);
    LONGLONG result = 0;
    if (::Module32First(snap, &me)) {
        do {
            if (_stricmp(me.szModule, module_name.c_str()) == 0 ||
                _stricmp(me.szExePath, module_name.c_str()) == 0) {
                result = static_cast<LONGLONG>(reinterpret_cast<ULONG_PTR>(me.modBaseAddr));
                break;
            }
        } while (::Module32Next(snap, &me));
    }
    ::CloseHandle(snap);
    return result;
}

class AddressExprParserCompat {
public:
    AddressExprParserCompat(HANDLE process, DWORD pid, bool target64, PCSTR text)
        : process_(process), pid_(pid), target64_(target64), text_(text ? text : "") {}

    bool Parse(ULONGLONG &value) {
        pos_ = 0;
        SkipSpace();
        if (!ParseExpr(value)) return false;
        SkipSpace();
        return pos_ == text_.size();
    }

private:
    void SkipSpace() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }

    bool ParseExpr(ULONGLONG &value) {
        if (!ParsePrimary(value)) return false;
        for (;;) {
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] == ']') return true;
            const char op = text_[pos_];
            if (op != '+' && op != '-') return false;
            ++pos_;
            ULONGLONG rhs = 0;
            if (!ParsePrimary(rhs)) return false;
            if (op == '+') value += rhs;
            else value -= rhs;
        }
    }

    bool ParsePrimary(ULONGLONG &value) {
        SkipSpace();
        if (pos_ >= text_.size()) return false;

        if (text_[pos_] == '[') {
            ++pos_;
            ULONGLONG address = 0;
            if (!ParseExpr(address)) return false;
            SkipSpace();
            if (pos_ >= text_.size() || text_[pos_] != ']') return false;
            ++pos_;

            SIZE_T got = 0;
            if (target64_) {
                ULONGLONG ptr = 0;
                if (!::ReadProcessMemory(
                        process_,
                        reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(address)),
                        &ptr, sizeof(ptr), &got) ||
                    got != sizeof(ptr)) return false;
                value = ptr;
            } else {
                DWORD ptr = 0;
                if (!::ReadProcessMemory(
                        process_,
                        reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(address)),
                        &ptr, sizeof(ptr), &got) ||
                    got != sizeof(ptr)) return false;
                value = ptr;
            }
            return true;
        }

        if (text_[pos_] == '<') {
            const size_t begin = ++pos_;
            while (pos_ < text_.size() && text_[pos_] != '>') ++pos_;
            if (pos_ >= text_.size() || pos_ == begin) return false;
            const std::string module = text_.substr(begin, pos_ - begin);
            ++pos_;
            const LONGLONG base = ModuleBaseForPidCompat(pid_, module);
            if (!base) return false;
            value = static_cast<ULONGLONG>(base);
            return true;
        }

        size_t begin = pos_;
        if (pos_ + 2 <= text_.size() && text_[pos_] == '0' &&
            (text_[pos_ + 1] == 'x' || text_[pos_ + 1] == 'X')) {
            pos_ += 2;
            begin = pos_;
        }
        while (pos_ < text_.size() &&
               std::isxdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
        if (pos_ == begin) return false;

        const std::string token = text_.substr(begin, pos_ - begin);
        char *end = nullptr;
        const unsigned long long parsed = std::strtoull(token.c_str(), &end, 16);
        if (!end || *end != '\0') return false;
        value = parsed;
        return true;
    }

    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    bool target64_ = false;
    std::string text_;
    size_t pos_ = 0;
};

bool ResolveAddressExprCompat(DmImpl *p, long hwnd_or_pid, PCSTR expr, LONGLONG &out) {
    if (!p || !expr || !*expr) {
        SetNativeError(p, ERROR_INVALID_PARAMETER);
        return false;
    }
    const DWORD pid = ResolvePid(p, hwnd_or_pid);
    if (!pid) return false;

    HANDLE process = ::OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!process) {
        SetNativeError(p, static_cast<long>(::GetLastError()));
        return false;
    }

    ULONGLONG value = 0;
    AddressExprParserCompat parser(process, pid, ProcessIs64BitCompat(pid), expr);
    const bool ok = parser.Parse(value);
    if (!ok) SetNativeError(p, ERROR_INVALID_DATA);
    else SetNativeError(p, 0);
    ::CloseHandle(process);

    if (!ok) return false;
    out = static_cast<LONGLONG>(value);
    return true;
}


struct MemoryBytePatternCompat {
    std::vector<unsigned char> bytes;
    std::vector<unsigned char> mask;

    bool empty() const { return bytes.empty(); }
    size_t size() const { return bytes.size(); }

    bool Match(const unsigned char *p) const {
        for (size_t i = 0; i < bytes.size(); ++i) {
            if (mask[i] && p[i] != bytes[i]) return false;
        }
        return true;
    }
};

bool ParseMemoryPatternCompat(PCSTR text, MemoryBytePatternCompat &out) {
    out = {};
    if (!text) return false;
    std::istringstream iss(text);
    std::string token;
    while (iss >> token) {
        if (token == "??") {
            out.bytes.push_back(0);
            out.mask.push_back(0);
            continue;
        }
        if (token.size() != 2 ||
            !std::isxdigit(static_cast<unsigned char>(token[0])) ||
            !std::isxdigit(static_cast<unsigned char>(token[1]))) {
            return false;
        }
        char *end = nullptr;
        const unsigned long v = std::strtoul(token.c_str(), &end, 16);
        if (!end || *end != '\0' || v > 0xff) return false;
        out.bytes.push_back(static_cast<unsigned char>(v));
        out.mask.push_back(1);
    }
    return !out.empty();
}

bool ParseHexU64Compat(const std::string &text, ULONGLONG &value) {
    if (text.empty()) return false;
    char *end = nullptr;
    value = std::strtoull(text.c_str(), &end, 16);
    return end && *end == '\0';
}

bool ParseMemoryRangeCompat(PCSTR text, ULONGLONG &begin, ULONGLONG &end) {
    if (!text) return false;
    std::string v(text);
    v.erase(std::remove_if(v.begin(), v.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), v.end());
    const size_t dash = v.find('-');
    if (dash == std::string::npos || v.find('-', dash + 1) != std::string::npos)
        return false;
    if (!ParseHexU64Compat(v.substr(0, dash), begin) ||
        !ParseHexU64Compat(v.substr(dash + 1), end))
        return false;
    return begin <= end;
}

std::vector<ULONGLONG> ParseAddressListCompat(const std::string &text) {
    std::vector<ULONGLONG> out;
    size_t begin = 0;
    while (begin <= text.size()) {
        size_t sep = text.find('|', begin);
        std::string token = text.substr(
            begin, sep == std::string::npos ? std::string::npos : sep - begin);
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        }), token.end());
        ULONGLONG value = 0;
        if (!token.empty() && ParseHexU64Compat(token, value))
            out.push_back(value);
        if (sep == std::string::npos) break;
        begin = sep + 1;
    }
    return out;
}

bool IsReadableProtectCompat(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD p = protect & 0xff;
    return p == PAGE_READONLY ||
           p == PAGE_READWRITE ||
           p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}

bool IsWritableProtectCompat(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD p = protect & 0xff;
    return p == PAGE_READWRITE ||
           p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}

bool MemoryRegionAllowedCompat(const MEMORY_BASIC_INFORMATION &mbi, long mode) {
    if (mbi.State != MEM_COMMIT || !IsReadableProtectCompat(mbi.Protect))
        return false;
    const bool include_mapped = (mode & 16) != 0;
    const bool writable_only = (mode & 1) != 0;
    if (!include_mapped && mbi.Type == MEM_MAPPED) return false;
    if (writable_only && !IsWritableProtectCompat(mbi.Protect)) return false;
    return true;
}

std::string FormatMemoryAddressCompat(ULONGLONG address) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%llX",
                  static_cast<unsigned long long>(address));
    return buf;
}

std::string JoinMemoryAddressesCompat(const std::vector<ULONGLONG> &addresses) {
    std::string out;
    for (size_t i = 0; i < addresses.size(); ++i) {
        if (i) out.push_back('|');
        out += FormatMemoryAddressCompat(addresses[i]);
    }
    return out;
}

std::string ResolveMemoryResultFileCompat(DmImpl *p, PCSTR file) {
    if (!p || !file || !*file) return {};
    std::filesystem::path path(file);
    if (path.is_relative()) {
        std::filesystem::path base(
            p->global_path.empty() ? ModuleDirectoryCompat(false) : p->global_path);
        path = base / path;
    }
    return path.lexically_normal().string();
}

std::string ReadWholeFileCompat(const std::string &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return std::string(
        std::istreambuf_iterator<char>(in),
        std::istreambuf_iterator<char>());
}

bool WriteWholeFileCompat(const std::string &file, const std::string &data) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

template <class Matcher>
std::vector<ULONGLONG> ScanMemoryCompat(
    DmImpl *p,
    long hwnd_or_pid,
    PCSTR addr_range,
    size_t value_size,
    long step,
    long mode,
    Matcher matcher) {

    std::vector<ULONGLONG> results;
    if (!p || !addr_range || value_size == 0 || step <= 0) {
        SetNativeError(p, ERROR_INVALID_PARAMETER);
        return results;
    }

    const DWORD pid = ResolvePid(p, hwnd_or_pid);
    if (!pid) return results;

    HANDLE process = ::OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        SetNativeError(p, static_cast<long>(::GetLastError()));
        return results;
    }

    ULONGLONG range_begin = 0, range_end = 0;
    const bool is_range = ParseMemoryRangeCompat(
        addr_range, range_begin, range_end);

    std::string list_text;
    if (!is_range && !p->memory_find_result_file.empty())
        list_text = ReadWholeFileCompat(p->memory_find_result_file);
    else if (!is_range)
        list_text = addr_range;

    if (!is_range) {
        const auto addresses = ParseAddressListCompat(list_text);
        std::vector<unsigned char> value(value_size);
        for (ULONGLONG address : addresses) {
            SIZE_T got = 0;
            if (::ReadProcessMemory(
                    process,
                    reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(address)),
                    value.data(), value.size(), &got) &&
                got == value.size() &&
                matcher(value.data())) {
                results.push_back(address);
            }
        }
        ::CloseHandle(process);
        SetNativeError(p, 0);
        return results;
    }

    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const ULONGLONG max_address =
        static_cast<ULONGLONG>(
            reinterpret_cast<ULONG_PTR>(si.lpMaximumApplicationAddress));
    range_end = (std::min)(range_end, max_address);
    if (range_begin > range_end) {
        ::CloseHandle(process);
        SetNativeError(p, 0);
        return results;
    }

    constexpr SIZE_T kChunk = 1024 * 1024;
    ULONGLONG cursor = range_begin;

    while (cursor <= range_end) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T q = ::VirtualQueryEx(
            process,
            reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(cursor)),
            &mbi, sizeof(mbi));
        if (!q) {
            const ULONGLONG next =
                cursor + static_cast<ULONGLONG>(si.dwPageSize);
            if (next <= cursor) break;
            cursor = next;
            continue;
        }

        const ULONGLONG region_begin =
            static_cast<ULONGLONG>(
                reinterpret_cast<ULONG_PTR>(mbi.BaseAddress));
        const ULONGLONG region_size =
            static_cast<ULONGLONG>(mbi.RegionSize);
        const ULONGLONG region_end =
            region_size && region_begin <= ULLONG_MAX - region_size
                ? region_begin + region_size - 1
                : ULLONG_MAX;

        const ULONGLONG scan_begin = (std::max)(range_begin, region_begin);
        const ULONGLONG scan_end = (std::min)(range_end, region_end);

        if (scan_begin <= scan_end && MemoryRegionAllowedCompat(mbi, mode)) {
            ULONGLONG chunk_begin = scan_begin;
            while (chunk_begin <= scan_end) {
                const ULONGLONG remaining = scan_end - chunk_begin + 1;
                const SIZE_T request = static_cast<SIZE_T>(
                    (std::min<ULONGLONG>)(
                        remaining,
                        static_cast<ULONGLONG>(kChunk + value_size - 1)));

                std::vector<unsigned char> buffer(request);
                SIZE_T got = 0;
                if (::ReadProcessMemory(
                        process,
                        reinterpret_cast<LPCVOID>(
                            static_cast<ULONG_PTR>(chunk_begin)),
                        buffer.data(), buffer.size(), &got) &&
                    got >= value_size) {

                    ULONGLONG candidate = chunk_begin;
                    const ULONGLONG rem =
                        (candidate - range_begin) %
                        static_cast<ULONGLONG>(step);
                    if (rem)
                        candidate += static_cast<ULONGLONG>(step) - rem;

                    const ULONGLONG got_end =
                        chunk_begin + static_cast<ULONGLONG>(got) - 1;

                    while (candidate <= got_end &&
                           candidate <= scan_end &&
                           value_size - 1 <= got_end - candidate) {
                        const size_t offset =
                            static_cast<size_t>(candidate - chunk_begin);
                        if (matcher(buffer.data() + offset))
                            results.push_back(candidate);
                        if (candidate > ULLONG_MAX -
                                static_cast<ULONGLONG>(step))
                            break;
                        candidate += static_cast<ULONGLONG>(step);
                    }
                }

                if (remaining <= kChunk) break;
                if (chunk_begin > ULLONG_MAX - kChunk) break;
                chunk_begin += kChunk;
            }
        }

        if (region_end == ULLONG_MAX || region_end < cursor) break;
        cursor = region_end + 1;
    }

    ::CloseHandle(process);
    SetNativeError(p, 0);
    return results;
}

std::string FinalizeMemoryFindCompat(
    DmImpl *p, const std::vector<ULONGLONG> &addresses) {
    if (!p) return {};
    std::string result = JoinMemoryAddressesCompat(addresses);
    if (!p->memory_find_result_file.empty()) {
        if (!WriteWholeFileCompat(p->memory_find_result_file, result))
            SetNativeError(p, ERROR_WRITE_FAULT);
    }
    return result;
}

std::wstring AcpToWideCompat(PCSTR s) {
    if (!s) return {};
    const int n = ::MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (::MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n) <= 0) return {};
    return out;
}

std::string WideToAcpCompat(const wchar_t *s, int chars = -1) {
    if (!s) return {};
    const int n = ::WideCharToMultiByte(CP_ACP, 0, s, chars, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    if (::WideCharToMultiByte(CP_ACP, 0, s, chars, out.data(), n, nullptr, nullptr) <= 0) return {};
    if (chars == -1 && !out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

std::string Utf8ToAcpCompat(const std::string &utf8) {
    if (utf8.empty()) return {};
    const int wn = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wn <= 0) return {};
    std::wstring wide(static_cast<size_t>(wn), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
            wide.data(), wn) <= 0) return {};
    return WideToAcpCompat(wide.data(), static_cast<int>(wide.size()));
}

std::string AcpToUtf8Compat(PCSTR s) {
    const std::wstring wide = AcpToWideCompat(s);
    if (wide.empty()) return {};
    const int chars = wide.back() == L'\0' ? static_cast<int>(wide.size() - 1)
                                            : static_cast<int>(wide.size());
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), chars, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), chars, out.data(), n, nullptr, nullptr);
    return out;
}

bool ReadRemoteBytesCompat(
    DmImpl *p, long hwnd_or_pid, LONGLONG address, void *dst, SIZE_T size) {
    if (!p || (!dst && size)) return false;
    HANDLE process = OpenTarget(p, hwnd_or_pid, PROCESS_VM_READ | PROCESS_QUERY_INFORMATION);
    if (!process) return false;
    SIZE_T got = 0;
    const BOOL ok = ::ReadProcessMemory(
        process,
        reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(address)),
        dst, size, &got);
    if (!ok || got != size) SetNativeError(p, static_cast<long>(::GetLastError()));
    else SetNativeError(p, 0);
    ::CloseHandle(process);
    return ok && got == size;
}

bool WriteRemoteBytesCompat(
    DmImpl *p, long hwnd_or_pid, LONGLONG address, const void *src, SIZE_T size) {
    if (!p || (!src && size)) return false;
    HANDLE process = OpenTarget(
        p, hwnd_or_pid, PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION);
    if (!process) return false;
    SIZE_T put = 0;
    const BOOL ok = ::WriteProcessMemory(
        process,
        reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(address)),
        src, size, &put);
    if (!ok || put != size) SetNativeError(p, static_cast<long>(::GetLastError()));
    else SetNativeError(p, 0);
    ::CloseHandle(process);
    return ok && put == size;
}


struct PosEntryCompat {
    std::string raw;
    long x = 0;
    long y = 0;
    size_t order = 0;
};

bool ParseLongCompat(const std::string &s, long &out) {
    if (s.empty()) return false;
    char *end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    out = v;
    return true;
}

std::vector<std::string> SplitCompat(const std::string &s, char delim) {
    std::vector<std::string> out;
    size_t begin = 0;
    for (;;) {
        const size_t pos = s.find(delim, begin);
        if (pos == std::string::npos) {
            out.push_back(s.substr(begin));
            break;
        }
        out.push_back(s.substr(begin, pos - begin));
        begin = pos + 1;
    }
    return out;
}

bool ParsePosEntryCompat(const std::string &raw, long type, size_t order, PosEntryCompat &out) {
    out = {};
    out.raw = raw;
    out.order = order;

    if (type == 2) {
        const auto parts = SplitCompat(raw, '$');
        if (parts.size() < 3) return false;
        return ParseLongCompat(parts[1], out.x) && ParseLongCompat(parts[2], out.y);
    }

    const auto parts = SplitCompat(raw, ',');
    if (type == 1) {
        if (parts.size() < 2) return false;
        return ParseLongCompat(parts[0], out.x) && ParseLongCompat(parts[1], out.y);
    }
    if (type == 0 || type == 3) {
        if (parts.size() < 3) return false;
        return ParseLongCompat(parts[1], out.x) && ParseLongCompat(parts[2], out.y);
    }
    return false;
}

std::vector<PosEntryCompat> ParsePosListCompat(PCSTR all_pos, long type) {
    std::vector<PosEntryCompat> out;
    if (!all_pos || !*all_pos) return out;
    const auto items = SplitCompat(all_pos, '|');
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].empty()) continue;
        PosEntryCompat entry;
        if (ParsePosEntryCompat(items[i], type, i, entry))
            out.push_back(std::move(entry));
    }
    return out;
}

std::string JoinPosListCompat(const std::vector<PosEntryCompat> &items) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i) out.push_back('|');
        out += items[i].raw;
    }
    return out;
}


bool IsExtendedVkCompat(long vk) {
    switch (vk) {
    case VK_RMENU:
    case VK_RCONTROL:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
    case VK_SNAPSHOT:
    case VK_CANCEL:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
        return true;
    default:
        return false;
    }
}

long SendKeyboardVkCompat(long vk, bool key_up) {
    if (vk < 0 || vk > 0xFF) return 0;

    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(vk);
    input.ki.wScan = static_cast<WORD>(::MapVirtualKeyA(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
    input.ki.dwFlags = (key_up ? KEYEVENTF_KEYUP : 0) |
                       (IsExtendedVkCompat(vk) ? KEYEVENTF_EXTENDEDKEY : 0);
    input.ki.time = ::GetTickCount();
    input.ki.dwExtraInfo = ::GetMessageExtraInfo();
    return ::SendInput(1, &input, sizeof(input)) == 1 ? 1 : 0;
}

long SendMouseCompat(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD mouse_data = 0) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.mouseData = mouse_data;
    input.mi.dwFlags = flags;
    input.mi.time = ::GetTickCount();
    input.mi.dwExtraInfo = ::GetMessageExtraInfo();
    return ::SendInput(1, &input, sizeof(input)) == 1 ? 1 : 0;
}

long MoveMouseAbsoluteCompat(long x, long y) {
    const int left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 1 || height <= 1) return 0;

    const long long nx =
        ((static_cast<long long>(x) - left) * 65535LL) / (width - 1);
    const long long ny =
        ((static_cast<long long>(y) - top) * 65535LL) / (height - 1);

    return SendMouseCompat(
        MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
        static_cast<LONG>(nx), static_cast<LONG>(ny));
}


struct BindingSnapshotCompat {
    HWND hwnd = nullptr;
    std::string mouse = "normal";
    std::string keypad = "normal";
    long enable = 1;
    long mouse_x = 0;
    long mouse_y = 0;
};

BindingSnapshotCompat BindingSnapshotForObjectCompat(DmImpl *p) {
    BindingSnapshotCompat out{};
    if (!p) return out;
    std::lock_guard<std::mutex> lock(p->state_mutex);
    out.hwnd = p->bound_hwnd;
    out.mouse = p->bind_mouse;
    out.keypad = p->bind_keypad;
    out.enable = p->bind_enable;
    out.mouse_x = p->virtual_mouse_x;
    out.mouse_y = p->virtual_mouse_y;
    return out;
}

bool BoundInputEnabledCompat(const BindingSnapshotCompat &b) {
    return b.hwnd && ::IsWindow(b.hwnd) &&
           (b.enable == 1 || b.enable == -1);
}

HWND ResolveBoundMouseTargetCompat(
    const BindingSnapshotCompat &b, POINT &client_point) {
    if (!BoundInputEnabledCompat(b)) return nullptr;
    HWND target = b.hwnd;
    if (_stricmp(b.mouse.c_str(), "windows3") != 0)
        return target;

    POINT screen_point = client_point;
    if (!::ClientToScreen(b.hwnd, &screen_point))
        return target;

    HWND deepest = ::WindowFromPoint(screen_point);
    if (!deepest || (deepest != b.hwnd && !::IsChild(b.hwnd, deepest)))
        return target;

    POINT target_point = screen_point;
    if (!::ScreenToClient(deepest, &target_point))
        return target;
    client_point = target_point;
    return deepest;
}

LPARAM MakeMouseLParamCompat(long x, long y) {
    return MAKELPARAM(
        static_cast<short>(x),
        static_cast<short>(y));
}

long SendBoundMouseMessageCompat(
    DmImpl *p, UINT msg, WPARAM wparam = 0) {
    const auto b = BindingSnapshotForObjectCompat(p);
    if (!BoundInputEnabledCompat(b) ||
        (_stricmp(b.mouse.c_str(), "windows") != 0 &&
         _stricmp(b.mouse.c_str(), "windows3") != 0))
        return 0;

    POINT pt{
        static_cast<LONG>(b.mouse_x),
        static_cast<LONG>(b.mouse_y)
    };
    HWND target = ResolveBoundMouseTargetCompat(b, pt);
    if (!target) return 0;

    LPARAM lp = MakeMouseLParamCompat(pt.x, pt.y);
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
        POINT screen = pt;
        if (!::ClientToScreen(target, &screen)) return 0;
        lp = MakeMouseLParamCompat(screen.x, screen.y);
    }

    return ::PostMessageA(target, msg, wparam, lp) ? 1 : 0;
}

long MoveMouseForObjectCompat(DmImpl *p, long x, long y) {
    if (!p) return 0;
    const auto b = BindingSnapshotForObjectCompat(p);
    if (b.hwnd && ::IsWindow(b.hwnd)) {
        {
            std::lock_guard<std::mutex> lock(p->state_mutex);
            p->virtual_mouse_x = x;
            p->virtual_mouse_y = y;
        }

        if (BoundInputEnabledCompat(b) &&
            (_stricmp(b.mouse.c_str(), "windows") == 0 ||
             _stricmp(b.mouse.c_str(), "windows3") == 0)) {
            return SendBoundMouseMessageCompat(p, WM_MOUSEMOVE, 0);
        }

        POINT screen{
            static_cast<LONG>(x),
            static_cast<LONG>(y)
        };
        if (!::ClientToScreen(b.hwnd, &screen)) return 0;
        return MoveMouseAbsoluteCompat(screen.x, screen.y);
    }
    return MoveMouseAbsoluteCompat(x, y);
}

long MoveMouseRelativeForObjectCompat(DmImpl *p, long rx, long ry) {
    if (!p) return 0;
    const auto b = BindingSnapshotForObjectCompat(p);
    if (b.hwnd && ::IsWindow(b.hwnd) &&
        BoundInputEnabledCompat(b) &&
        (_stricmp(b.mouse.c_str(), "windows") == 0 ||
         _stricmp(b.mouse.c_str(), "windows3") == 0)) {
        long x = b.mouse_x + rx;
        long y = b.mouse_y + ry;
        RECT rc{};
        if (::GetClientRect(b.hwnd, &rc)) {
            x = std::clamp<long>(x, rc.left, std::max<LONG>(rc.left, rc.right - 1));
            y = std::clamp<long>(y, rc.top, std::max<LONG>(rc.top, rc.bottom - 1));
        }
        {
            std::lock_guard<std::mutex> lock(p->state_mutex);
            p->virtual_mouse_x = x;
            p->virtual_mouse_y = y;
        }
        return SendBoundMouseMessageCompat(p, WM_MOUSEMOVE, 0);
    }
    return SendMouseCompat(MOUSEEVENTF_MOVE, rx, ry);
}

long SendMouseButtonForObjectCompat(
    DmImpl *p, UINT message, DWORD send_input_flag,
    WPARAM message_state = 0) {
    if (!p) return 0;
    const auto b = BindingSnapshotForObjectCompat(p);
    if (BoundInputEnabledCompat(b) &&
        (_stricmp(b.mouse.c_str(), "windows") == 0 ||
         _stricmp(b.mouse.c_str(), "windows3") == 0))
        return SendBoundMouseMessageCompat(p, message, message_state);
    return SendMouseCompat(send_input_flag);
}

long SendMouseWheelForObjectCompat(DmImpl *p, long delta) {
    if (!p) return 0;
    const auto b = BindingSnapshotForObjectCompat(p);
    if (BoundInputEnabledCompat(b) &&
        (_stricmp(b.mouse.c_str(), "windows") == 0 ||
         _stricmp(b.mouse.c_str(), "windows3") == 0)) {
        return SendBoundMouseMessageCompat(
            p, WM_MOUSEWHEEL,
            MAKEWPARAM(0, static_cast<short>(delta)));
    }
    return SendMouseCompat(
        MOUSEEVENTF_WHEEL, 0, 0,
        static_cast<DWORD>(static_cast<LONG>(delta)));
}

HWND ResolveBoundKeyboardTargetCompat(DmImpl *p) {
    if (!p) return nullptr;
    const auto b = BindingSnapshotForObjectCompat(p);
    if (!BoundInputEnabledCompat(b) ||
        _stricmp(b.keypad.c_str(), "windows") != 0)
        return nullptr;

    const DWORD tid =
        ::GetWindowThreadProcessId(b.hwnd, nullptr);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (tid && ::GetGUIThreadInfo(tid, &info) &&
        info.hwndFocus &&
        (info.hwndFocus == b.hwnd ||
         ::IsChild(b.hwnd, info.hwndFocus)))
        return info.hwndFocus;
    return b.hwnd;
}

long SendKeyboardVkForObjectCompat(
    DmImpl *p, long vk, bool key_up) {
    if (!p || vk < 0 || vk > 0xFF) return 0;
    HWND target = ResolveBoundKeyboardTargetCompat(p);
    if (!target) return SendKeyboardVkCompat(vk, key_up);

    const UINT scan =
        ::MapVirtualKeyA(
            static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    LPARAM lp = 1 |
        (static_cast<LPARAM>(scan & 0xff) << 16);
    if (IsExtendedVkCompat(vk))
        lp |= (1L << 24);
    if (key_up)
        lp |= (1L << 30) | (1L << 31);

    return ::PostMessageA(
        target,
        key_up ? WM_KEYUP : WM_KEYDOWN,
        static_cast<WPARAM>(vk), lp) ? 1 : 0;
}

bool PressModifierStateForObjectCompat(
    DmImpl *p, BYTE state, bool down) {
    const long modifiers[] = {
        VK_SHIFT, VK_CONTROL, VK_MENU
    };
    for (int i = down ? 0 : 2;
         down ? i < 3 : i >= 0;
         down ? ++i : --i) {
        if (state & (1u << i)) {
            if (!SendKeyboardVkForObjectCompat(
                    p, modifiers[i], !down))
                return false;
        }
    }
    return true;
}

struct WordResultCompat {
    std::vector<std::string> xs;
    std::vector<std::string> ys;
    std::vector<std::string> words;
    bool has_first_pipe = false;
    bool has_second_pipe = false;
};

WordResultCompat ParseWordResultCompat(PCSTR str) {
    WordResultCompat out;
    if (!str || !*str) return out;

    const std::string text(str);
    const size_t p1 = text.find('|');
    if (p1 == std::string::npos) return out;
    out.has_first_pipe = true;
    out.xs = SplitCompat(text.substr(0, p1), ',');

    const size_t p2 = text.find('|', p1 + 1);
    if (p2 == std::string::npos) return out;
    out.has_second_pipe = true;
    out.ys = SplitCompat(text.substr(p1 + 1, p2 - p1 - 1), ',');

    size_t begin = p2 + 1;
    while (begin <= text.size()) {
        const size_t p = text.find('|', begin);
        if (p == std::string::npos) {
            out.words.push_back(text.substr(begin));
            break;
        }
        out.words.push_back(text.substr(begin, p - begin));
        begin = p + 1;
    }
    return out;
}

bool ParseDecimalFieldCompat(const std::string &s, long &value) {
    if (s.empty()) return false;
    char *end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return false;
    value = v;
    return true;
}


long KeyNameToVkCompat(PCSTR key_str) {
    if (!key_str || !*key_str) return 0;

    std::string key(key_str);
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::unordered_map<std::string, long> kMap = {
        {"backspace", VK_BACK}, {"tab", VK_TAB}, {"clear", VK_CLEAR},
        {"enter", VK_RETURN}, {"shift", VK_SHIFT}, {"ctrl", VK_CONTROL},
        {"control", VK_CONTROL}, {"alt", VK_MENU}, {"pause", VK_PAUSE},
        {"caps_lock", VK_CAPITAL}, {"capslock", VK_CAPITAL},
        {"esc", VK_ESCAPE}, {"escape", VK_ESCAPE},
        {"spacebar", VK_SPACE}, {"space", VK_SPACE},
        {"page_up", VK_PRIOR}, {"pageup", VK_PRIOR},
        {"page_down", VK_NEXT}, {"pagedown", VK_NEXT},
        {"end", VK_END}, {"home", VK_HOME},
        {"left_arrow", VK_LEFT}, {"left", VK_LEFT},
        {"up_arrow", VK_UP}, {"up", VK_UP},
        {"right_arrow", VK_RIGHT}, {"right", VK_RIGHT},
        {"down_arrow", VK_DOWN}, {"down", VK_DOWN},
        {"select", VK_SELECT}, {"print", VK_PRINT}, {"execute", VK_EXECUTE},
        {"print_screen", VK_SNAPSHOT}, {"printscreen", VK_SNAPSHOT},
        {"ins", VK_INSERT}, {"insert", VK_INSERT},
        {"del", VK_DELETE}, {"delete", VK_DELETE}, {"help", VK_HELP},
        {"num_lock", VK_NUMLOCK}, {"numlock", VK_NUMLOCK},
        {"scroll_lock", VK_SCROLL}, {"scrolllock", VK_SCROLL},
        {"left_shift", VK_LSHIFT}, {"right_shift", VK_RSHIFT},
        {"left_control", VK_LCONTROL}, {"right_control", VK_RCONTROL},
        {"left_ctrl", VK_LCONTROL}, {"right_ctrl", VK_RCONTROL},
        {"left_menu", VK_LMENU}, {"right_menu", VK_RMENU},
        {"left_alt", VK_LMENU}, {"right_alt", VK_RMENU},
        {"browser_back", VK_BROWSER_BACK}, {"browser_forward", VK_BROWSER_FORWARD},
        {"browser_refresh", VK_BROWSER_REFRESH}, {"browser_stop", VK_BROWSER_STOP},
        {"browser_search", VK_BROWSER_SEARCH}, {"browser_favorites", VK_BROWSER_FAVORITES},
        {"browser_start_and_home", VK_BROWSER_HOME}, {"browser_home", VK_BROWSER_HOME},
        {"volume_mute", VK_VOLUME_MUTE}, {"volume_down", VK_VOLUME_DOWN},
        {"volume_up", VK_VOLUME_UP}, {"next_track", VK_MEDIA_NEXT_TRACK},
        {"previous_track", VK_MEDIA_PREV_TRACK}, {"stop_media", VK_MEDIA_STOP},
        {"play/pause_media", VK_MEDIA_PLAY_PAUSE}, {"play_pause_media", VK_MEDIA_PLAY_PAUSE},
        {"start_mail", VK_LAUNCH_MAIL}, {"select_media", VK_LAUNCH_MEDIA_SELECT},
        {"start_application_1", VK_LAUNCH_APP1}, {"start_application_2", VK_LAUNCH_APP2},
        {"attn_key", VK_ATTN}, {"crsel_key", VK_CRSEL}, {"exsel_key", VK_EXSEL},
        {"play_key", VK_PLAY}, {"zoom_key", VK_ZOOM}, {"clear_key", VK_OEM_CLEAR},
        {"multiply_key", VK_MULTIPLY}, {"add_key", VK_ADD},
        {"separator_key", VK_SEPARATOR}, {"subtract_key", VK_SUBTRACT},
        {"decimal_key", VK_DECIMAL}, {"divide_key", VK_DIVIDE},
    };

    const auto it = kMap.find(key);
    if (it != kMap.end()) return it->second;

    if (key.size() >= 2 && key[0] == 'f') {
        char *end = nullptr;
        const long n = std::strtol(key.c_str() + 1, &end, 10);
        if (end && *end == '\0' && n >= 1 && n <= 24)
            return VK_F1 + n - 1;
    }

    if (key.rfind("numpad_", 0) == 0 && key.size() == 8 &&
        key[7] >= '0' && key[7] <= '9')
        return VK_NUMPAD0 + (key[7] - '0');

    if (key.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(key[0]);
        if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
        if (c >= '0' && c <= '9') return c;

        const SHORT mapped = ::VkKeyScanA(static_cast<char>(c));
        if (mapped != -1) return LOBYTE(mapped);
    }

    return 0;
}

bool PressModifierStateCompat(BYTE state, bool down) {
    // VkKeyScan high byte uses bit0=SHIFT, bit1=CTRL, bit2=ALT.
    const long modifiers[] = {VK_SHIFT, VK_CONTROL, VK_MENU};
    for (int i = down ? 0 : 2; down ? i < 3 : i >= 0; down ? ++i : --i) {
        if (state & (1u << i)) {
            if (!SendKeyboardVkCompat(modifiers[i], !down)) return false;
        }
    }
    return true;
}

long KeyPressCharacterCompat(DmImpl *p, unsigned char ch) {
    if (!p) return 0;
    const SHORT mapped = ::VkKeyScanA(static_cast<char>(ch));
    if (mapped == -1) return 0;

    const long vk = LOBYTE(mapped);
    const BYTE state = HIBYTE(mapped);
    if (!PressModifierStateForObjectCompat(p, state, true)) return 0;
    const long ret = SendKeyboardVkForObjectCompat(p, vk, false);
    if (ret) ::Sleep(static_cast<DWORD>(std::max<long>(0, (_stricmp(BindingSnapshotForObjectCompat(p).keypad.c_str(), "windows") == 0
         ? p->keypad_delay_windows : p->keypad_delay_normal))));
    const long up = ret ? SendKeyboardVkForObjectCompat(p, vk, true) : 0;
    PressModifierStateForObjectCompat(p, state, false);
    return ret && up ? 1 : 0;
}

HWND ResolvePasteTargetCompat(long hwnd) {
    if (hwnd != 0) return HwndFromLong(hwnd);

    HWND foreground = ::GetForegroundWindow();
    if (!foreground) return nullptr;
    const DWORD tid = ::GetWindowThreadProcessId(foreground, nullptr);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (tid && ::GetGUIThreadInfo(tid, &info) && info.hwndFocus)
        return info.hwndFocus;
    return foreground;
}

long SendPasteCompat(long hwnd) {
    HWND target = ResolvePasteTargetCompat(hwnd);
    if (!target || !::IsWindow(target)) return 0;

    const DWORD target_tid = ::GetWindowThreadProcessId(target, nullptr);
    const DWORD self_tid = ::GetCurrentThreadId();
    const BOOL attached =
        target_tid && target_tid != self_tid
            ? ::AttachThreadInput(self_tid, target_tid, TRUE)
            : FALSE;

    const LRESULT result = ::SendMessageA(target, WM_PASTE, 0, 0);

    if (attached) ::AttachThreadInput(self_tid, target_tid, FALSE);
    (void)result;
    return 1;
}


long SendStringAnsiCompat(long hwnd, PCSTR str) {
    if (!str) return 0;
    HWND target = ResolvePasteTargetCompat(hwnd);
    if (!target || !::IsWindow(target)) return 0;

    const DWORD target_tid = ::GetWindowThreadProcessId(target, nullptr);
    const DWORD self_tid = ::GetCurrentThreadId();
    const BOOL attached =
        target_tid && target_tid != self_tid
            ? ::AttachThreadInput(self_tid, target_tid, TRUE)
            : FALSE;

    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(str);
         *p; ++p) {
        ::SendMessageA(
            target,
            WM_CHAR,
            static_cast<WPARAM>(*p),
            static_cast<LPARAM>(0x04000001));
    }

    if (attached) ::AttachThreadInput(self_tid, target_tid, FALSE);
    return 1;
}


std::string ProcessImagePathCompat(DWORD pid) {
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    std::vector<char> buf(32768, 0);
    DWORD size = static_cast<DWORD>(buf.size());
    std::string out;
    if (::QueryFullProcessImageNameA(process, 0, buf.data(), &size))
        out.assign(buf.data(), size);
    ::CloseHandle(process);
    return out;
}

std::string BaseNameCompat(const std::string &path) {
    if (path.empty()) return {};
    const size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}


std::string TrimStorageStringCompat(const char *text) {
    if (!text) return {};
    std::string out(text);
    while (!out.empty() &&
           (out.back() == ' ' || out.back() == '\t' || out.back() == '\r' || out.back() == '\n' || out.back() == '\0'))
        out.pop_back();
    size_t begin = 0;
    while (begin < out.size() && (out[begin] == ' ' || out[begin] == '\t')) ++begin;
    if (begin) out.erase(0, begin);
    return out;
}

struct DiskDescriptorCompat {
    std::string vendor;
    std::string product;
    std::string revision;
    std::string serial;
};

bool QueryDiskDescriptorCompat(long index, DiskDescriptorCompat &out) {
    if (index < 0 || index > 5) return false;
    char device[64]{};
    std::snprintf(device, sizeof(device), "\\\\.\\PhysicalDrive%ld", index);

    HANDLE h = ::CreateFileA(
        device,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    std::vector<unsigned char> buffer(4096, 0);
    DWORD returned = 0;
    const BOOL ok = ::DeviceIoControl(
        h,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query,
        sizeof(query),
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &returned,
        nullptr);
    ::CloseHandle(h);
    if (!ok || returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return false;

    const auto *d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buffer.data());
    auto field = [&](DWORD offset) -> std::string {
        if (offset == 0 || offset >= returned) return {};
        return TrimStorageStringCompat(
            reinterpret_cast<const char *>(buffer.data() + offset));
    };

    out.vendor = field(d->VendorIdOffset);
    out.product = field(d->ProductIdOffset);
    out.revision = field(d->ProductRevisionOffset);
    out.serial = field(d->SerialNumberOffset);
    return true;
}

std::string DiskModelCompat(const DiskDescriptorCompat &d) {
    if (d.vendor.empty()) return d.product;
    if (d.product.empty()) return d.vendor;
    if (d.product.rfind(d.vendor, 0) == 0) return d.product;
    return d.vendor + " " + d.product;
}

std::string QueryCommandLineCompat(DWORD pid) {
    HANDLE process = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
        FALSE,
        pid);
    if (!process) return {};

    using NtQueryInformationProcessFn =
        LONG (NTAPI *)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    auto query = ntdll
        ? reinterpret_cast<NtQueryInformationProcessFn>(
              ::GetProcAddress(ntdll, "NtQueryInformationProcess"))
        : nullptr;
    if (!query) {
        ::CloseHandle(process);
        return {};
    }

    constexpr ULONG kProcessCommandLineInformation = 60;
    ULONG needed = 0;
    LONG status = query(
        process,
        kProcessCommandLineInformation,
        nullptr,
        0,
        &needed);

    if (needed == 0 || needed > 1024 * 1024) {
        ::CloseHandle(process);
        return {};
    }

    std::vector<unsigned char> buffer(needed + sizeof(wchar_t) * 2, 0);
    status = query(
        process,
        kProcessCommandLineInformation,
        buffer.data(),
        static_cast<ULONG>(buffer.size()),
        &needed);
    ::CloseHandle(process);
    if (status < 0 || buffer.size() < sizeof(UNICODE_STRING)) return {};

    const auto *us = reinterpret_cast<const UNICODE_STRING *>(buffer.data());
    if (!us->Buffer || us->Length == 0) return {};

    // For ProcessCommandLineInformation the returned UNICODE_STRING buffer
    // points inside the caller supplied result block.
    const auto begin = reinterpret_cast<const unsigned char *>(us->Buffer);
    const auto base = buffer.data();
    const auto end = base + buffer.size();
    if (begin < base || begin + us->Length > end) return {};

    return WideToAcpCompat(
        reinterpret_cast<const wchar_t *>(begin),
        static_cast<int>(us->Length / sizeof(wchar_t)));
}

bool SampleProcessCpuCompat(HANDLE process, long &cpu_percent, SIZE_T &working_set) {
    cpu_percent = 0;
    working_set = 0;

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(
            process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
            sizeof(pmc)))
        working_set = pmc.WorkingSetSize;

    FILETIME sys_idle1{}, sys_kernel1{}, sys_user1{};
    FILETIME proc_create1{}, proc_exit1{}, proc_kernel1{}, proc_user1{};
    if (!::GetSystemTimes(&sys_idle1, &sys_kernel1, &sys_user1) ||
        !::GetProcessTimes(process, &proc_create1, &proc_exit1, &proc_kernel1, &proc_user1))
        return false;

    ::Sleep(1000);

    FILETIME sys_idle2{}, sys_kernel2{}, sys_user2{};
    FILETIME proc_create2{}, proc_exit2{}, proc_kernel2{}, proc_user2{};
    if (!::GetSystemTimes(&sys_idle2, &sys_kernel2, &sys_user2) ||
        !::GetProcessTimes(process, &proc_create2, &proc_exit2, &proc_kernel2, &proc_user2))
        return false;

    const ULONGLONG system_delta =
        (FileTime64(sys_kernel2) - FileTime64(sys_kernel1)) +
        (FileTime64(sys_user2) - FileTime64(sys_user1));
    const ULONGLONG process_delta =
        (FileTime64(proc_kernel2) - FileTime64(proc_kernel1)) +
        (FileTime64(proc_user2) - FileTime64(proc_user1));

    if (system_delta != 0) {
        const ULONGLONG value = (process_delta * 100ULL + system_delta / 2ULL) / system_delta;
        cpu_percent = static_cast<long>(std::min<ULONGLONG>(value, 100ULL));
    }

    pmc = {};
    pmc.cb = sizeof(pmc);
    if (::GetProcessMemoryInfo(
            process,
            reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
            sizeof(pmc)))
        working_set = pmc.WorkingSetSize;
    return true;
}

std::string DisplayInfoCompat() {
    DISPLAY_DEVICEA adapter{};
    adapter.cb = sizeof(adapter);

    std::vector<std::string> names;
    for (DWORD i = 0; ::EnumDisplayDevicesA(nullptr, i, &adapter, 0); ++i) {
        if (!(adapter.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) {
            adapter = {};
            adapter.cb = sizeof(adapter);
            continue;
        }
        const std::string name = TrimStorageStringCompat(adapter.DeviceString);
        if (!name.empty() &&
            std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
        adapter = {};
        adapter.cb = sizeof(adapter);
    }

    std::ostringstream oss;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) oss << '|';
        oss << names[i];
    }
    return oss.str();
}


struct RgbColorCompat {
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
};

struct ColorRuleCompat {
    RgbColorCompat color;
    RgbColorCompat diff;
    bool explicit_diff = false;
};

struct ColorSpecCompat {
    bool inverse = false;
    std::vector<ColorRuleCompat> rules;
};

struct ScreenImageCompat {
    long x = 0;
    long y = 0;
    long width = 0;
    long height = 0;
    std::vector<RgbColorCompat> pixels;

    const RgbColorCompat *At(long screen_x, long screen_y) const {
        const long lx = screen_x - x;
        const long ly = screen_y - y;
        if (lx < 0 || ly < 0 || lx >= width || ly >= height) return nullptr;
        return &pixels[static_cast<size_t>(ly) * static_cast<size_t>(width) +
                       static_cast<size_t>(lx)];
    }
};

bool CropScreenImageCompat(
    const ScreenImageCompat &source,
    long x1, long y1, long x2, long y2,
    ScreenImageCompat &out) {
    if (x2 < x1 || y2 < y1 ||
        x1 < source.x || y1 < source.y ||
        x2 >= source.x + source.width ||
        y2 >= source.y + source.height)
        return false;

    out = {};
    out.x = x1;
    out.y = y1;
    out.width = x2 - x1 + 1;
    out.height = y2 - y1 + 1;
    out.pixels.resize(
        static_cast<size_t>(out.width) *
        static_cast<size_t>(out.height));

    for (long y = 0; y < out.height; ++y) {
        for (long x = 0; x < out.width; ++x) {
            const auto *pixel = source.At(x1 + x, y1 + y);
            if (!pixel) return false;
            out.pixels[
                static_cast<size_t>(y) *
                    static_cast<size_t>(out.width) +
                static_cast<size_t>(x)] = *pixel;
        }
    }
    return true;
}

thread_local std::shared_ptr<ScreenImageCompat> g_last_graphic_capture;

bool CaptureScreenRegionCompat(long x1, long y1, long x2, long y2, ScreenImageCompat &out) {
    out = {};
    if (x2 < x1 || y2 < y1) return false;

    const long long w64 = static_cast<long long>(x2) - x1 + 1;
    const long long h64 = static_cast<long long>(y2) - y1 + 1;
    if (w64 <= 0 || h64 <= 0 ||
        w64 > std::numeric_limits<int>::max() ||
        h64 > std::numeric_limits<int>::max() ||
        static_cast<unsigned long long>(w64) * static_cast<unsigned long long>(h64) >
            256ULL * 1024ULL * 1024ULL)
        return false;

    const int width = static_cast<int>(w64);
    const int height = static_cast<int>(h64);
    HDC screen = ::GetDC(nullptr);
    if (!screen) return false;
    HDC memory = ::CreateCompatibleDC(screen);
    if (!memory) {
        ::ReleaseDC(nullptr, screen);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(
        screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) ::DeleteObject(bitmap);
        ::DeleteDC(memory);
        ::ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ old = ::SelectObject(memory, bitmap);
    const BOOL copied = ::BitBlt(
        memory, 0, 0, width, height, screen, x1, y1, SRCCOPY | CAPTUREBLT);

    if (old) ::SelectObject(memory, old);

    bool ok = copied != FALSE;
    if (ok) {
        out.x = x1;
        out.y = y1;
        out.width = width;
        out.height = height;
        out.pixels.resize(static_cast<size_t>(width) * static_cast<size_t>(height));

        const auto *src = static_cast<const unsigned char *>(bits);
        for (size_t i = 0; i < out.pixels.size(); ++i) {
            out.pixels[i].b = src[i * 4 + 0];
            out.pixels[i].g = src[i * 4 + 1];
            out.pixels[i].r = src[i * 4 + 2];
        }
        g_last_graphic_capture =
            std::make_shared<ScreenImageCompat>(out);
    }

    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return ok;
}



bool CaptureBoundClientRegionCompat(
    HWND hwnd, const std::string &display,
    long x1, long y1, long x2, long y2,
    ScreenImageCompat &out) {
    out = {};
    if (!hwnd || !::IsWindow(hwnd) ||
        x2 < x1 || y2 < y1)
        return false;

    RECT client{};
    if (!::GetClientRect(hwnd, &client)) return false;
    const long client_w = client.right - client.left;
    const long client_h = client.bottom - client.top;
    if (client_w <= 0 || client_h <= 0) return false;

    if (_stricmp(display.c_str(), "normal") == 0) {
        POINT origin{0,0};
        if (!::ClientToScreen(hwnd, &origin)) return false;
        if (!CaptureScreenRegionCompat(
                origin.x + x1, origin.y + y1,
                origin.x + x2, origin.y + y2, out))
            return false;
        out.x = x1;
        out.y = y1;
        return true;
    }

    if (_stricmp(display.c_str(), "gdi") != 0 &&
        _stricmp(display.c_str(), "gdi2") != 0)
        return false;

    const long width = x2 - x1 + 1;
    const long height = y2 - y1 + 1;
    if (width <= 0 || height <= 0) return false;

    HDC source = ::GetDC(hwnd);
    if (!source) return false;
    HDC memory = ::CreateCompatibleDC(source);
    if (!memory) {
        ::ReleaseDC(hwnd, source);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(
        source, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits) {
        if (bitmap) ::DeleteObject(bitmap);
        ::DeleteDC(memory);
        ::ReleaseDC(hwnd, source);
        return false;
    }
    HGDIOBJ old = ::SelectObject(memory, bitmap);

    BOOL copied = FALSE;
    if (_stricmp(display.c_str(), "gdi") == 0) {
        copied = ::BitBlt(
            memory, 0, 0, width, height,
            source, x1, y1, SRCCOPY | CAPTUREBLT);
    } else {
        // gdi2 favors PrintWindow for windows that do not repaint their DC
        // while covered. Render the full client and crop with a temporary DC.
        HDC full_dc = ::CreateCompatibleDC(source);
        BITMAPINFO full_bmi{};
        full_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        full_bmi.bmiHeader.biWidth = client_w;
        full_bmi.bmiHeader.biHeight = -client_h;
        full_bmi.bmiHeader.biPlanes = 1;
        full_bmi.bmiHeader.biBitCount = 32;
        full_bmi.bmiHeader.biCompression = BI_RGB;
        void *full_bits = nullptr;
        HBITMAP full_bmp = ::CreateDIBSection(
            source, &full_bmi, DIB_RGB_COLORS,
            &full_bits, nullptr, 0);
        if (full_dc && full_bmp && full_bits) {
            HGDIOBJ full_old = ::SelectObject(full_dc, full_bmp);
            constexpr UINT kPwClientOnly = 0x00000001;
            constexpr UINT kPwRenderFullContent = 0x00000002;
            const BOOL printed = ::PrintWindow(
                hwnd, full_dc, kPwClientOnly | kPwRenderFullContent);
            if (printed) {
                copied = ::BitBlt(
                    memory, 0, 0, width, height,
                    full_dc, x1, y1, SRCCOPY);
            }
            if (full_old) ::SelectObject(full_dc, full_old);
        }
        if (full_bmp) ::DeleteObject(full_bmp);
        if (full_dc) ::DeleteDC(full_dc);
    }

    if (old) ::SelectObject(memory, old);

    if (copied) {
        out.x = x1;
        out.y = y1;
        out.width = width;
        out.height = height;
        out.pixels.resize(
            static_cast<size_t>(width) *
            static_cast<size_t>(height));
        const auto *src =
            static_cast<const unsigned char *>(bits);
        for (size_t i = 0; i < out.pixels.size(); ++i) {
            out.pixels[i].b = src[i*4+0];
            out.pixels[i].g = src[i*4+1];
            out.pixels[i].r = src[i*4+2];
        }
    }

    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(hwnd, source);
    return copied != FALSE;
}

bool CaptureScreenRegionBaseForObjectCompat(
    DmImpl *p, long x1, long y1, long x2, long y2,
    ScreenImageCompat &out) {
    if (!p) return CaptureScreenRegionCompat(x1,y1,x2,y2,out);

    HWND hwnd = nullptr;
    std::string display = "normal";
    long bind_enable = 1;
    bool display_locked = false;
    std::shared_ptr<ScreenImageCompat> locked_display;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        hwnd = p->bound_hwnd;
        display = p->bind_display;
        bind_enable = p->bind_enable;
        display_locked = p->display_locked;
        locked_display = p->locked_display;
    }

    if (display_locked && locked_display &&
        CropScreenImageCompat(*locked_display, x1, y1, x2, y2, out))
        return true;
    if (!hwnd || !::IsWindow(hwnd))
        return CaptureScreenRegionCompat(x1,y1,x2,y2,out);

    // EnableBind -1/0/5 makes graphics act as normal/front mode while
    // preserving the binding and client-coordinate convention.
    if (bind_enable != 1) display = "normal";
    return CaptureBoundClientRegionCompat(
        hwnd, display, x1,y1,x2,y2,out);
}

bool CaptureScreenRegionForObjectCompat(
    DmImpl *p, long x1, long y1, long x2, long y2, ScreenImageCompat &out) {
    if (!CaptureScreenRegionBaseForObjectCompat(p, x1, y1, x2, y2, out)) return false;
    if (!p) return true;

    std::vector<ExcludeRegionCompat> regions;
    unsigned int rgb = 0xFF00FF;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        regions = p->exclude_regions;
        rgb = p->exclude_region_rgb;
    }
    if (regions.empty()) return true;

    RgbColorCompat replacement{};
    replacement.r = static_cast<unsigned char>((rgb >> 16) & 0xFF);
    replacement.g = static_cast<unsigned char>((rgb >> 8) & 0xFF);
    replacement.b = static_cast<unsigned char>(rgb & 0xFF);

    const long image_x2 = out.x + out.width - 1;
    const long image_y2 = out.y + out.height - 1;
    for (const auto &r : regions) {
        const long left = std::max(out.x, std::min(r.x1, r.x2));
        const long top = std::max(out.y, std::min(r.y1, r.y2));
        const long right = std::min(image_x2, std::max(r.x1, r.x2));
        const long bottom = std::min(image_y2, std::max(r.y1, r.y2));
        if (left > right || top > bottom) continue;

        for (long y = top; y <= bottom; ++y) {
            const size_t row =
                static_cast<size_t>(y - out.y) *
                static_cast<size_t>(out.width);
            for (long x = left; x <= right; ++x) {
                out.pixels[row + static_cast<size_t>(x - out.x)] = replacement;
            }
        }
    }

    // CapturePre and later image consumers should see the same modified frame.
    g_last_graphic_capture = std::make_shared<ScreenImageCompat>(out);
    return true;
}

bool ReadScreenPixelCompat(DmImpl *p, long x, long y, RgbColorCompat &out) {
    if (p) {
        HWND bound = nullptr;
        bool by_capture = false;
        {
            std::lock_guard<std::mutex> lock(p->state_mutex);
            bound = p->bound_hwnd;
            by_capture = p->get_color_by_capture;
        }
        if (bound || by_capture) {
            ScreenImageCompat image;
            if (!CaptureScreenRegionBaseForObjectCompat(
                    p, x, y, x, y, image) ||
                image.pixels.empty())
                return false;
            out = image.pixels.front();
            return true;
        }
    }

    HDC dc = ::GetDC(nullptr);
    if (!dc) return false;
    const COLORREF c = ::GetPixel(dc, x, y);
    ::ReleaseDC(nullptr, dc);
    if (c == CLR_INVALID) return false;
    out.r = GetRValue(c);
    out.g = GetGValue(c);
    out.b = GetBValue(c);
    return true;
}

bool ParseHexByteCompat(const std::string &s, size_t off, unsigned char &out) {
    if (off + 2 > s.size()) return false;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const int hi = hex(s[off]);
    const int lo = hex(s[off + 1]);
    if (hi < 0 || lo < 0) return false;
    out = static_cast<unsigned char>((hi << 4) | lo);
    return true;
}

bool ParseRgbHexCompat(const std::string &s, RgbColorCompat &out) {
    if (s.size() != 6) return false;
    return ParseHexByteCompat(s, 0, out.r) &&
           ParseHexByteCompat(s, 2, out.g) &&
           ParseHexByteCompat(s, 4, out.b);
}

bool ParseColorSpecCompat(PCSTR text, ColorSpecCompat &out) {
    out = {};
    if (!text || !*text) return false;

    std::string source(text);
    if (!source.empty() && source.front() == '@') {
        out.inverse = true;
        source.erase(source.begin());
    }
    if (source.empty()) return false;

    const auto parts = SplitCompat(source, '|');
    for (const auto &part : parts) {
        if (part.empty()) continue;
        const size_t dash = part.find('-');
        const std::string color_text =
            dash == std::string::npos ? part : part.substr(0, dash);
        const std::string diff_text =
            dash == std::string::npos ? std::string() : part.substr(dash + 1);

        ColorRuleCompat rule{};
        if (!ParseRgbHexCompat(color_text, rule.color)) return false;
        if (!diff_text.empty()) {
            if (!ParseRgbHexCompat(diff_text, rule.diff)) return false;
            rule.explicit_diff = true;
        }
        out.rules.push_back(rule);
        if (out.rules.size() >= 10) break;
    }
    return !out.rules.empty();
}

unsigned char SimDiffCompat(double sim) {
    if (sim < 0.0 || sim > 1.0) sim = 1.0;
    const double raw = std::ceil((1.0 - sim) * 255.0);
    return static_cast<unsigned char>(std::clamp(raw, 0.0, 255.0));
}

bool MatchOneColorCompat(
    const RgbColorCompat &actual, const ColorRuleCompat &rule, double sim) {
    const unsigned char implicit = SimDiffCompat(sim);
    const unsigned char dr = rule.explicit_diff ? rule.diff.r : implicit;
    const unsigned char dg = rule.explicit_diff ? rule.diff.g : implicit;
    const unsigned char db = rule.explicit_diff ? rule.diff.b : implicit;
    return std::abs(static_cast<int>(actual.r) - static_cast<int>(rule.color.r)) <= dr &&
           std::abs(static_cast<int>(actual.g) - static_cast<int>(rule.color.g)) <= dg &&
           std::abs(static_cast<int>(actual.b) - static_cast<int>(rule.color.b)) <= db;
}

bool MatchColorSpecCompat(
    const RgbColorCompat &actual, const ColorSpecCompat &spec, double sim) {
    bool matched = false;
    for (const auto &rule : spec.rules) {
        if (MatchOneColorCompat(actual, rule, sim)) {
            matched = true;
            break;
        }
    }
    return spec.inverse ? !matched : matched;
}

std::string RgbHexCompat(const RgbColorCompat &c) {
    char buf[7]{};
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string BgrHexCompat(const RgbColorCompat &c) {
    char buf[7]{};
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x", c.b, c.g, c.r);
    return buf;
}

struct HsvColorCompat {
    long h = 0;
    long s = 0;
    long v = 0;
};

HsvColorCompat RgbToHsvCompat(const RgbColorCompat &c) {
    const double r = static_cast<double>(c.r) / 255.0;
    const double g = static_cast<double>(c.g) / 255.0;
    const double b = static_cast<double>(c.b) / 255.0;
    const double maxv = std::max({r, g, b});
    const double minv = std::min({r, g, b});
    const double delta = maxv - minv;

    double h = 0.0;
    if (delta > 0.0) {
        if (maxv == r)
            h = 60.0 * std::fmod((g - b) / delta, 6.0);
        else if (maxv == g)
            h = 60.0 * (((b - r) / delta) + 2.0);
        else
            h = 60.0 * (((r - g) / delta) + 4.0);
        if (h < 0.0) h += 360.0;
    }

    const double saturation = maxv <= 0.0 ? 0.0 : delta / maxv;
    HsvColorCompat out{};
    out.h = static_cast<long>(std::lround(h));
    if (out.h >= 360) out.h = 0;
    out.s = static_cast<long>(std::lround(saturation * 100.0));
    out.v = static_cast<long>(std::lround(maxv * 100.0));
    return out;
}

std::string HsvStringCompat(const RgbColorCompat &c) {
    const auto hsv = RgbToHsvCompat(c);
    return std::to_string(hsv.h) + "." +
           std::to_string(hsv.s) + "." +
           std::to_string(hsv.v);
}

RgbColorCompat AverageRgbCompat(const ScreenImageCompat &image) {
    RgbColorCompat result{};
    if (image.pixels.empty()) return result;
    unsigned long long sr = 0, sg = 0, sb = 0;
    for (const auto &p : image.pixels) {
        sr += p.r;
        sg += p.g;
        sb += p.b;
    }
    const unsigned long long n = image.pixels.size();
    result.r = static_cast<unsigned char>(sr / n);
    result.g = static_cast<unsigned char>(sg / n);
    result.b = static_cast<unsigned char>(sb / n);
    return result;
}

template <typename Fn>
bool ForEachPointInDirectionCompat(
    long x1, long y1, long x2, long y2, long dir, Fn &&fn) {
    if (x2 < x1 || y2 < y1) return false;
    if (dir < 0 || dir > 8) dir = 0;

    if (dir == 4) {
        struct Node { long x; long y; unsigned long long d; };
        std::vector<Node> nodes;
        const size_t width = static_cast<size_t>(x2 - x1 + 1);
        const size_t height = static_cast<size_t>(y2 - y1 + 1);
        if (width > 0 && height > std::numeric_limits<size_t>::max() / width)
            return false;
        nodes.reserve(width * height);
        const long long cx2 = static_cast<long long>(x1) + x2;
        const long long cy2 = static_cast<long long>(y1) + y2;
        for (long y = y1; y <= y2; ++y) {
            for (long x = x1; x <= x2; ++x) {
                const long long dx = static_cast<long long>(x) * 2 - cx2;
                const long long dy = static_cast<long long>(y) * 2 - cy2;
                nodes.push_back({x, y, static_cast<unsigned long long>(dx * dx + dy * dy)});
            }
        }
        std::stable_sort(nodes.begin(), nodes.end(), [](const Node &a, const Node &b) {
            if (a.d != b.d) return a.d < b.d;
            if (a.y != b.y) return a.y < b.y;
            return a.x < b.x;
        });
        for (const auto &n : nodes)
            if (fn(n.x, n.y)) return true;
        return false;
    }

    auto xs = [&](bool reverse, auto &&body) {
        if (!reverse) {
            for (long x = x1; x <= x2; ++x) if (body(x)) return true;
        } else {
            for (long x = x2;; --x) {
                if (body(x)) return true;
                if (x == x1) break;
            }
        }
        return false;
    };
    auto ys = [&](bool reverse, auto &&body) {
        if (!reverse) {
            for (long y = y1; y <= y2; ++y) if (body(y)) return true;
        } else {
            for (long y = y2;; --y) {
                if (body(y)) return true;
                if (y == y1) break;
            }
        }
        return false;
    };

    switch (dir) {
    case 0: return ys(false, [&](long y){ return xs(false, [&](long x){ return fn(x,y); }); });
    case 1: return ys(true,  [&](long y){ return xs(false, [&](long x){ return fn(x,y); }); });
    case 2: return ys(false, [&](long y){ return xs(true,  [&](long x){ return fn(x,y); }); });
    case 3: return ys(true,  [&](long y){ return xs(true,  [&](long x){ return fn(x,y); }); });
    case 5: return xs(false, [&](long x){ return ys(false, [&](long y){ return fn(x,y); }); });
    case 6: return xs(true,  [&](long x){ return ys(false, [&](long y){ return fn(x,y); }); });
    case 7: return xs(false, [&](long x){ return ys(true,  [&](long y){ return fn(x,y); }); });
    case 8: return xs(true,  [&](long x){ return ys(true,  [&](long y){ return fn(x,y); }); });
    default: return false;
    }
}


struct SignedColorRuleCompat {
    bool negative = false;
    ColorRuleCompat rule;
};

struct MultiColorOffsetCompat {
    long dx = 0;
    long dy = 0;
    std::vector<SignedColorRuleCompat> rules;
};

bool ParseSignedColorRuleCompat(const std::string &text, SignedColorRuleCompat &out) {
    if (text.empty()) return false;
    std::string value = text;
    if (!value.empty() && value.front() == '-') {
        out.negative = true;
        value.erase(value.begin());
    }
    if (value.empty()) return false;

    const size_t dash = value.find('-');
    const std::string color_text =
        dash == std::string::npos ? value : value.substr(0, dash);
    const std::string diff_text =
        dash == std::string::npos ? std::string() : value.substr(dash + 1);

    if (!ParseRgbHexCompat(color_text, out.rule.color)) return false;
    if (!diff_text.empty()) {
        if (!ParseRgbHexCompat(diff_text, out.rule.diff)) return false;
        out.rule.explicit_diff = true;
    }
    return true;
}

bool ParseMultiColorOffsetsCompat(PCSTR text, std::vector<MultiColorOffsetCompat> &out) {
    out.clear();
    if (!text || !*text) return true;

    const auto entries = SplitCompat(text, ',');
    for (const auto &entry : entries) {
        if (entry.empty()) continue;
        const auto parts = SplitCompat(entry, '|');
        if (parts.size() < 3) return false;

        MultiColorOffsetCompat item{};
        if (!ParseLongCompat(parts[0], item.dx) ||
            !ParseLongCompat(parts[1], item.dy))
            return false;

        for (size_t i = 2; i < parts.size(); ++i) {
            SignedColorRuleCompat rule{};
            if (!ParseSignedColorRuleCompat(parts[i], rule)) return false;
            item.rules.push_back(rule);
        }
        if (item.rules.empty()) return false;
        out.push_back(std::move(item));
    }
    return true;
}

bool MatchSignedColorRulesCompat(
    const RgbColorCompat &actual,
    const std::vector<SignedColorRuleCompat> &rules,
    double sim) {
    bool has_positive = false;
    bool positive_match = false;

    for (const auto &rule : rules) {
        const bool matched = MatchOneColorCompat(actual, rule.rule, sim);
        if (rule.negative) {
            if (matched) return false;
        } else {
            has_positive = true;
            if (matched) positive_match = true;
        }
    }
    return has_positive ? positive_match : true;
}

bool MatchMultiColorAtCompat(
    const ScreenImageCompat &image,
    long x,
    long y,
    const ColorSpecCompat &first,
    const std::vector<MultiColorOffsetCompat> &offsets,
    double sim) {
    const auto *base = image.At(x, y);
    if (!base || !MatchColorSpecCompat(*base, first, sim)) return false;

    long error_count = 0;
    const long max_error =
        static_cast<long>(static_cast<double>(offsets.size()) * (1.0 - std::clamp(sim, 0.0, 1.0)));

    for (const auto &off : offsets) {
        const auto *pixel = image.At(x + off.dx, y + off.dy);
        const bool matched =
            pixel && MatchSignedColorRulesCompat(*pixel, off.rules, sim);
        if (!matched && ++error_count > max_error) return false;
    }
    return true;
}

struct ShapeOffsetCompat {
    long dx = 0;
    long dy = 0;
    bool equal = false;
};

bool ParseShapeOffsetsCompat(PCSTR text, std::vector<ShapeOffsetCompat> &out) {
    out.clear();
    if (!text || !*text) return false;
    const auto entries = SplitCompat(text, ',');
    for (const auto &entry : entries) {
        if (entry.empty()) continue;
        const auto parts = SplitCompat(entry, '|');
        if (parts.size() != 3) return false;
        ShapeOffsetCompat item{};
        long equal = 0;
        if (!ParseLongCompat(parts[0], item.dx) ||
            !ParseLongCompat(parts[1], item.dy) ||
            !ParseLongCompat(parts[2], equal))
            return false;
        item.equal = equal != 0;
        out.push_back(item);
    }
    return !out.empty();
}

bool SimilarRgbCompat(
    const RgbColorCompat &a, const RgbColorCompat &b, double sim) {
    ColorRuleCompat expected{};
    expected.color = b;
    return MatchOneColorCompat(a, expected, sim);
}

bool MatchShapeAtCompat(
    const ScreenImageCompat &image,
    long x,
    long y,
    const std::vector<ShapeOffsetCompat> &offsets,
    double sim) {
    const auto *base = image.At(x, y);
    if (!base) return false;
    for (const auto &off : offsets) {
        const auto *pixel = image.At(x + off.dx, y + off.dy);
        if (!pixel) return false;
        const bool equal = SimilarRgbCompat(*pixel, *base, sim);
        if (equal != off.equal) return false;
    }
    return true;
}


std::filesystem::path ResolveObjectFilePathCompat(DmImpl *p, PCSTR file) {
    if (!file || !*file) return {};
    std::filesystem::path path(file);
    if (path.is_absolute()) return path;
    if (p && !p->global_path.empty())
        return std::filesystem::path(p->global_path) / path;
    return std::filesystem::path(ModuleDirectoryCompat(false)) / path;
}

bool WriteBmp24Compat(
    const std::filesystem::path &path,
    const ScreenImageCompat &image) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty())
        return false;

    const DWORD row_bytes =
        static_cast<DWORD>(((static_cast<unsigned long long>(image.width) * 3ULL + 3ULL) / 4ULL) * 4ULL);
    const unsigned long long pixel_bytes =
        static_cast<unsigned long long>(row_bytes) * static_cast<unsigned long long>(image.height);
    if (pixel_bytes > std::numeric_limits<DWORD>::max()) return false;

    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = image.width;
    info_header.biHeight = image.height;
    info_header.biPlanes = 1;
    info_header.biBitCount = 24;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = static_cast<DWORD>(pixel_bytes);

    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    file_header.bfSize = file_header.bfOffBits + info_header.biSizeImage;

    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char *>(&file_header), sizeof(file_header));
    out.write(reinterpret_cast<const char *>(&info_header), sizeof(info_header));

    std::vector<unsigned char> row(row_bytes, 0);
    for (long y = image.height - 1; y >= 0; --y) {
        std::fill(row.begin(), row.end(), 0);
        for (long x = 0; x < image.width; ++x) {
            const auto &c = image.pixels[
                static_cast<size_t>(y) * static_cast<size_t>(image.width) +
                static_cast<size_t>(x)];
            row[static_cast<size_t>(x) * 3 + 0] = c.b;
            row[static_cast<size_t>(x) * 3 + 1] = c.g;
            row[static_cast<size_t>(x) * 3 + 2] = c.r;
        }
        out.write(reinterpret_cast<const char *>(row.data()), row.size());
    }
    return out.good();
}

std::vector<long> BuildColorIntegralCompat(
    const ScreenImageCompat &image,
    const ColorSpecCompat &spec,
    double sim) {
    const long w = image.width;
    const long h = image.height;
    std::vector<long> integral(
        static_cast<size_t>(w + 1) * static_cast<size_t>(h + 1), 0);

    for (long y = 0; y < h; ++y) {
        long row_sum = 0;
        for (long x = 0; x < w; ++x) {
            if (MatchColorSpecCompat(
                    image.pixels[
                        static_cast<size_t>(y) * static_cast<size_t>(w) +
                        static_cast<size_t>(x)],
                    spec, sim))
                ++row_sum;
            integral[
                static_cast<size_t>(y + 1) * static_cast<size_t>(w + 1) +
                static_cast<size_t>(x + 1)] =
                integral[
                    static_cast<size_t>(y) * static_cast<size_t>(w + 1) +
                    static_cast<size_t>(x + 1)] +
                row_sum;
        }
    }
    return integral;
}

long IntegralRectCountCompat(
    const std::vector<long> &integral,
    long stride,
    long x,
    long y,
    long width,
    long height) {
    const long x2 = x + width;
    const long y2 = y + height;
    const auto at = [&](long px, long py) -> long {
        return integral[
            static_cast<size_t>(py) * static_cast<size_t>(stride) +
            static_cast<size_t>(px)];
    };
    return at(x2, y2) - at(x, y2) - at(x2, y) + at(x, y);
}


bool EnableShutdownPrivilegeCompat() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(
            ::GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &token))
        return false;

    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (!::LookupPrivilegeValueA(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) {
        ::CloseHandle(token);
        return false;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    ::SetLastError(ERROR_SUCCESS);
    const BOOL ok = ::AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    const DWORD err = ::GetLastError();
    ::CloseHandle(token);
    return ok && err == ERROR_SUCCESS;
}

std::string ResolveObjectPathCompat(DmImpl *p, PCSTR path) {
    if (!path || !*path) return {};
    std::filesystem::path candidate(path);
    if (candidate.is_relative() && p && !p->global_path.empty())
        candidate = std::filesystem::path(p->global_path) / candidate;
    std::error_code ec;
    candidate = std::filesystem::absolute(candidate, ec);
    if (ec) return {};
    return candidate.lexically_normal().string();
}

long DownloadFileCompat(PCSTR url, PCSTR save_file, long timeout) {
    if (!url || !*url || !save_file || !*save_file || timeout < 0) return -1;

    std::string normalized(url);
    if (normalized.find("://") == std::string::npos)
        normalized = "http://" + normalized;

    HINTERNET internet = ::InternetOpenA(
        "hcbyj64", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!internet) return -1;

    if (timeout > 0) {
        DWORD t = static_cast<DWORD>(timeout);
        ::InternetSetOptionA(internet, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
        ::InternetSetOptionA(internet, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof(t));
        ::InternetSetOptionA(internet, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    }

    HINTERNET request = ::InternetOpenUrlA(
        internet,
        normalized.c_str(),
        nullptr,
        0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
            INTERNET_FLAG_KEEP_CONNECTION,
        0);
    if (!request) {
        ::InternetCloseHandle(internet);
        return -1;
    }

    HANDLE file = ::CreateFileA(
        save_file, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        ::InternetCloseHandle(request);
        ::InternetCloseHandle(internet);
        return -2;
    }

    long result = 1;
    std::vector<unsigned char> buffer(64 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!::InternetReadFile(
                request, buffer.data(),
                static_cast<DWORD>(buffer.size()), &got)) {
            result = -1;
            break;
        }
        if (got == 0) break;

        DWORD written_total = 0;
        while (written_total < got) {
            DWORD written = 0;
            if (!::WriteFile(
                    file, buffer.data() + written_total,
                    got - written_total, &written, nullptr) ||
                written == 0) {
                result = -2;
                break;
            }
            written_total += written;
        }
        if (result != 1) break;
    }

    ::CloseHandle(file);
    ::InternetCloseHandle(request);
    ::InternetCloseHandle(internet);

    if (result != 1) ::DeleteFileA(save_file);
    return result;
}

std::string ExecuteCmdCompat(PCSTR cmd, PCSTR current_dir, long timeout) {
    if (!cmd || !*cmd || timeout < 0) return {};

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        return {};
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::string command = "cmd.exe /D /S /C \"" + std::string(cmd) + "\"";
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    const BOOL created = ::CreateProcessA(
        nullptr,
        mutable_command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        (current_dir && *current_dir) ? current_dir : nullptr,
        &si,
        &pi);

    ::CloseHandle(write_pipe);
    if (!created) {
        ::CloseHandle(read_pipe);
        return {};
    }

    std::string output;
    const ULONGLONG start = ::GetTickCount64();
    bool terminated = false;

    for (;;) {
        DWORD available = 0;
        if (::PeekNamedPipe(read_pipe, nullptr, 0, nullptr, &available, nullptr) &&
            available > 0) {
            std::vector<char> chunk(std::min<DWORD>(available, 64 * 1024));
            DWORD got = 0;
            if (::ReadFile(
                    read_pipe, chunk.data(),
                    static_cast<DWORD>(chunk.size()), &got, nullptr) &&
                got > 0) {
                output.append(chunk.data(), got);
            }
        }

        const DWORD wait = ::WaitForSingleObject(pi.hProcess, 10);
        if (wait == WAIT_OBJECT_0) break;

        if (timeout > 0 &&
            (::GetTickCount64() - start) >= static_cast<ULONGLONG>(timeout)) {
            ::TerminateProcess(pi.hProcess, 1);
            ::WaitForSingleObject(pi.hProcess, 1000);
            terminated = true;
            break;
        }
    }

    for (;;) {
        char chunk[4096];
        DWORD got = 0;
        if (!::ReadFile(read_pipe, chunk, sizeof(chunk), &got, nullptr) || got == 0)
            break;
        output.append(chunk, got);
    }

    ::CloseHandle(read_pipe);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    (void)terminated;
    return output;
}


bool IntelVtEnabledCompat() {
    int cpu[4]{};
    __cpuid(cpu, 0);
    char vendor[13]{};
    std::memcpy(vendor + 0, &cpu[1], 4);
    std::memcpy(vendor + 4, &cpu[3], 4);
    std::memcpy(vendor + 8, &cpu[2], 4);
    if (std::strcmp(vendor, "GenuineIntel") != 0) return false;

    __cpuid(cpu, 1);
    if ((cpu[2] & (1 << 5)) == 0) return false;

#ifndef PF_VIRT_FIRMWARE_ENABLED
#define PF_VIRT_FIRMWARE_ENABLED 21
#endif
    return ::IsProcessorFeaturePresent(PF_VIRT_FIRMWARE_ENABLED) != FALSE;
}


struct PicRefCompat {
    std::string display;
    std::filesystem::path path;
};

struct PicDeltaCompat {
    unsigned char r = 0;
    unsigned char g = 0;
    unsigned char b = 0;
};

bool ParsePicDeltaCompat(PCSTR text, PicDeltaCompat &out) {
    out = {};
    if (!text || !*text) return true;
    const std::string s(text);
    if (s.size() == 2) {
        unsigned char v = 0;
        if (!ParseHexByteCompat(s, 0, v)) return false;
        out.r = out.g = out.b = v;
        return true;
    }
    if (s.size() != 6) return false;
    return ParseHexByteCompat(s, 0, out.r) &&
           ParseHexByteCompat(s, 2, out.g) &&
           ParseHexByteCompat(s, 4, out.b);
}

bool HasWildcardCompat(const std::string &s) {
    return s.find('*') != std::string::npos ||
           s.find('?') != std::string::npos;
}

std::string LowerPathKeyCompat(const std::filesystem::path &path) {
    std::error_code ec;
    auto p = std::filesystem::absolute(path, ec);
    if (ec) p = path;
    std::string key = p.lexically_normal().string();
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return key;
}

std::vector<PicRefCompat> ExpandPicRefsCompat(DmImpl *p, PCSTR pic_name) {
    std::vector<PicRefCompat> out;
    if (!pic_name || !*pic_name) return out;

    for (const auto &token : SplitCompat(pic_name, '|')) {
        if (token.empty()) continue;

        std::filesystem::path raw(token);
        std::filesystem::path full =
            raw.is_absolute() ? raw : ResolveObjectFilePathCompat(p, token.c_str());

        if (!HasWildcardCompat(token)) {
            std::string memory_key = raw.filename().string();
            std::transform(
                memory_key.begin(), memory_key.end(), memory_key.begin(),
                [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            bool in_memory = false;
            if (p) {
                std::lock_guard<std::mutex> lock(p->state_mutex);
                in_memory = p->memory_pic_cache.count(memory_key) != 0;
            }
            if (in_memory) {
                out.push_back({token, raw.filename()});
                continue;
            }

            std::error_code ec;
            if (std::filesystem::is_regular_file(full, ec) && !ec)
                out.push_back({token, full});
            continue;
        }

        const std::filesystem::path dir =
            full.has_parent_path() ? full.parent_path() : std::filesystem::path(".");
        const std::string pattern = full.filename().string();
        const std::filesystem::path query = dir / pattern;

        WIN32_FIND_DATAA fd{};
        HANDLE h = ::FindFirstFileA(query.string().c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::filesystem::path found = dir / fd.cFileName;
            out.push_back({fd.cFileName, found});
        } while (::FindNextFileA(h, &fd));
        ::FindClose(h);
    }
    return out;
}

bool LoadBmp24Compat(
    const std::filesystem::path &path,
    ScreenImageCompat &out) {
    out = {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    in.read(reinterpret_cast<char *>(&fh), sizeof(fh));
    in.read(reinterpret_cast<char *>(&ih), sizeof(ih));
    if (!in || fh.bfType != 0x4D42 ||
        ih.biSize < sizeof(BITMAPINFOHEADER) ||
        ih.biPlanes != 1 ||
        ih.biBitCount != 24 ||
        ih.biCompression != BI_RGB ||
        ih.biWidth <= 0 ||
        ih.biHeight == 0)
        return false;

    const long width = ih.biWidth;
    const long height = ih.biHeight < 0 ? -ih.biHeight : ih.biHeight;
    if (width <= 0 || height <= 0 ||
        static_cast<unsigned long long>(width) *
            static_cast<unsigned long long>(height) >
        256ULL * 1024ULL * 1024ULL)
        return false;

    const size_t row_bytes =
        ((static_cast<size_t>(width) * 3u + 3u) / 4u) * 4u;
    std::vector<unsigned char> row(row_bytes);

    out.x = 0;
    out.y = 0;
    out.width = width;
    out.height = height;
    out.pixels.resize(
        static_cast<size_t>(width) * static_cast<size_t>(height));

    in.seekg(static_cast<std::streamoff>(fh.bfOffBits), std::ios::beg);
    if (!in) return false;

    const bool top_down = ih.biHeight < 0;
    for (long file_y = 0; file_y < height; ++file_y) {
        in.read(reinterpret_cast<char *>(row.data()), row.size());
        if (!in) return false;
        const long y = top_down ? file_y : (height - 1 - file_y);
        for (long x = 0; x < width; ++x) {
            auto &dst = out.pixels[
                static_cast<size_t>(y) * static_cast<size_t>(width) +
                static_cast<size_t>(x)];
            dst.b = row[static_cast<size_t>(x) * 3 + 0];
            dst.g = row[static_cast<size_t>(x) * 3 + 1];
            dst.r = row[static_cast<size_t>(x) * 3 + 2];
        }
    }
    return true;
}

std::shared_ptr<ScreenImageCompat> LoadPicCachedCompat(
    DmImpl *p, const std::filesystem::path &path) {
    if (!p) return {};

    std::string memory_key = path.filename().string();
    std::transform(
        memory_key.begin(), memory_key.end(), memory_key.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        const auto memory_it = p->memory_pic_cache.find(memory_key);
        if (memory_it != p->memory_pic_cache.end())
            return memory_it->second;
    }

    const std::string key = LowerPathKeyCompat(path);

    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        if (p->pic_cache_enabled) {
            const auto it = p->pic_cache.find(key);
            if (it != p->pic_cache.end()) return it->second;
        }
    }

    auto image = std::make_shared<ScreenImageCompat>();
    if (!LoadBmp24Compat(path, *image)) return {};

    if (p->pic_cache_enabled) {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        p->pic_cache[key] = image;
    }
    return image;
}

bool SameRgbCompat(
    const RgbColorCompat &a, const RgbColorCompat &b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool PicTransparentColorCompat(
    const ScreenImageCompat &pic, RgbColorCompat &transparent) {
    if (pic.width <= 0 || pic.height <= 0 || pic.pixels.empty()) return false;
    const auto &a = pic.pixels.front();
    const auto &b = pic.pixels[static_cast<size_t>(pic.width - 1)];
    const auto &c = pic.pixels[
        static_cast<size_t>(pic.height - 1) * static_cast<size_t>(pic.width)];
    const auto &d = pic.pixels.back();
    if (!SameRgbCompat(a, b) || !SameRgbCompat(a, c) || !SameRgbCompat(a, d))
        return false;
    transparent = a;
    return true;
}

bool PicPixelWithinDeltaCompat(
    const RgbColorCompat &actual,
    const RgbColorCompat &expected,
    const PicDeltaCompat &delta) {
    return std::abs(static_cast<int>(actual.r) - static_cast<int>(expected.r)) <= delta.r &&
           std::abs(static_cast<int>(actual.g) - static_cast<int>(expected.g)) <= delta.g &&
           std::abs(static_cast<int>(actual.b) - static_cast<int>(expected.b)) <= delta.b;
}

int PicMatchPercentCompat(
    const ScreenImageCompat &screen,
    long x,
    long y,
    const ScreenImageCompat &pic,
    const PicDeltaCompat &delta) {
    if (pic.width <= 0 || pic.height <= 0) return -1;
    if (x < screen.x || y < screen.y ||
        x + pic.width - 1 >= screen.x + screen.width ||
        y + pic.height - 1 >= screen.y + screen.height)
        return -1;

    RgbColorCompat transparent{};
    const bool has_transparent =
        PicTransparentColorCompat(pic, transparent);

    unsigned long long matched = 0;
    const unsigned long long total =
        static_cast<unsigned long long>(pic.width) *
        static_cast<unsigned long long>(pic.height);
    if (!total) return -1;

    for (long py = 0; py < pic.height; ++py) {
        for (long px = 0; px < pic.width; ++px) {
            const auto &expected = pic.pixels[
                static_cast<size_t>(py) * static_cast<size_t>(pic.width) +
                static_cast<size_t>(px)];
            if (has_transparent && SameRgbCompat(expected, transparent)) {
                ++matched;
                continue;
            }
            const auto *actual = screen.At(x + px, y + py);
            if (actual && PicPixelWithinDeltaCompat(*actual, expected, delta))
                ++matched;
        }
    }
    return static_cast<int>((matched * 100ULL) / total);
}

struct PicSearchResultCompat {
    long index = -1;
    long x = -1;
    long y = -1;
    long score = -1;
    std::string display;
};

bool FindOnePicCompat(
    const ScreenImageCompat &screen,
    const ScreenImageCompat &pic,
    const PicDeltaCompat &delta,
    long x1, long y1, long x2, long y2,
    long dir, long minimum_percent,
    long &out_x, long &out_y, long &out_score) {
    out_x = -1;
    out_y = -1;
    out_score = -1;
    const long max_x = x2 - pic.width + 1;
    const long max_y = y2 - pic.height + 1;
    if (max_x < x1 || max_y < y1) return false;

    bool found = false;
    ForEachPointInDirectionCompat(
        x1, y1, max_x, max_y, dir,
        [&](long x, long y) {
            const int score =
                PicMatchPercentCompat(screen, x, y, pic, delta);
            if (score >= minimum_percent) {
                out_x = x;
                out_y = y;
                out_score = score;
                found = true;
                return true;
            }
            return false;
        });
    return found;
}

std::vector<PicSearchResultCompat> FindPicsAllCompat(
    DmImpl *p,
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    long minimum_percent, long dir,
    size_t limit) {
    std::vector<PicSearchResultCompat> out;
    if (!p || x2 < x1 || y2 < y1 || minimum_percent < 0 || minimum_percent > 100)
        return out;

    PicDeltaCompat delta{};
    if (!ParsePicDeltaCompat(delta_color, delta)) return out;

    const auto refs = ExpandPicRefsCompat(p, pic_name);
    if (refs.empty()) return out;

    ScreenImageCompat screen;
    if (!CaptureScreenRegionForObjectCompat(p, x1, y1, x2, y2, screen)) return out;

    struct Loaded {
        PicRefCompat ref;
        std::shared_ptr<ScreenImageCompat> image;
        long index = -1;
    };
    std::vector<Loaded> loaded;
    for (size_t i = 0; i < refs.size(); ++i) {
        auto image = LoadPicCachedCompat(p, refs[i].path);
        if (image)
            loaded.push_back({refs[i], std::move(image), static_cast<long>(i)});
    }

    if (loaded.empty()) return out;

    long max_w = 0, max_h = 0;
    for (const auto &item : loaded) {
        max_w = std::max(max_w, item.image->width);
        max_h = std::max(max_h, item.image->height);
    }

    ForEachPointInDirectionCompat(
        x1, y1, x2, y2, dir,
        [&](long x, long y) {
            for (const auto &item : loaded) {
                if (x + item.image->width - 1 > x2 ||
                    y + item.image->height - 1 > y2)
                    continue;
                const int score =
                    PicMatchPercentCompat(screen, x, y, *item.image, delta);
                if (score < minimum_percent) continue;
                out.push_back({
                    item.index, x, y, score, item.ref.display
                });
                if (out.size() >= limit) return true;
            }
            return false;
        });
    return out;
}

std::optional<PicSearchResultCompat> FindPicFirstCompat(
    DmImpl *p,
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    long minimum_percent, long dir) {
    const auto all = FindPicsAllCompat(
        p, x1, y1, x2, y2, pic_name, delta_color,
        minimum_percent, dir, 1);
    if (all.empty()) return std::nullopt;
    return all.front();
}


class GdiplusSessionCompat {
public:
    GdiplusSessionCompat() {
        Gdiplus::GdiplusStartupInput input;
        ok_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
    }
    ~GdiplusSessionCompat() {
        if (ok_) Gdiplus::GdiplusShutdown(token_);
    }
    bool ok() const { return ok_; }

private:
    ULONG_PTR token_ = 0;
    bool ok_ = false;
};

GdiplusSessionCompat &GdiplusSessionInstanceCompat() {
    static GdiplusSessionCompat session;
    return session;
}

bool GetImageEncoderClsidCompat(
    const WCHAR *mime,
    CLSID &clsid) {
    UINT count = 0;
    UINT bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&count, &bytes) != Gdiplus::Ok ||
        count == 0 || bytes == 0)
        return false;

    std::vector<unsigned char> buffer(bytes);
    auto *encoders =
        reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok)
        return false;

    for (UINT i = 0; i < count; ++i) {
        if (encoders[i].MimeType &&
            _wcsicmp(encoders[i].MimeType, mime) == 0) {
            clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

std::unique_ptr<Gdiplus::Bitmap> ScreenImageToBitmapCompat(
    const ScreenImageCompat &image) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty())
        return {};

    auto bitmap = std::make_unique<Gdiplus::Bitmap>(
        image.width, image.height, PixelFormat24bppRGB);
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) return {};

    Gdiplus::Rect rect(0, 0, image.width, image.height);
    Gdiplus::BitmapData data{};
    if (bitmap->LockBits(
            &rect,
            Gdiplus::ImageLockModeWrite,
            PixelFormat24bppRGB,
            &data) != Gdiplus::Ok)
        return {};

    auto *base = static_cast<unsigned char *>(data.Scan0);
    for (long y = 0; y < image.height; ++y) {
        auto *row =
            base + static_cast<ptrdiff_t>(y) * data.Stride;
        for (long x = 0; x < image.width; ++x) {
            const auto &c = image.pixels[
                static_cast<size_t>(y) *
                    static_cast<size_t>(image.width) +
                static_cast<size_t>(x)];
            row[static_cast<size_t>(x) * 3 + 0] = c.b;
            row[static_cast<size_t>(x) * 3 + 1] = c.g;
            row[static_cast<size_t>(x) * 3 + 2] = c.r;
        }
    }
    bitmap->UnlockBits(&data);
    return bitmap;
}

bool SaveScreenImageEncodedCompat(
    const std::filesystem::path &path,
    const ScreenImageCompat &image,
    const WCHAR *mime,
    long quality) {
    if (!GdiplusSessionInstanceCompat().ok()) return false;

    auto bitmap = ScreenImageToBitmapCompat(image);
    if (!bitmap) return false;

    CLSID encoder{};
    if (!GetImageEncoderClsidCompat(mime, encoder)) return false;

    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);

    Gdiplus::EncoderParameters params{};
    Gdiplus::EncoderParameters *param_ptr = nullptr;
    if (_wcsicmp(mime, L"image/jpeg") == 0) {
        quality = std::clamp<long>(quality, 1, 100);
        params.Count = 1;
        params.Parameter[0].Guid = Gdiplus::EncoderQuality;
        params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        ULONG q = static_cast<ULONG>(quality);
        params.Parameter[0].Value = &q;

        std::wstring wide = path.wstring();
        return bitmap->Save(
                   wide.c_str(), &encoder, &params) == Gdiplus::Ok;
    }

    std::wstring wide = path.wstring();
    return bitmap->Save(
               wide.c_str(), &encoder, param_ptr) == Gdiplus::Ok;
}

bool ConvertImageToBmp24Compat(
    const std::filesystem::path &source_path,
    const std::filesystem::path &dest_path) {
    if (!GdiplusSessionInstanceCompat().ok()) return false;

    const std::wstring src = source_path.wstring();
    Gdiplus::Bitmap source(src.c_str(), FALSE);
    if (source.GetLastStatus() != Gdiplus::Ok ||
        source.GetWidth() == 0 || source.GetHeight() == 0)
        return false;

    ScreenImageCompat converted{};
    converted.width = static_cast<long>(source.GetWidth());
    converted.height = static_cast<long>(source.GetHeight());
    converted.x = converted.y = 0;
    converted.pixels.resize(
        static_cast<size_t>(converted.width) *
        static_cast<size_t>(converted.height));

    Gdiplus::Rect rect(
        0, 0,
        static_cast<INT>(source.GetWidth()),
        static_cast<INT>(source.GetHeight()));
    Gdiplus::BitmapData data{};
    if (source.LockBits(
            &rect,
            Gdiplus::ImageLockModeRead,
            PixelFormat24bppRGB,
            &data) != Gdiplus::Ok)
        return false;

    const auto *base =
        static_cast<const unsigned char *>(data.Scan0);
    for (long y = 0; y < converted.height; ++y) {
        const auto *row =
            base + static_cast<ptrdiff_t>(y) * data.Stride;
        for (long x = 0; x < converted.width; ++x) {
            auto &c = converted.pixels[
                static_cast<size_t>(y) *
                    static_cast<size_t>(converted.width) +
                static_cast<size_t>(x)];
            c.b = row[static_cast<size_t>(x) * 3 + 0];
            c.g = row[static_cast<size_t>(x) * 3 + 1];
            c.r = row[static_cast<size_t>(x) * 3 + 2];
        }
    }
    source.UnlockBits(&data);
    return WriteBmp24Compat(dest_path, converted);
}


std::string WideStringToAcpCompat(const wchar_t *text, int chars) {
    if (!text || chars <= 0) return {};
    const int n = ::WideCharToMultiByte(
        CP_ACP, 0, text, chars, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    if (::WideCharToMultiByte(
            CP_ACP, 0, text, chars, out.data(), n, nullptr, nullptr) <= 0)
        return {};
    return out;
}

std::string CurrentProcessDirectoryCompat() {
    std::vector<char> buf(32768, 0);
    const DWORD n = ::GetModuleFileNameA(
        nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) return {};
    std::filesystem::path p(std::string(buf.data(), n));
    return p.parent_path().string();
}

std::string QueryProcessCommandLineCompat(DWORD pid) {
    HANDLE process = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process)
        process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) return {};

    using NtQueryInformationProcessFn =
        LONG (NTAPI *)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<NtQueryInformationProcessFn>(
        ::GetProcAddress(ntdll, "NtQueryInformationProcess")) : nullptr;
    if (!fn) {
        ::CloseHandle(process);
        return {};
    }

    constexpr PROCESSINFOCLASS kProcessCommandLineInformation =
        static_cast<PROCESSINFOCLASS>(60);
    ULONG needed = 0;
    LONG status = fn(
        process, kProcessCommandLineInformation, nullptr, 0, &needed);

    if (needed == 0 || needed > 16 * 1024 * 1024) {
        ::CloseHandle(process);
        return {};
    }

    std::vector<unsigned char> buf(static_cast<size_t>(needed) + sizeof(wchar_t) * 2, 0);
    status = fn(
        process, kProcessCommandLineInformation,
        buf.data(), static_cast<ULONG>(buf.size()), &needed);
    ::CloseHandle(process);

    if (status < 0 || buf.size() < sizeof(UNICODE_STRING)) return {};

    const auto *us = reinterpret_cast<const UNICODE_STRING *>(buf.data());
    if (!us->Buffer || us->Length == 0) return {};

    const auto begin = reinterpret_cast<ULONG_PTR>(buf.data());
    const auto end = begin + buf.size();
    const auto ptr = reinterpret_cast<ULONG_PTR>(us->Buffer);

    // ProcessCommandLineInformation normally returns the string inline in
    // the caller-owned buffer. Validate before dereferencing.
    if (ptr < begin || ptr + us->Length > end) return {};

    return WideStringToAcpCompat(
        us->Buffer, static_cast<int>(us->Length / sizeof(wchar_t)));
}



bool LoadBmp24MemoryCompat(
    const unsigned char *data, size_t size, ScreenImageCompat &out) {
    out = {};
    if (!data ||
        size < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER))
        return false;

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    std::memcpy(&fh, data, sizeof(fh));
    std::memcpy(&ih, data + sizeof(fh), sizeof(ih));

    if (fh.bfType != 0x4D42 ||
        ih.biSize < sizeof(BITMAPINFOHEADER) ||
        ih.biPlanes != 1 ||
        ih.biBitCount != 24 ||
        ih.biCompression != BI_RGB ||
        ih.biWidth <= 0 ||
        ih.biHeight == 0)
        return false;

    const long width = ih.biWidth;
    const long height = ih.biHeight < 0 ? -ih.biHeight : ih.biHeight;
    if (width <= 0 || height <= 0 ||
        static_cast<unsigned long long>(width) *
            static_cast<unsigned long long>(height) >
            256ULL * 1024ULL * 1024ULL)
        return false;

    const size_t row_bytes =
        ((static_cast<size_t>(width) * 3u + 3u) / 4u) * 4u;
    const unsigned long long needed =
        static_cast<unsigned long long>(fh.bfOffBits) +
        static_cast<unsigned long long>(row_bytes) *
            static_cast<unsigned long long>(height);
    if (fh.bfOffBits < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) ||
        needed > size)
        return false;

    out.x = 0;
    out.y = 0;
    out.width = width;
    out.height = height;
    out.pixels.resize(
        static_cast<size_t>(width) * static_cast<size_t>(height));

    const bool top_down = ih.biHeight < 0;
    const auto *pixels = data + fh.bfOffBits;
    for (long file_y = 0; file_y < height; ++file_y) {
        const long y = top_down ? file_y : (height - 1 - file_y);
        const auto *row =
            pixels + static_cast<size_t>(file_y) * row_bytes;
        for (long x = 0; x < width; ++x) {
            auto &dst = out.pixels[
                static_cast<size_t>(y) * static_cast<size_t>(width) +
                static_cast<size_t>(x)];
            dst.b = row[static_cast<size_t>(x) * 3 + 0];
            dst.g = row[static_cast<size_t>(x) * 3 + 1];
            dst.r = row[static_cast<size_t>(x) * 3 + 2];
        }
    }
    return true;
}

bool CopyLegacyBytesCompat(
    long address, long size, std::vector<unsigned char> &out) {
    out.clear();
    if (address == 0 || size <= 0 || size > 256 * 1024 * 1024)
        return false;
    out.resize(static_cast<size_t>(size));
    SIZE_T got = 0;
    const ULONG_PTR ptr =
        static_cast<ULONG_PTR>(static_cast<unsigned long>(address));
    if (!::ReadProcessMemory(
            ::GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(ptr),
            out.data(), out.size(), &got) ||
        got != out.size()) {
        out.clear();
        return false;
    }
    return true;
}

struct MemoryPicCompat {
    long address = 0;
    long size = 0;
    long index = -1;
    std::shared_ptr<ScreenImageCompat> image;
};

bool ParseMemoryPicInfoCompat(
    PCSTR pic_info, std::vector<MemoryPicCompat> &out) {
    out.clear();
    if (!pic_info || !*pic_info) return false;

    const auto entries = SplitCompat(pic_info, '|');
    long index = 0;
    for (const auto &entry : entries) {
        if (entry.empty()) continue;
        const auto parts = SplitCompat(entry, ',');
        if (parts.size() != 2) return false;

        char *end1 = nullptr;
        char *end2 = nullptr;
        const unsigned long long addr64 =
            std::strtoull(parts[0].c_str(), &end1, 10);
        const long long size64 =
            std::strtoll(parts[1].c_str(), &end2, 10);
        if (!end1 || *end1 != '\0' ||
            !end2 || *end2 != '\0' ||
            addr64 == 0 ||
            addr64 > static_cast<unsigned long long>(
                std::numeric_limits<unsigned long>::max()) ||
            size64 <= 0 || size64 > 256LL * 1024LL * 1024LL)
            return false;

        std::vector<unsigned char> bytes;
        const long address =
            static_cast<long>(static_cast<unsigned long>(addr64));
        const long byte_count = static_cast<long>(size64);
        if (!CopyLegacyBytesCompat(address, byte_count, bytes))
            return false;

        auto image = std::make_shared<ScreenImageCompat>();
        if (!LoadBmp24MemoryCompat(bytes.data(), bytes.size(), *image))
            return false;

        out.push_back({address, byte_count, index++, std::move(image)});
    }
    return !out.empty();
}

std::vector<PicSearchResultCompat> FindMemoryPicsAllCompat(
    DmImpl *p,
    long x1, long y1, long x2, long y2,
    PCSTR pic_info, PCSTR delta_color,
    long minimum_percent, long dir,
    size_t limit) {
    std::vector<PicSearchResultCompat> out;
    if (!p || x2 < x1 || y2 < y1 ||
        minimum_percent < 0 || minimum_percent > 100 ||
        limit == 0)
        return out;

    PicDeltaCompat delta{};
    if (!ParsePicDeltaCompat(delta_color, delta)) return out;

    std::vector<MemoryPicCompat> pics;
    if (!ParseMemoryPicInfoCompat(pic_info, pics)) return out;

    ScreenImageCompat screen;
    if (!CaptureScreenRegionForObjectCompat(
            p, x1, y1, x2, y2, screen))
        return out;

    ForEachPointInDirectionCompat(
        x1, y1, x2, y2, dir,
        [&](long x, long y) {
            for (const auto &pic : pics) {
                if (!pic.image) continue;
                if (x + pic.image->width - 1 > x2 ||
                    y + pic.image->height - 1 > y2)
                    continue;
                const int score = PicMatchPercentCompat(
                    screen, x, y, *pic.image, delta);
                if (score < minimum_percent) continue;
                out.push_back({
                    pic.index, x, y, score,
                    std::to_string(pic.index)
                });
                if (out.size() >= limit) return true;
            }
            return false;
        });
    return out;
}

bool ScreenImageToBmpBytesCompat(
    const ScreenImageCompat &image,
    std::vector<unsigned char> &out) {
    out.clear();
    if (image.width <= 0 || image.height <= 0 ||
        image.pixels.empty())
        return false;

    const size_t row_bytes =
        ((static_cast<size_t>(image.width) * 3u + 3u) / 4u) * 4u;
    const unsigned long long pixel_bytes =
        static_cast<unsigned long long>(row_bytes) *
        static_cast<unsigned long long>(image.height);
    const unsigned long long total =
        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) +
        pixel_bytes;
    if (total > static_cast<unsigned long long>(
                    std::numeric_limits<DWORD>::max()))
        return false;

    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits =
        sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = static_cast<DWORD>(total);

    ih.biSize = sizeof(ih);
    ih.biWidth = image.width;
    ih.biHeight = image.height;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = static_cast<DWORD>(pixel_bytes);

    out.resize(static_cast<size_t>(total), 0);
    std::memcpy(out.data(), &fh, sizeof(fh));
    std::memcpy(
        out.data() + sizeof(fh), &ih, sizeof(ih));

    auto *dst = out.data() + fh.bfOffBits;
    for (long file_y = 0; file_y < image.height; ++file_y) {
        const long y = image.height - 1 - file_y;
        auto *row =
            dst + static_cast<size_t>(file_y) * row_bytes;
        for (long x = 0; x < image.width; ++x) {
            const auto &c = image.pixels[
                static_cast<size_t>(y) *
                    static_cast<size_t>(image.width) +
                static_cast<size_t>(x)];
            row[static_cast<size_t>(x) * 3 + 0] = c.b;
            row[static_cast<size_t>(x) * 3 + 1] = c.g;
            row[static_cast<size_t>(x) * 3 + 2] = c.r;
        }
    }
    return true;
}

void FreeLegacyBinBufferCompat(DmImpl *p) {
    if (!p || !p->legacy_bin_buffer) return;
    ::VirtualFree(p->legacy_bin_buffer, 0, MEM_RELEASE);
    p->legacy_bin_buffer = nullptr;
    p->legacy_bin_size = 0;
}

void *AllocateLegacyBinBufferCompat(DmImpl *p, SIZE_T size) {
    if (!p || size == 0) return nullptr;
    FreeLegacyBinBufferCompat(p);

#if defined(_WIN64)
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const ULONG_PTR gran =
        static_cast<ULONG_PTR>(si.dwAllocationGranularity);
    const ULONG_PTR limit = 0x7FFF0000ULL;

    MEMORY_BASIC_INFORMATION mbi{};
    ULONG_PTR cursor = 0x00010000ULL;
    while (cursor < limit) {
        if (!::VirtualQuery(
                reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)))
            break;

        const ULONG_PTR base =
            reinterpret_cast<ULONG_PTR>(mbi.BaseAddress);
        const ULONG_PTR region_size =
            static_cast<ULONG_PTR>(mbi.RegionSize);
        const ULONG_PTR region_end =
            base <= std::numeric_limits<ULONG_PTR>::max() - region_size
                ? base + region_size
                : std::numeric_limits<ULONG_PTR>::max();

        if (mbi.State == MEM_FREE) {
            ULONG_PTR candidate =
                (base + gran - 1) & ~(gran - 1);
            if (candidate < limit &&
                size <= limit - candidate &&
                candidate + size <= region_end) {
                void *mem = ::VirtualAlloc(
                    reinterpret_cast<LPVOID>(candidate),
                    size,
                    MEM_RESERVE | MEM_COMMIT,
                    PAGE_READWRITE);
                if (mem &&
                    reinterpret_cast<ULONG_PTR>(mem) <=
                        static_cast<ULONG_PTR>(LONG_MAX)) {
                    p->legacy_bin_buffer = mem;
                    p->legacy_bin_size = size;
                    return mem;
                }
                if (mem) ::VirtualFree(mem, 0, MEM_RELEASE);
            }
        }

        if (region_end <= cursor) break;
        cursor = region_end;
    }
    return nullptr;
#else
    void *mem = ::VirtualAlloc(
        nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return nullptr;
    p->legacy_bin_buffer = mem;
    p->legacy_bin_size = size;
    return mem;
#endif
}

bool CopyFromLegacyPointerCompat(long data, void *dst, SIZE_T size) {
    if (!dst || size == 0 || data == 0) return false;
    const ULONG_PTR address =
        static_cast<ULONG_PTR>(
            static_cast<unsigned long>(data));
    SIZE_T got = 0;
    return ::ReadProcessMemory(
               ::GetCurrentProcess(),
               reinterpret_cast<LPCVOID>(address),
               dst, size, &got) &&
           got == size;
}


bool FindKeyboardLayoutByTextCompat(
    PCSTR layout_text, std::string &klid_out) {
    klid_out.clear();
    if (!layout_text || !*layout_text) return false;

    HKEY root = nullptr;
    if (::RegOpenKeyExA(
            HKEY_LOCAL_MACHINE,
            "SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts",
            0, KEY_READ, &root) != ERROR_SUCCESS)
        return false;

    bool found = false;
    DWORD index = 0;
    for (;;) {
        char subkey[256]{};
        DWORD subkey_len = static_cast<DWORD>(sizeof(subkey));
        FILETIME ft{};
        const LSTATUS enum_status = ::RegEnumKeyExA(
            root, index++, subkey, &subkey_len,
            nullptr, nullptr, nullptr, &ft);
        if (enum_status == ERROR_NO_MORE_ITEMS) break;
        if (enum_status != ERROR_SUCCESS) continue;

        HKEY key = nullptr;
        if (::RegOpenKeyExA(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS)
            continue;

        char text[512]{};
        DWORD type = 0;
        DWORD bytes = sizeof(text);
        const LSTATUS get_status = ::RegQueryValueExA(
            key, "Layout Text", nullptr, &type,
            reinterpret_cast<BYTE *>(text), &bytes);
        ::RegCloseKey(key);

        if (get_status != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ))
            continue;

        text[sizeof(text)-1] = '\0';
        if (_stricmp(text, layout_text) == 0) {
            klid_out.assign(subkey, subkey_len);
            found = true;
            break;
        }
    }

    ::RegCloseKey(root);
    return found;
}

std::string HklToKlidCompat(HKL hkl) {
    char buf[16]{};
    const unsigned long value =
        static_cast<unsigned long>(
            reinterpret_cast<ULONG_PTR>(hkl) & 0xffffffffULL);
    std::snprintf(buf, sizeof(buf), "%08lX", value);
    return buf;
}

bool ThreadUsesLayoutTextCompat(HWND hwnd, PCSTR layout_text) {
    if (!hwnd || !layout_text || !*layout_text) return false;
    std::string wanted;
    if (!FindKeyboardLayoutByTextCompat(layout_text, wanted))
        return false;

    const DWORD tid = ::GetWindowThreadProcessId(hwnd, nullptr);
    if (!tid) return false;
    const HKL hkl = ::GetKeyboardLayout(tid);
    if (!hkl) return false;

    const std::string current = HklToKlidCompat(hkl);
    if (_stricmp(current.c_str(), wanted.c_str()) == 0)
        return true;

    // Some IMEs expose an HKL with the low word as the language ID while
    // the registry key is a classic 0000LLLL layout. Compare that fallback.
    char lang_key[16]{};
    std::snprintf(
        lang_key, sizeof(lang_key), "0000%04X",
        static_cast<unsigned>(LOWORD(reinterpret_cast<ULONG_PTR>(hkl))));
    return _stricmp(lang_key, wanted.c_str()) == 0;
}

bool ActivateLayoutTextCompat(HWND hwnd, PCSTR layout_text) {
    if (!hwnd || !::IsWindow(hwnd) || !layout_text || !*layout_text)
        return false;

    std::string klid;
    if (!FindKeyboardLayoutByTextCompat(layout_text, klid))
        return false;

    HKL hkl = ::LoadKeyboardLayoutA(
        klid.c_str(), KLF_NOTELLSHELL);
    if (!hkl) return false;

    const LRESULT result = ::SendMessageA(
        hwnd, WM_INPUTLANGCHANGEREQUEST,
        0, reinterpret_cast<LPARAM>(hkl));
    (void)result;

    // The target may process the request asynchronously in some frameworks.
    for (int i = 0; i < 10; ++i) {
        if (ThreadUsesLayoutTextCompat(hwnd, layout_text))
            return true;
        ::Sleep(10);
    }
    return ThreadUsesLayoutTextCompat(hwnd, layout_text);
}

} // namespace

extern "C" HCBYJ64_API BOOL LoadDm(PCSTR path) { return hcbyj64::OpRuntime::Configure(path) ? TRUE : FALSE; }
extern "C" HCBYJ64_API BOOL LoadDmW(PCWSTR path) { return hcbyj64::OpRuntime::ConfigureW(path) ? TRUE : FALSE; }
extern "C" HCBYJ64_API BOOL FreeDm(void) { hcbyj64::OpRuntime::Reset(); return TRUE; }

dmsoft::dmsoft() : impl(new DmImpl()) {
    auto *p = P(impl);
    p->id = g_next_dm_id.fetch_add(1, std::memory_order_relaxed);
    p->global_path = ModuleDirectoryCompat(false);
    g_dm_object_count.fetch_add(1, std::memory_order_relaxed);
}
dmsoft::~dmsoft() {
    if (impl) {
        auto *p = P(impl);
        for (const auto &entry : p->play_aliases) {
            const std::string stop = "stop " + entry.second;
            const std::string close = "close " + entry.second;
            ::mciSendStringA(stop.c_str(), nullptr, 0, nullptr);
            ::mciSendStringA(close.c_str(), nullptr, 0, nullptr);
        }
        p->play_aliases.clear();
        ClearObjectBindingCompat(p);
        FreeLegacyBinBufferCompat(p);
        g_dm_object_count.fetch_sub(1, std::memory_order_relaxed);
        delete p;
        impl = nullptr;
    }
}
bool dmsoft::IsValid() const { return impl != nullptr; }

long dmsoft::ReleaseRef() { return 1; }
long dmsoft::Is64Bit() { return 1; }

long dmsoft::GetLastError() {
    DmImpl *p = P(impl);
    if (!p) return ERROR_INVALID_HANDLE;
    if (p->native_error != 0) return p->native_error;
    return p->op.LastError();
}

long dmsoft::Delay(long mis) {
    if (mis < 0) mis = 0;
    ::Sleep(static_cast<DWORD>(mis));
    return 1;
}

long dmsoft::Delays(long min_s, long max_s) {
    if (min_s > max_s) std::swap(min_s, max_s);
    min_s = std::max<long>(0, min_s);
    max_s = std::max<long>(0, max_s);
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<long> dist(min_s, max_s);
    ::Sleep(static_cast<DWORD>(dist(rng)));
    return 1;
}

long dmsoft::Int64ToInt32(LONGLONG v) { return static_cast<long>(static_cast<std::int32_t>(v)); }

const char *dmsoft::Hex32(long v) {
    auto *p = P(impl); if (!p) return "";
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned int>(v));
    p->scratch = buf; return p->scratch.c_str();
}

const char *dmsoft::Hex64(LONGLONG v) {
    auto *p = P(impl); if (!p) return "";
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
    p->scratch = buf; return p->scratch.c_str();
}

long dmsoft::SetMemoryHwndAsProcessId(long en) {
    auto *p = P(impl); if (!p) return 0;
    p->hwnd_is_pid = (en != 0); p->native_error = 0;
    p->op.InvokeLong("SetMemoryHwndAsProcessId", {A(en)});
    return 1;
}

long dmsoft::GetWindowProcessId(long hwnd) {
    DWORD pid = 0; HWND h = reinterpret_cast<HWND>(static_cast<INT_PTR>(hwnd));
    ::GetWindowThreadProcessId(h, &pid);
    if (!pid) SetNativeError(P(impl), static_cast<long>(::GetLastError())); else SetNativeError(P(impl), 0);
    return static_cast<long>(pid);
}

long dmsoft::GetWindowThreadId(long hwnd) {
    DWORD pid = 0; HWND h = reinterpret_cast<HWND>(static_cast<INT_PTR>(hwnd));
    DWORD tid = ::GetWindowThreadProcessId(h, &pid);
    if (!tid) SetNativeError(P(impl), static_cast<long>(::GetLastError())); else SetNativeError(P(impl), 0);
    return static_cast<long>(tid);
}

const char *dmsoft::ReadDataAddr(long hwnd, LONGLONG addr, long len) {
    auto *p = P(impl); if (!p || len < 0 || len > 64 * 1024 * 1024) return "";
    std::vector<unsigned char> data(static_cast<size_t>(len));
    HANDLE h = OpenTarget(p, hwnd, PROCESS_VM_READ | PROCESS_QUERY_INFORMATION); if (!h) return "";
    SIZE_T got = 0;
    BOOL ok = ::ReadProcessMemory(h, reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(addr)), data.data(), data.size(), &got);
    CloseHandle(h);
    if (!ok || got != data.size()) { SetNativeError(p, static_cast<long>(::GetLastError())); p->scratch.clear(); return p->scratch.c_str(); }
    static const char kHex[] = "0123456789abcdef";
    p->scratch.clear(); if (!data.empty()) p->scratch.reserve(data.size() * 3 - 1);
    for (size_t i=0;i<data.size();++i) { if(i) p->scratch.push_back(' '); p->scratch.push_back(kHex[(data[i]>>4)&0xf]); p->scratch.push_back(kHex[data[i]&0xf]); }
    SetNativeError(p,0); return p->scratch.c_str();
}

long dmsoft::WriteDataAddr(long hwnd, LONGLONG addr, PCSTR data) {
    auto *p=P(impl); if(!p) return 0; std::vector<unsigned char> bytes;
    if(!ParseHexBytes(data,bytes)){SetNativeError(p,ERROR_INVALID_DATA);return 0;}
    HANDLE h=OpenTarget(p,hwnd,PROCESS_VM_WRITE|PROCESS_VM_OPERATION|PROCESS_QUERY_INFORMATION); if(!h) return 0;
    SIZE_T put=0; BOOL ok=::WriteProcessMemory(h,reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(addr)),bytes.data(),bytes.size(),&put); CloseHandle(h);
    if(!ok||put!=bytes.size()){SetNativeError(p,static_cast<long>(::GetLastError()));return 0;} SetNativeError(p,0); return 1;
}

LONGLONG dmsoft::ReadIntAddr(long hwnd, LONGLONG addr, long type) {
    auto *p=P(impl); if(!p) return 0;
    switch(type){
    case 0:{std::int32_t v{};return ReadValue(p,hwnd,addr,v)?v:0;}
    case 1:{std::int16_t v{};return ReadValue(p,hwnd,addr,v)?v:0;}
    case 2:{std::int8_t v{};return ReadValue(p,hwnd,addr,v)?v:0;}
    case 3:{std::int64_t v{};return ReadValue(p,hwnd,addr,v)?v:0;}
    case 4:{std::uint32_t v{};return ReadValue(p,hwnd,addr,v)?v:0;}
    case 5:{std::uint16_t v{};return ReadValue(p,hwnd,addr,v)?v:0;}
    case 6:{std::uint8_t v{};return ReadValue(p,hwnd,addr,v)?v:0;}
    default:SetNativeError(p,ERROR_INVALID_PARAMETER);return 0;}
}

long dmsoft::WriteIntAddr(long hwnd, LONGLONG addr, long type, LONGLONG v) {
    auto *p=P(impl); if(!p)return 0; bool ok=false;
    switch(type){
    case 0:{std::int32_t x=static_cast<std::int32_t>(v);ok=WriteValue(p,hwnd,addr,x);break;}
    case 1:{std::int16_t x=static_cast<std::int16_t>(v);ok=WriteValue(p,hwnd,addr,x);break;}
    case 2:{std::int8_t x=static_cast<std::int8_t>(v);ok=WriteValue(p,hwnd,addr,x);break;}
    case 3:{std::int64_t x=static_cast<std::int64_t>(v);ok=WriteValue(p,hwnd,addr,x);break;}
    case 4:{std::uint32_t x=static_cast<std::uint32_t>(v);ok=WriteValue(p,hwnd,addr,x);break;}
    case 5:{std::uint16_t x=static_cast<std::uint16_t>(v);ok=WriteValue(p,hwnd,addr,x);break;}
    case 6:{std::uint8_t x=static_cast<std::uint8_t>(v);ok=WriteValue(p,hwnd,addr,x);break;}
    default:SetNativeError(p,ERROR_INVALID_PARAMETER);return 0;}
    return ok?1:0;
}

float dmsoft::ReadFloatAddr(long hwnd,LONGLONG addr){float v=0.0f;return ReadValue(P(impl),hwnd,addr,v)?v:0.0f;}
long dmsoft::WriteFloatAddr(long hwnd,LONGLONG addr,float v){return WriteValue(P(impl),hwnd,addr,v)?1:0;}
double dmsoft::ReadDoubleAddr(long hwnd,LONGLONG addr){double v=0.0;return ReadValue(P(impl),hwnd,addr,v)?v:0.0;}
long dmsoft::WriteDoubleAddr(long hwnd,LONGLONG addr,double v){return WriteValue(P(impl),hwnd,addr,v)?1:0;}

LONGLONG dmsoft::GetModuleBaseAddr(long hwnd, PCSTR module_name) {
    auto *p=P(impl); if(!p)return 0; DWORD pid=ResolvePid(p,hwnd); if(!pid)return 0;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(snap==INVALID_HANDLE_VALUE){SetNativeError(p,static_cast<long>(::GetLastError()));return 0;}
    MODULEENTRY32 me{};me.dwSize=sizeof(me);LONGLONG result=0;
    if(Module32First(snap,&me)){do{if(!module_name||!*module_name||_stricmp(me.szModule,module_name)==0||_stricmp(me.szExePath,module_name)==0){result=static_cast<LONGLONG>(reinterpret_cast<ULONG_PTR>(me.modBaseAddr));break;}}while(Module32Next(snap,&me));}
    CloseHandle(snap);SetNativeError(p,result?0:ERROR_MOD_NOT_FOUND);return result;
}

long dmsoft::GetModuleSize(long hwnd, PCSTR module_name) {
    auto *p=P(impl);if(!p)return 0;DWORD pid=ResolvePid(p,hwnd);if(!pid)return 0;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(snap==INVALID_HANDLE_VALUE){SetNativeError(p,static_cast<long>(::GetLastError()));return 0;}
    MODULEENTRY32 me{};me.dwSize=sizeof(me);DWORD result=0;
    if(Module32First(snap,&me)){do{if(!module_name||!*module_name||_stricmp(me.szModule,module_name)==0||_stricmp(me.szExePath,module_name)==0){result=me.modBaseSize;break;}}while(Module32Next(snap,&me));}
    CloseHandle(snap);SetNativeError(p,result?0:ERROR_MOD_NOT_FOUND);return static_cast<long>(result);
}

LONGLONG dmsoft::VirtualAllocEx(long hwnd,LONGLONG addr,long size,long type) {
    auto *p=P(impl);if(!p||size<=0)return 0;
    HANDLE h=OpenTarget(p,hwnd,PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ|PROCESS_QUERY_INFORMATION);if(!h)return 0;
    LPVOID mem=::VirtualAllocEx(h,reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(addr)),static_cast<SIZE_T>(size),MEM_COMMIT|MEM_RESERVE,ProtectForType(type));
    if(!mem)SetNativeError(p,static_cast<long>(::GetLastError()));else SetNativeError(p,0);CloseHandle(h);return static_cast<LONGLONG>(reinterpret_cast<ULONG_PTR>(mem));
}

long dmsoft::VirtualFreeEx(long hwnd,LONGLONG addr) {
    auto *p=P(impl);if(!p)return 0;HANDLE h=OpenTarget(p,hwnd,PROCESS_VM_OPERATION|PROCESS_QUERY_INFORMATION);if(!h)return 0;
    BOOL ok=::VirtualFreeEx(h,reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(addr)),0,MEM_RELEASE);
    if(!ok)SetNativeError(p,static_cast<long>(::GetLastError()));else SetNativeError(p,0);CloseHandle(h);return ok?1:0;
}


long dmsoft::GetScreenWidth() { return ::GetSystemMetrics(SM_CXSCREEN); }
long dmsoft::GetScreenHeight() { return ::GetSystemMetrics(SM_CYSCREEN); }

long dmsoft::GetScreenDepth() {
    HDC dc = ::GetDC(nullptr);
    if (!dc) return 0;
    const int depth = ::GetDeviceCaps(dc, BITSPIXEL) * ::GetDeviceCaps(dc, PLANES);
    ::ReleaseDC(nullptr, dc);
    return static_cast<long>(depth);
}

long dmsoft::GetDPI() {
    HDC dc = ::GetDC(nullptr);
    if (!dc) return 0;
    const int dpi = ::GetDeviceCaps(dc, LOGPIXELSX);
    ::ReleaseDC(nullptr, dc);
    return dpi == 96 ? 1 : 0;
}

long dmsoft::IsFileExist(PCSTR file) {
    if (!file || !*file) return 0;
    const DWORD a = ::GetFileAttributesA(file);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

long dmsoft::IsFolderExist(PCSTR folder) {
    if (!folder || !*folder) return 0;
    const DWORD a = ::GetFileAttributesA(folder);
    return (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

long dmsoft::CreateFolder(PCSTR folder_name) {
    if (!folder_name || !*folder_name) return 0;
    std::error_code ec;
    if (std::filesystem::exists(folder_name, ec)) return std::filesystem::is_directory(folder_name, ec) ? 1 : 0;
    return std::filesystem::create_directories(folder_name, ec) && !ec ? 1 : 0;
}

long dmsoft::DeleteFolder(PCSTR folder_name) {
    if (!folder_name || !*folder_name) return 0;
    std::error_code ec;
    std::filesystem::remove_all(folder_name, ec);
    return ec ? 0 : 1;
}

long dmsoft::DeleteFile(PCSTR file) {
    if (!file || !*file) return 0;
    return ::DeleteFileA(file) ? 1 : 0;
}

long dmsoft::MoveFile(PCSTR src_file, PCSTR dst_file) {
    if (!src_file || !dst_file) return 0;
    return ::MoveFileExA(src_file, dst_file, MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING) ? 1 : 0;
}

long dmsoft::CopyFile(PCSTR src_file, PCSTR dst_file, long over) {
    if (!src_file || !dst_file) return 0;
    return ::CopyFileA(src_file, dst_file, over ? FALSE : TRUE) ? 1 : 0;
}

long dmsoft::GetFileLength(PCSTR file) {
    if (!file) return -1;
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!::GetFileAttributesExA(file, GetFileExInfoStandard, &d)) return -1;
    ULARGE_INTEGER u{};
    u.LowPart = d.nFileSizeLow;
    u.HighPart = d.nFileSizeHigh;
    return u.QuadPart > static_cast<ULONGLONG>(LONG_MAX) ? LONG_MAX : static_cast<long>(u.QuadPart);
}

long dmsoft::WriteFile(PCSTR file, PCSTR content) {
    if (!file || !content) return 0;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return 0;
    out.write(content, static_cast<std::streamsize>(std::strlen(content)));
    return out.good() ? 1 : 0;
}

const char *dmsoft::ReadFile(PCSTR file) {
    auto *p = P(impl);
    if (!p || !file) return "";
    std::ifstream in(file, std::ios::binary);
    if (!in) { p->scratch.clear(); return p->scratch.c_str(); }
    p->scratch.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return p->scratch.c_str();
}

const char *dmsoft::ReadFileData(PCSTR file, long start_pos, long end_pos) {
    auto *p = P(impl);
    if (!p || !file || start_pos < 0) return "";
    std::ifstream in(file, std::ios::binary);
    if (!in) { p->scratch.clear(); return p->scratch.c_str(); }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || start_pos >= size) { p->scratch.clear(); return p->scratch.c_str(); }
    const std::streamoff end = (end_pos < start_pos || end_pos < 0)
        ? size : std::min<std::streamoff>(size, static_cast<std::streamoff>(end_pos) + 1);
    const std::streamoff len = end - static_cast<std::streamoff>(start_pos);
    p->scratch.resize(static_cast<size_t>(len));
    in.seekg(start_pos, std::ios::beg);
    in.read(p->scratch.data(), len);
    p->scratch.resize(static_cast<size_t>(in.gcount()));
    return p->scratch.c_str();
}

long dmsoft::WriteIni(PCSTR section, PCSTR key, PCSTR v, PCSTR file) {
    if (!section || !key || !file) return 0;
    return ::WritePrivateProfileStringA(section, key, v ? v : "", file) ? 1 : 0;
}

const char *dmsoft::ReadIni(PCSTR section, PCSTR key, PCSTR file) {
    auto *p = P(impl);
    if (!p || !section || !key || !file) return "";
    std::vector<char> buf(65536);
    const DWORD n = ::GetPrivateProfileStringA(section, key, "", buf.data(), static_cast<DWORD>(buf.size()), file);
    p->scratch.assign(buf.data(), n);
    return p->scratch.c_str();
}

long dmsoft::DeleteIni(PCSTR section, PCSTR key, PCSTR file) {
    if (!section || !file) return 0;
    return ::WritePrivateProfileStringA(section, key, nullptr, file) ? 1 : 0;
}

const char *dmsoft::EnumIniKey(PCSTR section, PCSTR file) {
    auto *p = P(impl);
    if (!p || !section || !file) return "";
    std::vector<char> buf(65536, 0);
    ::GetPrivateProfileStringA(section, nullptr, "", buf.data(), static_cast<DWORD>(buf.size()), file);
    p->scratch = JoinMultiSz(buf.data(), buf.size());
    return p->scratch.c_str();
}

const char *dmsoft::EnumIniSection(PCSTR file) {
    auto *p = P(impl);
    if (!p || !file) return "";
    std::vector<char> buf(65536, 0);
    ::GetPrivateProfileSectionNamesA(buf.data(), static_cast<DWORD>(buf.size()), file);
    p->scratch = JoinMultiSz(buf.data(), buf.size());
    return p->scratch.c_str();
}

long dmsoft::SetEnv(long index, PCSTR name, PCSTR value) {
    auto *p = P(impl);
    if (!p || !name) return 0;
    std::lock_guard<std::mutex> lock(p->state_mutex);
    p->env[{index, name}] = value ? value : "";
    return 1;
}

long dmsoft::DelEnv(long index, PCSTR name) {
    auto *p = P(impl);
    if (!p || !name) return 0;
    std::lock_guard<std::mutex> lock(p->state_mutex);
    return p->env.erase({index, name}) ? 1 : 0;
}

const char *dmsoft::GetEnv(long index, PCSTR name) {
    auto *p = P(impl);
    if (!p || !name) return "";
    std::lock_guard<std::mutex> lock(p->state_mutex);
    const auto it = p->env.find({index, name});
    p->scratch = it == p->env.end() ? "" : it->second;
    return p->scratch.c_str();
}

long dmsoft::GetMemoryUsage() {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    return ::GlobalMemoryStatusEx(&ms) ? static_cast<long>(ms.dwMemoryLoad) : 0;
}

long dmsoft::GetCpuUsage() {
    auto *p = P(impl);
    if (!p) return 0;
    FILETIME idle{}, kernel{}, user{};
    if (!::GetSystemTimes(&idle, &kernel, &user)) return 0;
    const ULONGLONG i = FileTime64(idle), k = FileTime64(kernel), u = FileTime64(user);
    std::lock_guard<std::mutex> lock(p->state_mutex);
    if (!p->cpu_sample_valid) {
        p->prev_idle = i; p->prev_kernel = k; p->prev_user = u; p->cpu_sample_valid = true;
        return 0;
    }
    const ULONGLONG di = i - p->prev_idle, dk = k - p->prev_kernel, du = u - p->prev_user;
    p->prev_idle = i; p->prev_kernel = k; p->prev_user = u;
    const ULONGLONG total = dk + du;
    if (!total) return 0;
    const ULONGLONG busy = total > di ? total - di : 0;
    return static_cast<long>((busy * 100) / total);
}

long dmsoft::CheckFontSmooth() {
    BOOL enabled = FALSE;
    return ::SystemParametersInfoA(SPI_GETFONTSMOOTHING, 0, &enabled, 0) && enabled ? 1 : 0;
}

long dmsoft::EnableFontSmooth() {
    return ::SystemParametersInfoA(SPI_SETFONTSMOOTHING, TRUE, nullptr, SPIF_SENDCHANGE) ? 1 : 0;
}

long dmsoft::DisableFontSmooth() {
    return ::SystemParametersInfoA(SPI_SETFONTSMOOTHING, FALSE, nullptr, SPIF_SENDCHANGE) ? 1 : 0;
}

long dmsoft::DisablePowerSave() {
    return ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) ? 1 : 0;
}

long dmsoft::DisableScreenSave() {
    return ::SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED) ? 1 : 0;
}

long dmsoft::DisableCloseDisplayAndSleep() {
    return ::SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) ? 1 : 0;
}

long dmsoft::StrStr(PCSTR s, PCSTR str) {
    if (!s || !str) return -1;
    const char *p = std::strstr(s, str);
    return p ? static_cast<long>(p - s) : -1;
}

const char *dmsoft::RGB2BGR(PCSTR rgb_color) {
    auto *p = P(impl);
    if (!p || !rgb_color) return "";
    std::string v(rgb_color);
    if (v.size() == 6) p->scratch = v.substr(4,2) + v.substr(2,2) + v.substr(0,2);
    else p->scratch = v;
    return p->scratch.c_str();
}

const char *dmsoft::BGR2RGB(PCSTR bgr_color) { return RGB2BGR(bgr_color); }

long dmsoft::GetOsBuildNumber() {
    using RtlGetVersionFn = LONG (WINAPI *)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!fn) return 0;
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    return fn(&vi) == 0 ? static_cast<long>(vi.dwBuildNumber) : 0;
}

long dmsoft::GetTime() { return static_cast<long>(::GetTickCount()); }


const char *dmsoft::GetClipboard() {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch.clear();
    if (!::OpenClipboard(nullptr)) return p->scratch.c_str();
    HANDLE h = ::GetClipboardData(CF_TEXT);
    if (h) {
        const char *text = static_cast<const char *>(::GlobalLock(h));
        if (text) { p->scratch = text; ::GlobalUnlock(h); }
    }
    ::CloseClipboard();
    return p->scratch.c_str();
}

long dmsoft::SetClipboard(PCSTR data) {
    if (!data || !::OpenClipboard(nullptr)) return 0;
    if (!::EmptyClipboard()) { ::CloseClipboard(); return 0; }
    const SIZE_T bytes = std::strlen(data) + 1;
    HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) { ::CloseClipboard(); return 0; }
    void *dst = ::GlobalLock(mem);
    if (!dst) { ::GlobalFree(mem); ::CloseClipboard(); return 0; }
    std::memcpy(dst, data, bytes);
    ::GlobalUnlock(mem);
    if (!::SetClipboardData(CF_TEXT, mem)) { ::GlobalFree(mem); ::CloseClipboard(); return 0; }
    ::CloseClipboard();
    return 1;
}

long dmsoft::FindWindow(PCSTR class_name, PCSTR title_name) {
    HWND h = ::FindWindowA((class_name && *class_name) ? class_name : nullptr,
                           (title_name && *title_name) ? title_name : nullptr);
    return static_cast<long>(reinterpret_cast<INT_PTR>(h));
}

long dmsoft::FindWindowEx(long parent, PCSTR class_name, PCSTR title_name) {
    HWND h = ::FindWindowExA(HwndFromLong(parent), nullptr,
                             (class_name && *class_name) ? class_name : nullptr,
                             (title_name && *title_name) ? title_name : nullptr);
    return static_cast<long>(reinterpret_cast<INT_PTR>(h));
}

long dmsoft::GetForegroundWindow() {
    return static_cast<long>(reinterpret_cast<INT_PTR>(::GetForegroundWindow()));
}

long dmsoft::GetMousePointWindow() {
    POINT pt{};
    if (!::GetCursorPos(&pt)) return 0;
    return static_cast<long>(reinterpret_cast<INT_PTR>(::WindowFromPoint(pt)));
}

long dmsoft::GetPointWindow(long x, long y) {
    POINT pt{static_cast<LONG>(x), static_cast<LONG>(y)};
    return static_cast<long>(reinterpret_cast<INT_PTR>(::WindowFromPoint(pt)));
}

long dmsoft::GetWindowRect(long hwnd, long *x1, long *y1, long *x2, long *y2) {
    if (!x1 || !y1 || !x2 || !y2) return 0;
    RECT r{};
    if (!::GetWindowRect(HwndFromLong(hwnd), &r)) return 0;
    *x1 = r.left; *y1 = r.top; *x2 = r.right; *y2 = r.bottom;
    return 1;
}

long dmsoft::GetClientRect(long hwnd, long *x1, long *y1, long *x2, long *y2) {
    if (!x1 || !y1 || !x2 || !y2) return 0;
    RECT r{};
    if (!::GetClientRect(HwndFromLong(hwnd), &r)) return 0;
    *x1 = r.left; *y1 = r.top; *x2 = r.right; *y2 = r.bottom;
    return 1;
}

long dmsoft::GetClientSize(long hwnd, long *width, long *height) {
    if (!width || !height) return 0;
    RECT r{};
    if (!::GetClientRect(HwndFromLong(hwnd), &r)) return 0;
    *width = r.right - r.left; *height = r.bottom - r.top;
    return 1;
}

long dmsoft::MoveWindow(long hwnd, long x, long y) {
    RECT r{};
    HWND h = HwndFromLong(hwnd);
    if (!::GetWindowRect(h, &r)) return 0;
    return ::MoveWindow(h, x, y, r.right - r.left, r.bottom - r.top, TRUE) ? 1 : 0;
}

long dmsoft::SetWindowSize(long hwnd, long width, long height) {
    HWND h = HwndFromLong(hwnd);
    return ::SetWindowPos(h, nullptr, 0, 0, width, height,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ? 1 : 0;
}

long dmsoft::SetClientSize(long hwnd, long width, long height) {
    HWND h = HwndFromLong(hwnd);
    RECT rc{0, 0, width, height};
    const DWORD style = static_cast<DWORD>(::GetWindowLongPtr(h, GWL_STYLE));
    const DWORD ex = static_cast<DWORD>(::GetWindowLongPtr(h, GWL_EXSTYLE));
    if (!::AdjustWindowRectEx(&rc, style, ::GetMenu(h) != nullptr, ex)) return 0;
    return ::SetWindowPos(h, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                          SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ? 1 : 0;
}

long dmsoft::SetWindowText(long hwnd, PCSTR text) {
    return ::SetWindowTextA(HwndFromLong(hwnd), text ? text : "") ? 1 : 0;
}

const char *dmsoft::GetWindowTitle(long hwnd) {
    auto *p = P(impl);
    if (!p) return "";
    const int n = ::GetWindowTextLengthA(HwndFromLong(hwnd));
    std::vector<char> buf(static_cast<size_t>(std::max(1, n + 1)), 0);
    ::GetWindowTextA(HwndFromLong(hwnd), buf.data(), static_cast<int>(buf.size()));
    p->scratch = buf.data();
    return p->scratch.c_str();
}

const char *dmsoft::GetWindowClass(long hwnd) {
    auto *p = P(impl);
    if (!p) return "";
    char buf[512]{};
    ::GetClassNameA(HwndFromLong(hwnd), buf, static_cast<int>(sizeof(buf)));
    p->scratch = buf;
    return p->scratch.c_str();
}

long dmsoft::ClientToScreen(long hwnd, long *x, long *y) {
    if (!x || !y) return 0;
    POINT pt{*x, *y};
    if (!::ClientToScreen(HwndFromLong(hwnd), &pt)) return 0;
    *x = pt.x; *y = pt.y; return 1;
}

long dmsoft::ScreenToClient(long hwnd, long *x, long *y) {
    if (!x || !y) return 0;
    POINT pt{*x, *y};
    if (!::ScreenToClient(HwndFromLong(hwnd), &pt)) return 0;
    *x = pt.x; *y = pt.y; return 1;
}

long dmsoft::GetCursorPos(long *x, long *y) {
    if (!x || !y) return 0;
    auto *p = P(impl);
    if (p) {
        const auto b = BindingSnapshotForObjectCompat(p);
        if (b.hwnd && ::IsWindow(b.hwnd)) {
            if (BoundInputEnabledCompat(b) &&
                (_stricmp(b.mouse.c_str(), "windows") == 0 ||
                 _stricmp(b.mouse.c_str(), "windows3") == 0)) {
                *x = b.mouse_x;
                *y = b.mouse_y;
                return 1;
            }
            POINT pt{};
            if (!::GetCursorPos(&pt) ||
                !::ScreenToClient(b.hwnd, &pt))
                return 0;
            *x = pt.x;
            *y = pt.y;
            return 1;
        }
    }
    POINT pt{};
    if (!::GetCursorPos(&pt)) return 0;
    *x = pt.x;
    *y = pt.y;
    return 1;
}

const char *dmsoft::GetWindowProcessPath(long hwnd) {
    auto *p = P(impl);
    if (!p) return "";
    DWORD pid = 0;
    ::GetWindowThreadProcessId(HwndFromLong(hwnd), &pid);
    HANDLE proc = pid ? ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
    if (!proc) { p->scratch.clear(); return p->scratch.c_str(); }
    std::vector<char> buf(32768, 0);
    DWORD n = static_cast<DWORD>(buf.size());
    if (!::QueryFullProcessImageNameA(proc, 0, buf.data(), &n)) n = 0;
    ::CloseHandle(proc);
    p->scratch.assign(buf.data(), n);
    return p->scratch.c_str();
}

const char *dmsoft::GetRealPath(PCSTR path) {
    auto *p = P(impl);
    if (!p || !path) return "";
    std::vector<char> buf(32768, 0);
    const DWORD n = ::GetFullPathNameA(path, static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    p->scratch = (n > 0 && n < buf.size()) ? std::string(buf.data(), n) : std::string();
    return p->scratch.c_str();
}


const char *dmsoft::Md5(PCSTR str) {
    auto *p = P(impl);
    if (!p || !str) return "";

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[16]{};
    DWORD digest_len = sizeof(digest);

    if (!::CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        p->scratch.clear();
        return p->scratch.c_str();
    }
    const bool ok =
        ::CryptCreateHash(prov, CALG_MD5, 0, 0, &hash) &&
        ::CryptHashData(hash, reinterpret_cast<const BYTE *>(str),
                        static_cast<DWORD>(std::strlen(str)), 0) &&
        ::CryptGetHashParam(hash, HP_HASHVAL, digest, &digest_len, 0);

    if (hash) ::CryptDestroyHash(hash);
    ::CryptReleaseContext(prov, 0);

    if (!ok || digest_len != sizeof(digest)) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    static const char hex[] = "0123456789abcdef";
    p->scratch.resize(32);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        p->scratch[i * 2] = hex[(digest[i] >> 4) & 0x0f];
        p->scratch[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    return p->scratch.c_str();
}

long dmsoft::GetResultCount(PCSTR str) {
    return static_cast<long>(SplitPipeCompat(str).size());
}

long dmsoft::GetResultPos(PCSTR str, long index, long *x, long *y) {
    if (!x || !y || index < 0) return 0;
    const auto items = SplitPipeCompat(str);
    if (static_cast<size_t>(index) >= items.size()) return 0;
    return ParseResultXYCompat(items[static_cast<size_t>(index)], x, y) ? 1 : 0;
}

const char *dmsoft::IntToData(LONGLONG int_value, long type) {
    auto *p = P(impl);
    if (!p) return "";
    switch (type) {
    case 0: { std::int32_t v = static_cast<std::int32_t>(int_value); p->scratch = HexBytesCompat(&v, sizeof(v)); break; }
    case 1: { std::int16_t v = static_cast<std::int16_t>(int_value); p->scratch = HexBytesCompat(&v, sizeof(v)); break; }
    case 2: { std::int8_t  v = static_cast<std::int8_t>(int_value);  p->scratch = HexBytesCompat(&v, sizeof(v)); break; }
    case 3: { std::int64_t v = static_cast<std::int64_t>(int_value); p->scratch = HexBytesCompat(&v, sizeof(v)); break; }
    default: p->scratch.clear(); break;
    }
    return p->scratch.c_str();
}

const char *dmsoft::FloatToData(float float_value) {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch = HexBytesCompat(&float_value, sizeof(float_value));
    return p->scratch.c_str();
}

const char *dmsoft::DoubleToData(double double_value) {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch = HexBytesCompat(&double_value, sizeof(double_value));
    return p->scratch.c_str();
}

const char *dmsoft::StringToData(PCSTR string_value, long type) {
    auto *p = P(impl);
    if (!p || !string_value) return "";
    if (type == 0) {
        p->scratch = HexBytesCompat(string_value, std::strlen(string_value));
        return p->scratch.c_str();
    }
    if (type == 1 || type == 2) {
        const int count = ::MultiByteToWideChar(CP_ACP, 0, string_value, -1, nullptr, 0);
        if (count <= 0) { p->scratch.clear(); return p->scratch.c_str(); }
        std::wstring wide(static_cast<size_t>(count), L'\0');
        if (::MultiByteToWideChar(CP_ACP, 0, string_value, -1, wide.data(), count) <= 0) {
            p->scratch.clear();
            return p->scratch.c_str();
        }
        if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
        if (type == 1) {
            p->scratch = HexBytesCompat(wide.data(), wide.size() * sizeof(wchar_t));
            return p->scratch.c_str();
        }
        const int utf8_len = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (utf8_len <= 0) { p->scratch.clear(); return p->scratch.c_str(); }
        std::string utf8(static_cast<size_t>(utf8_len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
            utf8.data(), utf8_len, nullptr, nullptr);
        p->scratch = HexBytesCompat(utf8.data(), utf8.size());
        return p->scratch.c_str();
    }
    p->scratch.clear();
    return p->scratch.c_str();
}

long dmsoft::GetLocale() {
    return ::GetACP() == 936 ? 1 : 0;
}

long dmsoft::CheckUAC() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS st = ::RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        "EnableLUA",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    return st == ERROR_SUCCESS && value != 0 ? 1 : 0;
}


long dmsoft::GetWindowState(long hwnd, long flag) {
    const HWND h = HwndFromLong(hwnd);
    if (!h) return 0;

    switch (flag) {
    case 0:
        return ::IsWindow(h) ? 1 : 0;
    case 1:
        return ::GetForegroundWindow() == h ? 1 : 0;
    case 2:
        return ::IsWindowVisible(h) ? 1 : 0;
    case 3:
        return ::IsIconic(h) ? 1 : 0;
    case 4:
        return ::IsZoomed(h) ? 1 : 0;
    case 5:
        return (::GetWindowLongPtr(h, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;
    case 6:
        return ::IsHungAppWindow(h) ? 1 : 0;
    case 7:
        return ::IsWindowEnabled(h) ? 1 : 0;
    case 8: {
        DWORD_PTR result = 0;
        const LRESULT ok = ::SendMessageTimeoutA(
            h, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &result);
        return ok ? 0 : 1;
    }
    case 9: {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(h, &pid);
        return pid && ProcessIs64BitCompat(pid) ? 1 : 0;
    }
    default:
        return 0;
    }
}

long dmsoft::GetWindow(long hwnd, long flag) {
    const HWND h = HwndFromLong(hwnd);
    HWND result = nullptr;
    switch (flag) {
    case 0: result = ::GetParent(h); break;
    case 1: result = ::GetWindow(h, GW_CHILD); break;
    case 2: result = ::GetWindow(h, GW_HWNDFIRST); break;
    case 3: result = ::GetWindow(h, GW_HWNDLAST); break;
    case 4: result = ::GetWindow(h, GW_HWNDNEXT); break;
    case 5: result = ::GetWindow(h, GW_HWNDPREV); break;
    case 6: result = ::GetWindow(h, GW_OWNER); break;
    case 7: result = ::GetAncestor(h, GA_ROOT); break;
    default: break;
    }
    return static_cast<long>(reinterpret_cast<INT_PTR>(result));
}

long dmsoft::GetForegroundFocus() {
    HWND foreground = ::GetForegroundWindow();
    if (!foreground) return 0;

    const DWORD tid = ::GetWindowThreadProcessId(foreground, nullptr);
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    if (!tid || !::GetGUIThreadInfo(tid, &info)) return 0;
    return static_cast<long>(reinterpret_cast<INT_PTR>(info.hwndFocus));
}

long dmsoft::SetWindowState(long hwnd, long flag) {
    HWND h = HwndFromLong(hwnd);
    if (!::IsWindow(h)) return 0;

    switch (flag) {
    case 0:
        return ::PostMessageA(h, WM_CLOSE, 0, 0) ? 1 : 0;
    case 1:
        ::ShowWindow(h, SW_SHOW);
        return ::SetForegroundWindow(h) ? 1 : 0;
    case 2:
        return ::ShowWindow(h, SW_MINIMIZE) ? 1 : 1;
    case 3: {
        ::ShowWindow(h, SW_MINIMIZE);
        DWORD pid = 0;
        ::GetWindowThreadProcessId(h, &pid);
        HANDLE process = pid ? ::OpenProcess(PROCESS_SET_QUOTA, FALSE, pid) : nullptr;
        if (process) {
            ::SetProcessWorkingSetSize(process, static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
            ::CloseHandle(process);
        }
        ::SetForegroundWindow(h);
        return 1;
    }
    case 4:
        ::ShowWindow(h, SW_MAXIMIZE);
        ::SetForegroundWindow(h);
        return 1;
    case 5:
        ::ShowWindow(h, SW_SHOWNOACTIVATE);
        return 1;
    case 6:
        ::ShowWindow(h, SW_HIDE);
        return 1;
    case 7:
        ::ShowWindow(h, SW_SHOWNA);
        return 1;
    case 8:
        return ::SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) ? 1 : 0;
    case 9:
        return ::SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) ? 1 : 0;
    case 10:
        return ::EnableWindow(h, FALSE) ? 1 : 1;
    case 11:
        return ::EnableWindow(h, TRUE) ? 1 : 1;
    case 12:
        ::ShowWindow(h, SW_RESTORE);
        ::SetForegroundWindow(h);
        return 1;
    case 13: {
        DWORD pid = 0;
        ::GetWindowThreadProcessId(h, &pid);
        HANDLE process = pid ? ::OpenProcess(PROCESS_TERMINATE, FALSE, pid) : nullptr;
        if (!process) return 0;
        const BOOL ok = ::TerminateProcess(process, 0);
        ::CloseHandle(process);
        return ok ? 1 : 0;
    }
    case 14:
        return ::FlashWindow(h, TRUE) ? 1 : 0;
    case 15: {
        const DWORD target_tid = ::GetWindowThreadProcessId(h, nullptr);
        const DWORD self_tid = ::GetCurrentThreadId();
        BOOL attached = FALSE;
        if (target_tid && target_tid != self_tid)
            attached = ::AttachThreadInput(self_tid, target_tid, TRUE);
        const HWND old_focus = ::SetFocus(h);
        if (attached) ::AttachThreadInput(self_tid, target_tid, FALSE);
        return old_focus || ::GetFocus() == h ? 1 : 0;
    }
    default:
        return 0;
    }
}

const char *dmsoft::EnumProcess(PCSTR name) {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch = JoinPidsCompat(EnumProcessIdsCompat(name));
    return p->scratch.c_str();
}

const char *dmsoft::EnumWindow(long parent, PCSTR title, PCSTR class_name, long filter) {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch = JoinHwndsCompat(EnumWindowsCompat(HwndFromLong(parent), 0, title, class_name, filter));
    return p->scratch.c_str();
}

long dmsoft::FindWindowByProcessId(long process_id, PCSTR class_name, PCSTR title_name) {
    auto windows = EnumWindowsCompat(nullptr, static_cast<DWORD>(process_id), title_name, class_name, 1 | 2 | 8 | 16);
    return windows.empty() ? 0 : static_cast<long>(reinterpret_cast<INT_PTR>(windows.front()));
}

long dmsoft::FindWindowByProcess(PCSTR process_name, PCSTR class_name, PCSTR title_name) {
    const auto pids = EnumProcessIdsCompat(process_name);
    for (DWORD pid : pids) {
        auto windows = EnumWindowsCompat(nullptr, pid, title_name, class_name, 1 | 2 | 8 | 16);
        if (!windows.empty())
            return static_cast<long>(reinterpret_cast<INT_PTR>(windows.front()));
    }
    return 0;
}

const char *dmsoft::EnumWindowByProcessId(long pid, PCSTR title, PCSTR class_name, long filter) {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch = JoinHwndsCompat(
        EnumWindowsCompat(nullptr, static_cast<DWORD>(pid), title, class_name, filter));
    return p->scratch.c_str();
}

const char *dmsoft::EnumWindowByProcess(PCSTR process_name, PCSTR title, PCSTR class_name, long filter) {
    auto *p = P(impl);
    if (!p) return "";

    std::vector<HWND> all;
    const auto pids = EnumProcessIdsCompat(process_name);
    const size_t count = (filter & 4) && !pids.empty() ? 1 : pids.size();
    for (size_t i = 0; i < count; ++i) {
        auto part = EnumWindowsCompat(nullptr, pids[i], title, class_name, filter & ~4L);
        all.insert(all.end(), part.begin(), part.end());
    }
    p->scratch = JoinHwndsCompat(all);
    return p->scratch.c_str();
}


long dmsoft::OpenProcess(long pid) {
    HANDLE process = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, static_cast<DWORD>(pid));
    if (!process) {
        process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                PROCESS_VM_WRITE | PROCESS_VM_OPERATION |
                                PROCESS_SET_QUOTA | PROCESS_TERMINATE,
                                FALSE, static_cast<DWORD>(pid));
    }
    return static_cast<long>(reinterpret_cast<INT_PTR>(process));
}

long dmsoft::TerminateProcess(long pid) {
    HANDLE process = ::OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!process) return 0;
    const BOOL ok = ::TerminateProcess(process, 0);
    ::CloseHandle(process);
    return ok ? 1 : 0;
}

long dmsoft::FreeProcessMemory(long hwnd) {
    auto *p = P(impl);
    if (!p) return 0;
    HANDLE process = OpenTarget(p, hwnd, PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION);
    if (!process) return 0;
    const BOOL ok = ::EmptyWorkingSet(process);
    if (!ok) SetNativeError(p, static_cast<long>(::GetLastError()));
    else SetNativeError(p, 0);
    ::CloseHandle(process);
    return ok ? 1 : 0;
}

long dmsoft::VirtualProtectEx(long hwnd, LONGLONG addr, long size, long type, long old_protect) {
    auto *p = P(impl);
    if (!p || size <= 0) return 0;

    DWORD new_protect = 0;
    if (type == 0) new_protect = PAGE_EXECUTE_READWRITE;
    else if (type == 1) new_protect = static_cast<DWORD>(old_protect);
    else {
        SetNativeError(p, ERROR_INVALID_PARAMETER);
        return 0;
    }

    HANDLE process = OpenTarget(
        p, hwnd, PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION);
    if (!process) return 0;

    DWORD previous = 0;
    const BOOL ok = ::VirtualProtectEx(
        process,
        reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(addr)),
        static_cast<SIZE_T>(size),
        new_protect,
        &previous);
    if (!ok) {
        SetNativeError(p, static_cast<long>(::GetLastError()));
        previous = 0;
    } else {
        SetNativeError(p, 0);
    }
    ::CloseHandle(process);
    return static_cast<long>(previous);
}

const char *dmsoft::VirtualQueryEx(long hwnd, LONGLONG addr, long pmbi) {
    auto *p = P(impl);
    if (!p) return "";

    HANDLE process = OpenTarget(p, hwnd, PROCESS_QUERY_INFORMATION | PROCESS_VM_READ);
    if (!process) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    MEMORY_BASIC_INFORMATION mbi{};
    const SIZE_T got = ::VirtualQueryEx(
        process,
        reinterpret_cast<LPCVOID>(static_cast<ULONG_PTR>(addr)),
        &mbi,
        sizeof(mbi));
    if (!got) {
        SetNativeError(p, static_cast<long>(::GetLastError()));
        ::CloseHandle(process);
        p->scratch.clear();
        return p->scratch.c_str();
    }

    DWORD pid = ResolvePid(p, hwnd);
    const bool target64 = pid && ProcessIs64BitCompat(pid);

    struct MBI32Compat {
        DWORD BaseAddress;
        DWORD AllocationBase;
        DWORD AllocationProtect;
        DWORD RegionSize;
        DWORD State;
        DWORD Protect;
        DWORD Type;
    };
    struct alignas(16) MBI64Compat {
        ULONGLONG BaseAddress;
        ULONGLONG AllocationBase;
        DWORD AllocationProtect;
        DWORD alignment1;
        ULONGLONG RegionSize;
        DWORD State;
        DWORD Protect;
        DWORD Type;
        DWORD alignment2;
    };

    if (pmbi != 0) {
        SIZE_T written = 0;
        if (target64) {
            MBI64Compat out{};
            out.BaseAddress = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(mbi.BaseAddress));
            out.AllocationBase = static_cast<ULONGLONG>(reinterpret_cast<ULONG_PTR>(mbi.AllocationBase));
            out.AllocationProtect = mbi.AllocationProtect;
            out.RegionSize = static_cast<ULONGLONG>(mbi.RegionSize);
            out.State = mbi.State;
            out.Protect = mbi.Protect;
            out.Type = mbi.Type;
            ::WriteProcessMemory(
                ::GetCurrentProcess(),
                reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(static_cast<unsigned long>(pmbi))),
                &out, sizeof(out), &written);
        } else {
            MBI32Compat out{};
            out.BaseAddress = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(mbi.BaseAddress));
            out.AllocationBase = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(mbi.AllocationBase));
            out.AllocationProtect = mbi.AllocationProtect;
            out.RegionSize = static_cast<DWORD>(mbi.RegionSize);
            out.State = mbi.State;
            out.Protect = mbi.Protect;
            out.Type = mbi.Type;
            ::WriteProcessMemory(
                ::GetCurrentProcess(),
                reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(static_cast<unsigned long>(pmbi))),
                &out, sizeof(out), &written);
        }
    }

    std::ostringstream oss;
    oss << static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(mbi.BaseAddress)) << ','
        << static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(mbi.AllocationBase)) << ','
        << static_cast<unsigned long>(mbi.AllocationProtect) << ','
        << static_cast<unsigned long long>(mbi.RegionSize) << ','
        << static_cast<unsigned long>(mbi.State) << ','
        << static_cast<unsigned long>(mbi.Protect) << ','
        << static_cast<unsigned long>(mbi.Type);
    p->scratch = oss.str();
    SetNativeError(p, 0);
    ::CloseHandle(process);
    return p->scratch.c_str();
}


long dmsoft::GetKeyState(long vk) {
    if (vk < 0 || vk > 255) return 0;
    return (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) ? 1 : 0;
}

long dmsoft::GetMouseSpeed() {
    UINT speed = 0;
    if (!::SystemParametersInfoA(SPI_GETMOUSESPEED, 0, &speed, 0)) return 0;
    return MouseSpeedLevelFromWindowsCompat(static_cast<long>(speed));
}

long dmsoft::SetMouseSpeed(long speed) {
    const long native_speed = WindowsMouseSpeedFromLevelCompat(speed);
    if (!native_speed) return 0;
    return ::SystemParametersInfoA(
        SPI_SETMOUSESPEED, 0,
        reinterpret_cast<PVOID>(static_cast<INT_PTR>(native_speed)),
        SPIF_SENDCHANGE) ? 1 : 0;
}

long dmsoft::SetWindowTransparent(long hwnd, long v) {
    if (v < 0 || v > 255) return 0;
    HWND h = HwndFromLong(hwnd);
    if (!::IsWindow(h)) return 0;

    LONG_PTR ex = ::GetWindowLongPtr(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) {
        ::SetLastError(0);
        const LONG_PTR previous = ::SetWindowLongPtr(h, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        if (previous == 0 && ::GetLastError() != 0) return 0;
    }
    return ::SetLayeredWindowAttributes(h, 0, static_cast<BYTE>(v), LWA_ALPHA) ? 1 : 0;
}

long dmsoft::Beep(long fre, long delay) {
    if (fre < 37 || fre > 32767 || delay < 0) return 0;
    return ::Beep(static_cast<DWORD>(fre), static_cast<DWORD>(delay)) ? 1 : 0;
}

long dmsoft::RunApp(PCSTR path, long mode) {
    if (!path || !*path) return 0;

    if (mode == 0) {
        const HINSTANCE r = ::ShellExecuteA(nullptr, "open", path, nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(r) > 32 ? 1 : 0;
    }
    if (mode == 1) {
        std::string cmd(path);
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (::CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            return 1;
        }
        const HINSTANCE r = ::ShellExecuteA(nullptr, "open", path, nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(r) > 32 ? 1 : 0;
    }
    return 0;
}


long dmsoft::SetPath(PCSTR path) {
    auto *p = P(impl);
    if (!p || !path || !*path) return 0;
    const std::string normalized = NormalizeGlobalPathCompat(path);
    if (normalized.empty()) return 0;
    p->global_path = normalized;
    return 1;
}

const char *dmsoft::GetPath() {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch = p->global_path;
    return p->scratch.c_str();
}

const char *dmsoft::GetBasePath() {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch = ModuleDirectoryCompat(true);
    return p->scratch.c_str();
}

long dmsoft::GetID() {
    auto *p = P(impl);
    return p ? p->id : 0;
}

long dmsoft::GetDmCount() {
    return g_dm_object_count.load(std::memory_order_relaxed);
}

long dmsoft::SetEnumWindowDelay(long delay) {
    auto *p = P(impl);
    if (!p || delay < 0) return 0;
    p->enum_window_delay = delay;
    return 1;
}

long dmsoft::SetShowErrorMsg(long show) {
    auto *p = P(impl);
    if (!p) return 0;
    p->show_error_msg = show != 0;
    return 1;
}


const char *dmsoft::ReadStringAddr(long hwnd, LONGLONG addr, long type, long len) {
    auto *p = P(impl);
    if (!p || len < 0 || type < 0 || type > 2) return "";

    std::vector<unsigned char> bytes;
    if (len > 0) {
        bytes.resize(static_cast<size_t>(len));
        if (!ReadRemoteBytesCompat(p, hwnd, addr, bytes.data(), bytes.size())) {
            p->scratch.clear();
            return p->scratch.c_str();
        }
    } else if (type == 1) {
        constexpr size_t kMaxChars = 1024 * 1024 / sizeof(wchar_t);
        for (size_t i = 0; i < kMaxChars; ++i) {
            wchar_t ch = 0;
            if (!ReadRemoteBytesCompat(
                    p, hwnd, addr + static_cast<LONGLONG>(i * sizeof(wchar_t)),
                    &ch, sizeof(ch))) {
                p->scratch.clear();
                return p->scratch.c_str();
            }
            if (ch == L'\0') break;
            const auto *raw = reinterpret_cast<const unsigned char *>(&ch);
            bytes.insert(bytes.end(), raw, raw + sizeof(ch));
        }
    } else {
        constexpr size_t kMaxBytes = 1024 * 1024;
        for (size_t i = 0; i < kMaxBytes; ++i) {
            unsigned char ch = 0;
            if (!ReadRemoteBytesCompat(p, hwnd, addr + static_cast<LONGLONG>(i), &ch, 1)) {
                p->scratch.clear();
                return p->scratch.c_str();
            }
            if (ch == 0) break;
            bytes.push_back(ch);
        }
    }

    if (type == 0) {
        auto it = std::find(bytes.begin(), bytes.end(), 0);
        p->scratch.assign(bytes.begin(), it);
        return p->scratch.c_str();
    }

    if (type == 1) {
        const size_t wchar_count = bytes.size() / sizeof(wchar_t);
        std::wstring wide(wchar_count, L'\0');
        if (!wide.empty()) std::memcpy(wide.data(), bytes.data(), wchar_count * sizeof(wchar_t));
        const auto nul = std::find(wide.begin(), wide.end(), L'\0');
        wide.erase(nul, wide.end());
        p->scratch = WideToAcpCompat(wide.data(), static_cast<int>(wide.size()));
        return p->scratch.c_str();
    }

    auto it = std::find(bytes.begin(), bytes.end(), 0);
    const std::string utf8(bytes.begin(), it);
    p->scratch = Utf8ToAcpCompat(utf8);
    return p->scratch.c_str();
}

long dmsoft::WriteStringAddr(long hwnd, LONGLONG addr, long type, PCSTR v) {
    auto *p = P(impl);
    if (!p || !v || type < 0 || type > 2) return 0;

    if (type == 0) {
        const size_t n = std::strlen(v) + 1;
        return WriteRemoteBytesCompat(p, hwnd, addr, v, n) ? 1 : 0;
    }
    if (type == 1) {
        const std::wstring wide = AcpToWideCompat(v);
        if (wide.empty() && *v) return 0;
        return WriteRemoteBytesCompat(
            p, hwnd, addr, wide.data(), wide.size() * sizeof(wchar_t)) ? 1 : 0;
    }

    std::string utf8 = AcpToUtf8Compat(v);
    utf8.push_back('\0');
    return WriteRemoteBytesCompat(p, hwnd, addr, utf8.data(), utf8.size()) ? 1 : 0;
}

LONGLONG dmsoft::ReadInt(long hwnd, PCSTR addr, long type) {
    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved)) return 0;
    return ReadIntAddr(hwnd, resolved, type);
}

long dmsoft::WriteInt(long hwnd, PCSTR addr, long type, LONGLONG v) {
    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved)) return 0;
    return WriteIntAddr(hwnd, resolved, type, v);
}

float dmsoft::ReadFloat(long hwnd, PCSTR addr) {
    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved)) return 0.0f;
    return ReadFloatAddr(hwnd, resolved);
}

long dmsoft::WriteFloat(long hwnd, PCSTR addr, float float_value) {
    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved)) return 0;
    return WriteFloatAddr(hwnd, resolved, float_value);
}

double dmsoft::ReadDouble(long hwnd, PCSTR addr) {
    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved)) return 0.0;
    return ReadDoubleAddr(hwnd, resolved);
}

long dmsoft::WriteDouble(long hwnd, PCSTR addr, double double_value) {
    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved)) return 0;
    return WriteDoubleAddr(hwnd, resolved, double_value);
}

const char *dmsoft::ReadData(long hwnd, PCSTR addr, long len) {
    LONGLONG resolved = 0;
    auto *p = P(impl);
    if (!ResolveAddressExprCompat(p, hwnd, addr, resolved)) {
        if (p) p->scratch.clear();
        return p ? p->scratch.c_str() : "";
    }
    return ReadDataAddr(hwnd, resolved, len);
}

long dmsoft::WriteData(long hwnd, PCSTR addr, PCSTR data) {
    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved)) return 0;
    return WriteDataAddr(hwnd, resolved, data);
}

const char *dmsoft::ReadString(long hwnd, PCSTR addr, long type, long len) {
    LONGLONG resolved = 0;
    auto *p = P(impl);
    if (!ResolveAddressExprCompat(p, hwnd, addr, resolved)) {
        if (p) p->scratch.clear();
        return p ? p->scratch.c_str() : "";
    }
    return ReadStringAddr(hwnd, resolved, type, len);
}

long dmsoft::WriteString(long hwnd, PCSTR addr, long type, PCSTR v) {
    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved)) return 0;
    return WriteStringAddr(hwnd, resolved, type, v);
}


const char *dmsoft::ExcludePos(PCSTR all_pos, long type, long x1, long y1, long x2, long y2) {
    auto *p = P(impl);
    if (!p) return "";
    const auto items = ParsePosListCompat(all_pos, type);
    std::vector<PosEntryCompat> kept;
    kept.reserve(items.size());
    for (const auto &item : items) {
        const bool inside =
            item.x >= x1 && item.x <= x2 &&
            item.y >= y1 && item.y <= y2;
        if (!inside) kept.push_back(item);
    }
    p->scratch = JoinPosListCompat(kept);
    return p->scratch.c_str();
}

const char *dmsoft::FindNearestPos(PCSTR all_pos, long type, long x, long y) {
    auto *p = P(impl);
    if (!p) return "";
    const auto items = ParsePosListCompat(all_pos, type);
    if (items.empty()) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    const PosEntryCompat *best = nullptr;
    unsigned long long best_distance = std::numeric_limits<unsigned long long>::max();
    for (const auto &item : items) {
        const long long dx = static_cast<long long>(item.x) - x;
        const long long dy = static_cast<long long>(item.y) - y;
        const unsigned long long distance =
            static_cast<unsigned long long>(dx * dx) +
            static_cast<unsigned long long>(dy * dy);
        if (!best || distance < best_distance) {
            best = &item;
            best_distance = distance;
        }
    }
    p->scratch = best ? best->raw : "";
    return p->scratch.c_str();
}

const char *dmsoft::SortPosDistance(PCSTR all_pos, long type, long x, long y) {
    auto *p = P(impl);
    if (!p) return "";
    auto items = ParsePosListCompat(all_pos, type);

    std::stable_sort(items.begin(), items.end(), [x, y](const PosEntryCompat &a, const PosEntryCompat &b) {
        if (x == 65535 && y == 0) return a.x < b.x;
        if (y == 65535 && x == 0) return a.y < b.y;

        const long long adx = static_cast<long long>(a.x) - x;
        const long long ady = static_cast<long long>(a.y) - y;
        const long long bdx = static_cast<long long>(b.x) - x;
        const long long bdy = static_cast<long long>(b.y) - y;
        const unsigned long long da =
            static_cast<unsigned long long>(adx * adx) +
            static_cast<unsigned long long>(ady * ady);
        const unsigned long long db =
            static_cast<unsigned long long>(bdx * bdx) +
            static_cast<unsigned long long>(bdy * bdy);
        return da < db;
    });

    p->scratch = JoinPosListCompat(items);
    return p->scratch.c_str();
}


long dmsoft::GetWordResultCount(PCSTR str) {
    if (!str || !*str) return 0;
    const std::string text(str);
    const size_t first_pipe = text.find('|');
    if (first_pipe == std::string::npos) return 0;

    long count = 1;
    for (size_t i = 0; i < first_pipe; ++i) {
        if (text[i] == ',') ++count;
    }
    return count;
}

long dmsoft::GetWordResultPos(PCSTR str, long index, long *x, long *y) {
    if (!x || !y) return 0;
    *x = -1;
    *y = -1;

    const long count = GetWordResultCount(str);
    if (index >= count) return 0;

    const WordResultCompat parsed = ParseWordResultCompat(str);
    if (!parsed.has_first_pipe) return 0;

    if (index >= 0) {
        if (static_cast<size_t>(index) < parsed.xs.size())
            ParseDecimalFieldCompat(parsed.xs[static_cast<size_t>(index)], *x);
        if (static_cast<size_t>(index) < parsed.ys.size())
            ParseDecimalFieldCompat(parsed.ys[static_cast<size_t>(index)], *y);
    }

    // The original helper returns success for negative index as long as
    // index < count; x/y remain -1.
    return 1;
}

const char *dmsoft::GetWordResultStr(PCSTR str, long index) {
    auto *p = P(impl);
    if (!p) return "";

    const long count = GetWordResultCount(str);
    if (index >= count) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    const WordResultCompat parsed = ParseWordResultCompat(str);
    if (!parsed.has_second_pipe || index < 0 ||
        static_cast<size_t>(index) >= parsed.words.size()) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    p->scratch = parsed.words[static_cast<size_t>(index)];
    return p->scratch.c_str();
}

long dmsoft::WaitKey(long key_code, long time_out) {
    const DWORD start = ::GetTickCount();

    if (key_code == 0) {
        for (;;) {
            if (time_out > 0 &&
                static_cast<DWORD>(::GetTickCount() - start) > static_cast<DWORD>(time_out))
                return 0;

            if (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) return VK_LBUTTON;
            if (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) return VK_RBUTTON;
            if (::GetAsyncKeyState(VK_MBUTTON) & 0x8000) return VK_MBUTTON;
            for (long vk = 8; vk < 255; ++vk) {
                if (::GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)
                    return vk;
            }
            ::Sleep(1);
        }
    }

    for (;;) {
        if (time_out > 0 &&
            static_cast<DWORD>(::GetTickCount() - start) > static_cast<DWORD>(time_out))
            return 0;
        if (::GetAsyncKeyState(static_cast<int>(key_code)) & 0x8000)
            return 1;
        ::Sleep(1);
    }
}

long dmsoft::SetKeypadDelay(PCSTR type, long delay) {
    auto *p = P(impl);
    if (!p || !type || delay < 0) return 0;
    if (_stricmp(type, "normal") == 0) p->keypad_delay_normal = delay;
    else if (_stricmp(type, "windows") == 0) p->keypad_delay_windows = delay;
    else if (_stricmp(type, "dx") == 0) p->keypad_delay_dx = delay;
    else return 0;
    return 1;
}

long dmsoft::SetMouseDelay(PCSTR type, long delay) {
    auto *p = P(impl);
    if (!p || !type || delay < 0) return 0;
    if (_stricmp(type, "normal") == 0) p->mouse_delay_normal = delay;
    else if (_stricmp(type, "windows") == 0) p->mouse_delay_windows = delay;
    else if (_stricmp(type, "dx") == 0) p->mouse_delay_dx = delay;
    else return 0;
    return 1;
}

long dmsoft::KeyDown(long vk) {
    return SendKeyboardVkForObjectCompat(P(impl), vk, false);
}

long dmsoft::KeyUp(long vk) {
    return SendKeyboardVkForObjectCompat(P(impl), vk, true);
}

long dmsoft::KeyPress(long vk) {
    auto *p = P(impl);
    if (!p) return 0;
    if (!SendKeyboardVkForObjectCompat(p, vk, false)) return 0;
    const auto b = BindingSnapshotForObjectCompat(p);
    const long delay =
        BoundInputEnabledCompat(b) &&
        _stricmp(b.keypad.c_str(), "windows") == 0
            ? p->keypad_delay_windows
            : p->keypad_delay_normal;
    ::Sleep(static_cast<DWORD>(std::max<long>(0, delay)));
    return SendKeyboardVkForObjectCompat(p, vk, true);
}

long dmsoft::LeftDown() {
    return SendMouseButtonForObjectCompat(
        P(impl), WM_LBUTTONDOWN,
        MOUSEEVENTF_LEFTDOWN, MK_LBUTTON);
}

long dmsoft::LeftUp() {
    return SendMouseButtonForObjectCompat(
        P(impl), WM_LBUTTONUP,
        MOUSEEVENTF_LEFTUP, 0);
}

long dmsoft::RightDown() {
    return SendMouseButtonForObjectCompat(
        P(impl), WM_RBUTTONDOWN,
        MOUSEEVENTF_RIGHTDOWN, MK_RBUTTON);
}

long dmsoft::RightUp() {
    return SendMouseButtonForObjectCompat(
        P(impl), WM_RBUTTONUP,
        MOUSEEVENTF_RIGHTUP, 0);
}

long dmsoft::MiddleDown() {
    return SendMouseButtonForObjectCompat(
        P(impl), WM_MBUTTONDOWN,
        MOUSEEVENTF_MIDDLEDOWN, MK_MBUTTON);
}

long dmsoft::MiddleUp() {
    return SendMouseButtonForObjectCompat(
        P(impl), WM_MBUTTONUP,
        MOUSEEVENTF_MIDDLEUP, 0);
}

long dmsoft::LeftClick() {
    auto *p = P(impl);
    if (!p) return 0;
    if (!LeftDown()) return 0;
    ::Sleep(static_cast<DWORD>(std::max<long>(0, (BoundInputEnabledCompat(BindingSnapshotForObjectCompat(p)) &&
          (_stricmp(BindingSnapshotForObjectCompat(p).mouse.c_str(), "windows") == 0 ||
           _stricmp(BindingSnapshotForObjectCompat(p).mouse.c_str(), "windows3") == 0)
              ? p->mouse_delay_windows : p->mouse_delay_normal))));
    return LeftUp();
}

long dmsoft::RightClick() {
    auto *p = P(impl);
    if (!p) return 0;
    if (!RightDown()) return 0;
    ::Sleep(static_cast<DWORD>(std::max<long>(0, (BoundInputEnabledCompat(BindingSnapshotForObjectCompat(p)) &&
          (_stricmp(BindingSnapshotForObjectCompat(p).mouse.c_str(), "windows") == 0 ||
           _stricmp(BindingSnapshotForObjectCompat(p).mouse.c_str(), "windows3") == 0)
              ? p->mouse_delay_windows : p->mouse_delay_normal))));
    return RightUp();
}

long dmsoft::MiddleClick() {
    auto *p = P(impl);
    if (!p) return 0;
    if (!MiddleDown()) return 0;
    ::Sleep(static_cast<DWORD>(std::max<long>(0, (BoundInputEnabledCompat(BindingSnapshotForObjectCompat(p)) &&
          (_stricmp(BindingSnapshotForObjectCompat(p).mouse.c_str(), "windows") == 0 ||
           _stricmp(BindingSnapshotForObjectCompat(p).mouse.c_str(), "windows3") == 0)
              ? p->mouse_delay_windows : p->mouse_delay_normal))));
    return MiddleUp();
}

long dmsoft::LeftDoubleClick() {
    auto *p = P(impl);
    if (!p) return 0;
    if (!LeftClick()) return 0;
    ::Sleep(static_cast<DWORD>(std::max<long>(0, (BoundInputEnabledCompat(BindingSnapshotForObjectCompat(p)) &&
          (_stricmp(BindingSnapshotForObjectCompat(p).mouse.c_str(), "windows") == 0 ||
           _stricmp(BindingSnapshotForObjectCompat(p).mouse.c_str(), "windows3") == 0)
              ? p->mouse_delay_windows : p->mouse_delay_normal))));
    return LeftClick();
}

long dmsoft::WheelDown() {
    return SendMouseWheelForObjectCompat(P(impl), -WHEEL_DELTA);
}

long dmsoft::WheelUp() {
    return SendMouseWheelForObjectCompat(P(impl), WHEEL_DELTA);
}

long dmsoft::MoveTo(long x, long y) {
    return MoveMouseForObjectCompat(P(impl), x, y);
}

long dmsoft::MoveR(long rx, long ry) {
    return MoveMouseRelativeForObjectCompat(P(impl), rx, ry);
}

const char *dmsoft::MoveToEx(long x, long y, long w, long h) {
    auto *p = P(impl);
    if (!p) return "";

    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }

    static thread_local std::mt19937 rng{std::random_device{}()};
    const long tx = x + (w > 0 ? std::uniform_int_distribution<long>(0, w)(rng) : 0);
    const long ty = y + (h > 0 ? std::uniform_int_distribution<long>(0, h)(rng) : 0);

    if (!MoveTo(tx, ty)) {
        p->scratch.clear();
        return p->scratch.c_str();
    }
    p->scratch = std::to_string(tx) + "," + std::to_string(ty);
    return p->scratch.c_str();
}


long dmsoft::KeyDownChar(PCSTR key_str) {
    const long vk = KeyNameToVkCompat(key_str);
    return vk ? KeyDown(vk) : 0;
}

long dmsoft::KeyUpChar(PCSTR key_str) {
    const long vk = KeyNameToVkCompat(key_str);
    return vk ? KeyUp(vk) : 0;
}

long dmsoft::KeyPressChar(PCSTR key_str) {
    const long vk = KeyNameToVkCompat(key_str);
    return vk ? KeyPress(vk) : 0;
}

long dmsoft::KeyPressStr(PCSTR key_str, long delay) {
    auto *p = P(impl);
    if (!p || !key_str || delay < 0) return 0;

    const unsigned char *cur =
        reinterpret_cast<const unsigned char *>(key_str);
    if (!*cur) return 1;

    while (*cur) {
        // KeyPressStr is character-oriented. Multibyte ACP characters are
        // not representable by VkKeyScanA and therefore fail here, matching
        // the normal keyboard path's inability to type arbitrary CJK text.
        if (!KeyPressCharacterCompat(p, *cur)) return 0;
        ++cur;
        if (*cur && delay > 0)
            ::Sleep(static_cast<DWORD>(delay));
    }
    return 1;
}

long dmsoft::SendPaste(long hwnd) {
    return SendPasteCompat(hwnd);
}


long dmsoft::SendString(long hwnd, PCSTR str) {
    return SendStringAnsiCompat(hwnd, str);
}


const char *dmsoft::GetDir(long type) {
    auto *p = P(impl);
    if (!p) return "";

    std::vector<char> buf(32768, 0);
    switch (type) {
    case 0: {
        const DWORD n = ::GetCurrentDirectoryA(static_cast<DWORD>(buf.size()), buf.data());
        p->scratch = (n > 0 && n < buf.size()) ? std::string(buf.data(), n) : std::string();
        break;
    }
    case 1: {
        const UINT n = ::GetSystemDirectoryA(buf.data(), static_cast<UINT>(buf.size()));
        p->scratch = (n > 0 && n < buf.size()) ? std::string(buf.data(), n) : std::string();
        break;
    }
    case 2: {
        const UINT n = ::GetWindowsDirectoryA(buf.data(), static_cast<UINT>(buf.size()));
        p->scratch = (n > 0 && n < buf.size()) ? std::string(buf.data(), n) : std::string();
        break;
    }
    case 3: {
        const DWORD n = ::GetTempPathA(static_cast<DWORD>(buf.size()), buf.data());
        p->scratch = (n > 0 && n < buf.size()) ? std::string(buf.data(), n) : std::string();
        if (!p->scratch.empty()) {
            while (p->scratch.size() > 3 &&
                   (p->scratch.back() == '\\' || p->scratch.back() == '/'))
                p->scratch.pop_back();
        }
        break;
    }
    case 4:
        p->scratch = ModuleDirectoryCompat(false);
        break;
    default:
        p->scratch.clear();
        break;
    }
    return p->scratch.c_str();
}

long dmsoft::GetOsType() {
    using RtlGetVersionFn = LONG (WINAPI *)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"))
        : nullptr;
    if (!fn) return 0;

    RTL_OSVERSIONINFOEXW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (fn(reinterpret_cast<PRTL_OSVERSIONINFOW>(&vi)) != 0) return 0;

    const DWORD major = vi.dwMajorVersion;
    const DWORD minor = vi.dwMinorVersion;

    if (major < 5) return 0;
    if (major == 5 && (minor == 0 || minor == 1)) return 1;
    if (major == 5 && minor >= 2) return 2;
    if (major == 6 && minor == 0) return 4;
    if (major == 6 && minor == 1) return 3;
    if (major == 6 && minor == 2) return 5;
    if (major == 6 && minor == 3) return 6;
    if (major >= 10) return 7;
    return 0;
}


const char *dmsoft::GetProcessInfo(long pid) {
    auto *p = P(impl);
    if (!p) return "";

    const DWORD process_id = static_cast<DWORD>(pid);
    const std::string path = ProcessImagePathCompat(process_id);
    if (path.empty()) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    HANDLE process = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
        FALSE,
        process_id);
    if (!process) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    long cpu_percent = 0;
    SIZE_T working_set = 0;
    SampleProcessCpuCompat(process, cpu_percent, working_set);
    ::CloseHandle(process);

    std::ostringstream oss;
    oss << BaseNameCompat(path) << '|'
        << path << '|'
        << cpu_percent << '|'
        << static_cast<unsigned long long>(working_set);
    p->scratch = oss.str();
    return p->scratch.c_str();
}

long dmsoft::GetSpecialWindow(long flag) {
    HWND hwnd = nullptr;
    switch (flag) {
    case 0:
        hwnd = ::GetDesktopWindow();
        break;
    case 1:
        hwnd = ::FindWindowA("Shell_TrayWnd", nullptr);
        break;
    default:
        return 0;
    }
    return static_cast<long>(reinterpret_cast<INT_PTR>(hwnd));
}

const char *dmsoft::GetCommandLine(long hwnd) {
    auto *p = P(impl);
    if (!p) return "";
    const DWORD pid = ResolvePid(p, hwnd);
    if (!pid) {
        p->scratch.clear();
        return p->scratch.c_str();
    }
    p->scratch = QueryCommandLineCompat(pid);
    return p->scratch.c_str();
}

const char *dmsoft::GetDiskModel(long index) {
    auto *p = P(impl);
    if (!p) return "";
    DiskDescriptorCompat d{};
    p->scratch = QueryDiskDescriptorCompat(index, d) ? DiskModelCompat(d) : "";
    return p->scratch.c_str();
}

const char *dmsoft::GetDiskReversion(long index) {
    auto *p = P(impl);
    if (!p) return "";
    DiskDescriptorCompat d{};
    p->scratch = QueryDiskDescriptorCompat(index, d) ? d.revision : "";
    return p->scratch.c_str();
}

const char *dmsoft::GetDiskSerial(long index) {
    auto *p = P(impl);
    if (!p) return "";
    DiskDescriptorCompat d{};
    p->scratch = QueryDiskDescriptorCompat(index, d) ? d.serial : "";
    return p->scratch.c_str();
}

const char *dmsoft::GetDisplayInfo() {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch = DisplayInfoCompat();
    return p->scratch.c_str();
}



long dmsoft::EnableGetColorByCapture(long enable) {
    auto *p = P(impl);
    if (!p) return 0;
    p->get_color_by_capture = enable != 0;
    return 1;
}

const char *dmsoft::GetColor(long x, long y) {
    auto *p = P(impl);
    if (!p) return "";
    RgbColorCompat color{};
    p->scratch = ReadScreenPixelCompat(p, x, y, color) ? RgbHexCompat(color) : "";
    return p->scratch.c_str();
}

const char *dmsoft::GetColorBGR(long x, long y) {
    auto *p = P(impl);
    if (!p) return "";
    RgbColorCompat color{};
    p->scratch = ReadScreenPixelCompat(p, x, y, color) ? BgrHexCompat(color) : "";
    return p->scratch.c_str();
}

const char *dmsoft::GetColorHSV(long x, long y) {
    auto *p = P(impl);
    if (!p) return "";
    RgbColorCompat color{};
    p->scratch = ReadScreenPixelCompat(p, x, y, color) ? HsvStringCompat(color) : "";
    return p->scratch.c_str();
}

const char *dmsoft::GetAveRGB(long x1, long y1, long x2, long y2) {
    auto *p = P(impl);
    if (!p) return "";
    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) {
        p->scratch.clear();
        return p->scratch.c_str();
    }
    p->scratch = RgbHexCompat(AverageRgbCompat(image));
    return p->scratch.c_str();
}

const char *dmsoft::GetAveHSV(long x1, long y1, long x2, long y2) {
    auto *p = P(impl);
    if (!p) return "";
    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) {
        p->scratch.clear();
        return p->scratch.c_str();
    }
    p->scratch = HsvStringCompat(AverageRgbCompat(image));
    return p->scratch.c_str();
}

long dmsoft::CmpColor(long x, long y, PCSTR color, double sim) {
    auto *p = P(impl);
    if (!p) return 1;
    ColorSpecCompat spec;
    RgbColorCompat actual{};
    if (!ParseColorSpecCompat(color, spec) ||
        !ReadScreenPixelCompat(p, x, y, actual))
        return 1;
    return MatchColorSpecCompat(actual, spec, sim) ? 0 : 1;
}

long dmsoft::GetColorNum(
    long x1, long y1, long x2, long y2, PCSTR color, double sim) {
    ColorSpecCompat spec;
    if (!ParseColorSpecCompat(color, spec)) return 0;
    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) return 0;

    long count = 0;
    for (const auto &pixel : image.pixels) {
        if (MatchColorSpecCompat(pixel, spec, sim)) {
            if (count == LONG_MAX) return LONG_MAX;
            ++count;
        }
    }
    return count;
}

long dmsoft::FindColor(
    long x1, long y1, long x2, long y2,
    PCSTR color, double sim, long dir, long *x, long *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    if (!x || !y) return 0;

    ColorSpecCompat spec;
    if (!ParseColorSpecCompat(color, spec)) return 0;
    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) return 0;

    long found_x = -1, found_y = -1;
    const bool found = ForEachPointInDirectionCompat(
        x1, y1, x2, y2, dir, [&](long px, long py) {
            const auto *pixel = image.At(px, py);
            if (pixel && MatchColorSpecCompat(*pixel, spec, sim)) {
                found_x = px;
                found_y = py;
                return true;
            }
            return false;
        });
    if (!found) return 0;
    *x = found_x;
    *y = found_y;
    return 1;
}

const char *dmsoft::FindColorE(
    long x1, long y1, long x2, long y2,
    PCSTR color, double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    long x = -1, y = -1;
    FindColor(x1, y1, x2, y2, color, sim, dir, &x, &y);
    p->scratch = std::to_string(x) + "|" + std::to_string(y);
    return p->scratch.c_str();
}

const char *dmsoft::FindColorEx(
    long x1, long y1, long x2, long y2,
    PCSTR color, double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";

    ColorSpecCompat spec;
    ScreenImageCompat image;
    if (!ParseColorSpecCompat(color, spec) ||
        !CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    std::ostringstream oss;
    long count = 0;
    ForEachPointInDirectionCompat(
        x1, y1, x2, y2, dir, [&](long px, long py) {
            const auto *pixel = image.At(px, py);
            if (!pixel || !MatchColorSpecCompat(*pixel, spec, sim)) return false;
            if (count) oss << '|';
            oss << px << ',' << py;
            ++count;
            return count >= 1800;
        });
    p->scratch = oss.str();
    return p->scratch.c_str();
}



long dmsoft::FindMultiColor(
    long x1, long y1, long x2, long y2,
    PCSTR first_color, PCSTR offset_color,
    double sim, long dir, long *x, long *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    if (!x || !y) return 0;

    ColorSpecCompat first{};
    std::vector<MultiColorOffsetCompat> offsets;
    if (!ParseColorSpecCompat(first_color, first) ||
        !ParseMultiColorOffsetsCompat(offset_color, offsets))
        return 0;

    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) return 0;

    long found_x = -1, found_y = -1;
    const bool found = ForEachPointInDirectionCompat(
        x1, y1, x2, y2, dir, [&](long px, long py) {
            if (MatchMultiColorAtCompat(image, px, py, first, offsets, sim)) {
                found_x = px;
                found_y = py;
                return true;
            }
            return false;
        });
    if (!found) return 0;
    *x = found_x;
    *y = found_y;
    return 1;
}

const char *dmsoft::FindMultiColorE(
    long x1, long y1, long x2, long y2,
    PCSTR first_color, PCSTR offset_color,
    double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    long x = -1, y = -1;
    FindMultiColor(
        x1, y1, x2, y2, first_color, offset_color, sim, dir, &x, &y);
    p->scratch = std::to_string(x) + "|" + std::to_string(y);
    return p->scratch.c_str();
}

const char *dmsoft::FindMultiColorEx(
    long x1, long y1, long x2, long y2,
    PCSTR first_color, PCSTR offset_color,
    double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";

    ColorSpecCompat first{};
    std::vector<MultiColorOffsetCompat> offsets;
    ScreenImageCompat image;
    if (!ParseColorSpecCompat(first_color, first) ||
        !ParseMultiColorOffsetsCompat(offset_color, offsets) ||
        !CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    std::ostringstream oss;
    long count = 0;
    ForEachPointInDirectionCompat(
        x1, y1, x2, y2, dir, [&](long px, long py) {
            if (!MatchMultiColorAtCompat(image, px, py, first, offsets, sim))
                return false;
            if (count) oss << '|';
            oss << px << ',' << py;
            ++count;
            return count >= 1800;
        });
    p->scratch = oss.str();
    return p->scratch.c_str();
}

long dmsoft::FindShape(
    long x1, long y1, long x2, long y2,
    PCSTR offset_color, double sim, long dir, long *x, long *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    if (!x || !y) return 0;

    std::vector<ShapeOffsetCompat> offsets;
    if (!ParseShapeOffsetsCompat(offset_color, offsets)) return 0;
    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) return 0;

    long found_x = -1, found_y = -1;
    const bool found = ForEachPointInDirectionCompat(
        x1, y1, x2, y2, dir, [&](long px, long py) {
            if (MatchShapeAtCompat(image, px, py, offsets, sim)) {
                found_x = px;
                found_y = py;
                return true;
            }
            return false;
        });
    if (!found) return 0;
    *x = found_x;
    *y = found_y;
    return 1;
}

const char *dmsoft::FindShapeE(
    long x1, long y1, long x2, long y2,
    PCSTR offset_color, double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    long x = -1, y = -1;
    FindShape(x1, y1, x2, y2, offset_color, sim, dir, &x, &y);
    p->scratch = std::to_string(x) + "|" + std::to_string(y);
    return p->scratch.c_str();
}

const char *dmsoft::FindShapeEx(
    long x1, long y1, long x2, long y2,
    PCSTR offset_color, double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";

    std::vector<ShapeOffsetCompat> offsets;
    ScreenImageCompat image;
    if (!ParseShapeOffsetsCompat(offset_color, offsets) ||
        !CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    std::ostringstream oss;
    long count = 0;
    ForEachPointInDirectionCompat(
        x1, y1, x2, y2, dir, [&](long px, long py) {
            if (!MatchShapeAtCompat(image, px, py, offsets, sim))
                return false;
            if (count) oss << '|';
            oss << px << ',' << py;
            ++count;
            return count >= 1800;
        });
    p->scratch = oss.str();
    return p->scratch.c_str();
}

long dmsoft::FindMulColor(
    long x1, long y1, long x2, long y2, PCSTR color, double sim) {
    if (!color || !*color) return 0;
    std::string source(color);
    bool inverse = false;
    if (!source.empty() && source.front() == '@') {
        inverse = true;
        source.erase(source.begin());
    }

    const auto tokens = SplitCompat(source, '|');
    if (tokens.empty()) return 0;
    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) return 0;

    if (inverse) {
        ColorSpecCompat spec{};
        if (!ParseColorSpecCompat(color, spec)) return 0;
        for (const auto &pixel : image.pixels)
            if (MatchColorSpecCompat(pixel, spec, sim)) return 1;
        return 0;
    }

    for (const auto &token : tokens) {
        ColorSpecCompat one{};
        if (!ParseColorSpecCompat(token.c_str(), one)) return 0;
        bool found = false;
        for (const auto &pixel : image.pixels) {
            if (MatchColorSpecCompat(pixel, one, sim)) {
                found = true;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}



long dmsoft::FindColorBlock(
    long x1, long y1, long x2, long y2,
    PCSTR color, double sim, long count, long width, long height,
    long *x, long *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    if (!x || !y || count < 0 || width <= 0 || height <= 0) return 0;

    ColorSpecCompat spec{};
    ScreenImageCompat image;
    if (!ParseColorSpecCompat(color, spec) ||
        !CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image) ||
        width > image.width || height > image.height)
        return 0;

    const auto integral = BuildColorIntegralCompat(image, spec, sim);
    const long stride = image.width + 1;
    for (long py = 0; py <= image.height - height; ++py) {
        for (long px = 0; px <= image.width - width; ++px) {
            if (IntegralRectCountCompat(
                    integral, stride, px, py, width, height) >= count) {
                *x = x1 + px;
                *y = y1 + py;
                return 1;
            }
        }
    }
    return 0;
}

const char *dmsoft::FindColorBlockEx(
    long x1, long y1, long x2, long y2,
    PCSTR color, double sim, long count, long width, long height) {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch.clear();
    if (count < 0 || width <= 0 || height <= 0) return p->scratch.c_str();

    ColorSpecCompat spec{};
    ScreenImageCompat image;
    if (!ParseColorSpecCompat(color, spec) ||
        !CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image) ||
        width > image.width || height > image.height)
        return p->scratch.c_str();

    const auto integral = BuildColorIntegralCompat(image, spec, sim);
    const long stride = image.width + 1;
    std::ostringstream oss;
    long found = 0;
    for (long py = 0; py <= image.height - height && found < 1800; ++py) {
        for (long px = 0; px <= image.width - width && found < 1800; ++px) {
            if (IntegralRectCountCompat(
                    integral, stride, px, py, width, height) < count)
                continue;
            if (found) oss << '|';
            oss << (x1 + px) << ',' << (y1 + py);
            ++found;
        }
    }
    p->scratch = oss.str();
    return p->scratch.c_str();
}

long dmsoft::Capture(
    long x1, long y1, long x2, long y2, PCSTR file) {
    auto *p = P(impl);
    if (!p) return 0;
    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) return 0;
    const auto path = ResolveObjectFilePathCompat(p, file);
    if (path.empty()) return 0;
    return WriteBmp24Compat(path, image) ? 1 : 0;
}


long dmsoft::LockMouseRect(long x1, long y1, long x2, long y2) {
    if (x1 == 0 && y1 == 0 && x2 == 0 && y2 == 0)
        return ::ClipCursor(nullptr) ? 1 : 0;
    RECT rect{static_cast<LONG>(x1), static_cast<LONG>(y1),
              static_cast<LONG>(x2), static_cast<LONG>(y2)};
    return ::ClipCursor(&rect) ? 1 : 0;
}

long dmsoft::ExitOs(long type) {
    UINT flags = 0;
    switch (type) {
    case 0: flags = EWX_LOGOFF; break;
    case 1: flags = EWX_SHUTDOWN | EWX_POWEROFF; break;
    case 2: flags = EWX_REBOOT; break;
    default: return 0;
    }
    if (type != 0 && !EnableShutdownPrivilegeCompat()) return 0;
    return ::ExitWindowsEx(
        flags, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER) ? 1 : 0;
}

long dmsoft::SetUAC(long uac) {
    if (uac != 0 && uac != 1) return 0;
    HKEY key = nullptr;
    const LSTATUS open = ::RegOpenKeyExA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
        0, KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
    if (open != ERROR_SUCCESS) return 0;
    const DWORD value = static_cast<DWORD>(uac);
    const LSTATUS set = ::RegSetValueExA(
        key, "EnableLUA", 0, REG_DWORD,
        reinterpret_cast<const BYTE *>(&value), sizeof(value));
    ::RegCloseKey(key);
    return set == ERROR_SUCCESS ? 1 : 0;
}

long dmsoft::SetScreen(long width, long height, long depth) {
    if (width <= 0 || height <= 0 || depth <= 0) return 0;
    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    mode.dmPelsWidth = static_cast<DWORD>(width);
    mode.dmPelsHeight = static_cast<DWORD>(height);
    mode.dmBitsPerPel = static_cast<DWORD>(depth);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    return ::ChangeDisplaySettingsA(&mode, CDS_UPDATEREGISTRY) ==
           DISP_CHANGE_SUCCESSFUL ? 1 : 0;
}

long dmsoft::ShowTaskBarIcon(long hwnd, long is_show) {
    HWND h = HwndFromLong(hwnd);
    if (!::IsWindow(h) || (is_show != 0 && is_show != 1)) return 0;

    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(init);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return 0;

    ITaskbarList *taskbar = nullptr;
    HRESULT hr = ::CoCreateInstance(
        CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITaskbarList, reinterpret_cast<void **>(&taskbar));
    if (SUCCEEDED(hr) && taskbar) {
        hr = taskbar->HrInit();
        if (SUCCEEDED(hr))
            hr = is_show ? taskbar->AddTab(h) : taskbar->DeleteTab(h);
        taskbar->Release();
    }
    if (uninit) ::CoUninitialize();
    return SUCCEEDED(hr) ? 1 : 0;
}

const char *dmsoft::SelectDirectory() {
    auto *p = P(impl);
    if (!p) return "";
    BROWSEINFOA bi{};
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST item = ::SHBrowseForFolderA(&bi);
    if (!item) {
        p->scratch.clear();
        return p->scratch.c_str();
    }
    char path[MAX_PATH]{};
    const BOOL ok = ::SHGetPathFromIDListA(item, path);
    ::CoTaskMemFree(item);
    p->scratch = ok ? path : "";
    return p->scratch.c_str();
}

const char *dmsoft::SelectFile() {
    auto *p = P(impl);
    if (!p) return "";
    char file[32768]{};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(file));
    ofn.lpstrFilter = "All Files\0*.*\0\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    p->scratch = ::GetOpenFileNameA(&ofn) ? file : "";
    return p->scratch.c_str();
}

const char *dmsoft::ExecuteCmd(PCSTR cmd, PCSTR current_dir, long time_out) {
    auto *p = P(impl);
    if (!p) return "";
    p->scratch = ExecuteCmdCompat(cmd, current_dir, time_out);
    return p->scratch.c_str();
}

long dmsoft::DownloadFile(PCSTR url, PCSTR save_file, long timeout) {
    return DownloadFileCompat(url, save_file, timeout);
}


long dmsoft::Play(PCSTR file) {
    auto *p = P(impl);
    if (!p || !file || !*file) return 0;

    const std::string path = ResolveObjectPathCompat(p, file);
    if (path.empty() || !std::filesystem::is_regular_file(path)) return 0;

    long id = 0;
    std::string alias;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        for (;;) {
            id = p->next_play_id++;
            if (p->next_play_id <= 0) p->next_play_id = 1;
            if (id > 0 && p->play_aliases.find(id) == p->play_aliases.end())
                break;
        }
        alias = "hcbyj_" + std::to_string(p->id) + "_" + std::to_string(id);
    }

    const std::string open =
        "open \"" + path + "\" alias " + alias;
    if (::mciSendStringA(open.c_str(), nullptr, 0, nullptr) != 0)
        return 0;

    const std::string play = "play " + alias;
    if (::mciSendStringA(play.c_str(), nullptr, 0, nullptr) != 0) {
        const std::string close = "close " + alias;
        ::mciSendStringA(close.c_str(), nullptr, 0, nullptr);
        return 0;
    }

    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        p->play_aliases[id] = alias;
    }
    return id;
}

long dmsoft::Stop(long id) {
    auto *p = P(impl);
    if (!p || id <= 0) return 0;

    std::string alias;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        const auto it = p->play_aliases.find(id);
        if (it == p->play_aliases.end()) return 0;
        alias = it->second;
        p->play_aliases.erase(it);
    }

    const std::string stop = "stop " + alias;
    const MCIERROR stop_error =
        ::mciSendStringA(stop.c_str(), nullptr, 0, nullptr);
    const std::string close = "close " + alias;
    const MCIERROR close_error =
        ::mciSendStringA(close.c_str(), nullptr, 0, nullptr);
    return stop_error == 0 && close_error == 0 ? 1 : 0;
}

long dmsoft::SetAero(long enable) {
    if (enable != 0 && enable != 1) return 0;
    const HRESULT hr = ::DwmEnableComposition(
        enable ? DWM_EC_ENABLECOMPOSITION : DWM_EC_DISABLECOMPOSITION);
    return SUCCEEDED(hr) ? 1 : 0;
}


long dmsoft::IsSurrpotVt() {
    return IntelVtEnabledCompat() ? 1 : 0;
}


long dmsoft::SetLocale() {
    const HINSTANCE result = ::ShellExecuteA(
        nullptr, "open", "control.exe", "intl.cpl", nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32 ? 1 : 0;
}


long dmsoft::EnablePicCache(long en) {
    auto *p = P(impl);
    if (!p || (en != 0 && en != 1)) return 0;
    std::lock_guard<std::mutex> lock(p->state_mutex);
    p->pic_cache_enabled = en != 0;
    return 1;
}

const char *dmsoft::MatchPicName(PCSTR pic_name) {
    auto *p = P(impl);
    if (!p) return "";
    const auto refs = ExpandPicRefsCompat(p, pic_name);
    std::ostringstream oss;
    for (size_t i = 0; i < refs.size(); ++i) {
        if (i) oss << '|';
        oss << refs[i].display;
    }
    p->scratch = oss.str();
    return p->scratch.c_str();
}

long dmsoft::LoadPic(PCSTR pic_name) {
    auto *p = P(impl);
    if (!p) return 0;
    const auto refs = ExpandPicRefsCompat(p, pic_name);
    if (refs.empty()) return 0;

    for (const auto &ref : refs) {
        auto image = LoadPicCachedCompat(p, ref.path);
        if (!image) return 0;
        if (p->pic_cache_enabled) {
            std::lock_guard<std::mutex> lock(p->state_mutex);
            p->pic_cache[LowerPathKeyCompat(ref.path)] = image;
        }
    }
    return 1;
}

long dmsoft::FreePic(PCSTR pic_name) {
    auto *p = P(impl);
    if (!p || !pic_name || !*pic_name) return 0;

    std::lock_guard<std::mutex> lock(p->state_mutex);
    const std::string request(pic_name);
    if (request == "*" || request == "*.*") {
        p->pic_cache.clear();
        p->memory_pic_cache.clear();
        return 1;
    }

    long removed = 0;
    for (const auto &token : SplitCompat(request, '|')) {
        if (token.empty()) continue;

        if (!HasWildcardCompat(token)) {
            const auto full = ResolveObjectFilePathCompat(p, token.c_str());
            removed += static_cast<long>(
                p->pic_cache.erase(LowerPathKeyCompat(full)));
            continue;
        }

        const auto slash = token.find_last_of("\\/");
        const std::string pattern =
            slash == std::string::npos ? token : token.substr(slash + 1);
        const std::filesystem::path base =
            slash == std::string::npos
                ? std::filesystem::path(p->global_path)
                : ResolveObjectFilePathCompat(
                      p, token.substr(0, slash).c_str());

        std::vector<std::string> erase_keys;
        for (const auto &entry : p->pic_cache) {
            const std::filesystem::path cached(entry.first);
            if (!base.empty()) {
                std::error_code ec;
                const auto parent_abs =
                    std::filesystem::absolute(cached.parent_path(), ec)
                        .lexically_normal();
                const auto base_abs =
                    std::filesystem::absolute(base, ec).lexically_normal();
                if (!ec && parent_abs != base_abs) continue;
            }

            WIN32_FIND_DATAA fd{};
            const std::string probe =
                (cached.parent_path() / pattern).string();
            HANDLE h = ::FindFirstFileA(probe.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            bool matched = false;
            do {
                if (_stricmp(
                        fd.cFileName,
                        cached.filename().string().c_str()) == 0) {
                    matched = true;
                    break;
                }
            } while (::FindNextFileA(h, &fd));
            ::FindClose(h);
            if (matched) erase_keys.push_back(entry.first);
        }
        for (const auto &key : erase_keys) {
            removed += static_cast<long>(p->pic_cache.erase(key));
        }
    }
    return removed > 0 ? 1 : 0;
}

const char *dmsoft::GetPicSize(PCSTR pic_name) {
    auto *p = P(impl);
    if (!p) return "";
    const auto refs = ExpandPicRefsCompat(p, pic_name);
    if (refs.empty()) {
        p->scratch.clear();
        return p->scratch.c_str();
    }
    auto image = LoadPicCachedCompat(p, refs.front().path);
    if (!image) {
        p->scratch.clear();
        return p->scratch.c_str();
    }
    p->scratch =
        std::to_string(image->width) + "," + std::to_string(image->height);
    return p->scratch.c_str();
}

long dmsoft::FindPic(
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    double sim, long dir, long *x, long *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    if (!x || !y) return -1;

    auto *p = P(impl);
    if (!p) return -1;
    const long threshold = static_cast<long>(
        std::ceil(std::clamp(sim, 0.0, 1.0) * 100.0));
    const auto found = FindPicFirstCompat(
        p, x1, y1, x2, y2, pic_name, delta_color,
        threshold, dir);
    if (!found) return -1;

    *x = found->x;
    *y = found->y;
    return found->index;
}

const char *dmsoft::FindPicE(
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    long x = -1, y = -1;
    const long index = FindPic(
        x1, y1, x2, y2, pic_name, delta_color,
        sim, dir, &x, &y);
    p->scratch =
        std::to_string(index) + "|" +
        std::to_string(x) + "|" + std::to_string(y);
    return p->scratch.c_str();
}

const char *dmsoft::FindPicEx(
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    const long threshold = static_cast<long>(
        std::ceil(std::clamp(sim, 0.0, 1.0) * 100.0));
    const auto matches = FindPicsAllCompat(
        p, x1, y1, x2, y2, pic_name, delta_color,
        threshold, dir, 1500);

    std::ostringstream oss;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i) oss << '|';
        oss << matches[i].index << ','
            << matches[i].x << ',' << matches[i].y;
    }
    p->scratch = oss.str();
    return p->scratch.c_str();
}

const char *dmsoft::FindPicS(
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    double sim, long dir, long *x, long *y) {
    auto *p = P(impl);
    if (x) *x = -1;
    if (y) *y = -1;
    if (!p || !x || !y) return "";

    const long threshold = static_cast<long>(
        std::ceil(std::clamp(sim, 0.0, 1.0) * 100.0));
    const auto found = FindPicFirstCompat(
        p, x1, y1, x2, y2, pic_name, delta_color,
        threshold, dir);
    if (!found) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    *x = found->x;
    *y = found->y;
    p->scratch = found->display;
    return p->scratch.c_str();
}

const char *dmsoft::FindPicExS(
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    const long threshold = static_cast<long>(
        std::ceil(std::clamp(sim, 0.0, 1.0) * 100.0));
    const auto matches = FindPicsAllCompat(
        p, x1, y1, x2, y2, pic_name, delta_color,
        threshold, dir, 1500);

    std::ostringstream oss;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i) oss << '|';
        oss << matches[i].display << ','
            << matches[i].x << ',' << matches[i].y;
    }
    p->scratch = oss.str();
    return p->scratch.c_str();
}

long dmsoft::FindPicSim(
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    long sim, long dir, long *x, long *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    if (!x || !y || sim < 0 || sim > 100) return -1;

    auto *p = P(impl);
    if (!p) return -1;
    const auto found = FindPicFirstCompat(
        p, x1, y1, x2, y2, pic_name, delta_color,
        sim, dir);
    if (!found) return -1;
    *x = found->x;
    *y = found->y;
    return found->index;
}

const char *dmsoft::FindPicSimE(
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    long sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    long x = -1, y = -1;
    const long index = FindPicSim(
        x1, y1, x2, y2, pic_name, delta_color,
        sim, dir, &x, &y);
    p->scratch =
        std::to_string(index) + "|" +
        std::to_string(x) + "|" + std::to_string(y);
    return p->scratch.c_str();
}

const char *dmsoft::FindPicSimEx(
    long x1, long y1, long x2, long y2,
    PCSTR pic_name, PCSTR delta_color,
    long sim, long dir) {
    auto *p = P(impl);
    if (!p || sim < 0 || sim > 100) return "";
    const auto matches = FindPicsAllCompat(
        p, x1, y1, x2, y2, pic_name, delta_color,
        sim, dir, 1500);

    std::ostringstream oss;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i) oss << '|';
        oss << matches[i].index << ','
            << matches[i].score << ','
            << matches[i].x << ',' << matches[i].y;
    }
    p->scratch = oss.str();
    return p->scratch.c_str();
}


long dmsoft::CapturePng(
    long x1, long y1, long x2, long y2, PCSTR file) {
    auto *p = P(impl);
    if (!p) return 0;
    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) return 0;
    const auto path = ResolveObjectFilePathCompat(p, file);
    if (path.empty()) return 0;
    return SaveScreenImageEncodedCompat(
        path, image, L"image/png", 100) ? 1 : 0;
}


long dmsoft::CaptureJpg(
    long x1, long y1, long x2, long y2,
    PCSTR file, long quality) {
    auto *p = P(impl);
    if (!p || quality < 1 || quality > 100) return 0;
    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(P(impl), x1, y1, x2, y2, image)) return 0;
    const auto path = ResolveObjectFilePathCompat(p, file);
    if (path.empty()) return 0;
    return SaveScreenImageEncodedCompat(
        path, image, L"image/jpeg", quality) ? 1 : 0;
}


long dmsoft::ImageToBmp(PCSTR pic_name, PCSTR bmp_name) {
    auto *p = P(impl);
    if (!p || !pic_name || !bmp_name) return 0;
    const auto source = ResolveObjectFilePathCompat(p, pic_name);
    const auto dest = ResolveObjectFilePathCompat(p, bmp_name);
    if (source.empty() || dest.empty()) return 0;
    return ConvertImageToBmp24Compat(source, dest) ? 1 : 0;
}


long dmsoft::EnableDisplayDebug(long enable_debug) {
    auto *p = P(impl);
    if (!p || (enable_debug != 0 && enable_debug != 1)) return 0;
    p->display_debug_enabled = enable_debug != 0;
    return 1;
}


long dmsoft::CapturePre(PCSTR file) {
    auto *p = P(impl);
    if (!p || !p->display_debug_enabled || !file || !*file ||
        !g_last_graphic_capture)
        return 0;
    const auto path = ResolveObjectFilePathCompat(p, file);
    if (path.empty()) return 0;
    return WriteBmp24Compat(path, *g_last_graphic_capture) ? 1 : 0;
}


long dmsoft::SetDisplayDelay(long t) {
    auto *p = P(impl);
    if (!p || t < 0) return 0;
    p->display_delay = t;
    return 1;
}


long dmsoft::SetDisplayRefreshDelay(long t) {
    auto *p = P(impl);
    if (!p || t < 0) return 0;
    p->display_refresh_delay = t;
    return 1;
}


long dmsoft::SetShowAsmErrorMsg(long show) {
    auto *p = P(impl);
    if (!p || (show != 0 && show != 1)) return 0;
    p->show_asm_error_msg = show != 0;
    return 1;
}


long dmsoft::EnableFindPicMultithread(long en) {
    auto *p = P(impl);
    if (!p || (en != 0 && en != 1)) return 0;
    p->find_pic_multithread_enabled = en != 0;
    return 1;
}


long dmsoft::SetFindPicMultithreadCount(long count) {
    auto *p = P(impl);
    if (!p || count <= 0) return 0;
    p->find_pic_multithread_count = count;
    return 1;
}


long dmsoft::SetFindPicMultithreadLimit(long limit) {
    auto *p = P(impl);
    if (!p || limit <= 0) return 0;
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    if (static_cast<unsigned long>(limit) > cores) return 0;
    p->find_pic_multithread_limit = limit;
    return 1;
}


long dmsoft::UseDict(long index) {
    auto *p = P(impl);
    if (!p || index < 0 || index > 99) return 0;
    p->current_dict = index;
    return 1;
}

long dmsoft::GetNowDict() {
    auto *p = P(impl);
    return p ? p->current_dict : 0;
}

long dmsoft::EnableShareDict(long en) {
    auto *p = P(impl);
    if (!p || (en != 0 && en != 1)) return 0;
    p->share_dict_enabled = en != 0;
    return 1;
}

long dmsoft::SetExactOcr(long exact_ocr) {
    auto *p = P(impl);
    if (!p || (exact_ocr != 0 && exact_ocr != 1)) return 0;
    p->exact_ocr_enabled = exact_ocr != 0;
    return 1;
}

long dmsoft::SetMinRowGap(long row_gap) {
    auto *p = P(impl);
    if (!p) return 0;
    p->min_row_gap = row_gap;
    return 1;
}

long dmsoft::SetMinColGap(long col_gap) {
    auto *p = P(impl);
    if (!p) return 0;
    p->min_col_gap = col_gap;
    return 1;
}

long dmsoft::SetWordGap(long word_gap) {
    auto *p = P(impl);
    if (!p) return 0;
    p->word_gap = word_gap;
    return 1;
}

long dmsoft::SetWordLineHeight(long line_height) {
    auto *p = P(impl);
    if (!p) return 0;
    p->word_line_height = line_height;
    return 1;
}

long dmsoft::SetRowGapNoDict(long row_gap) {
    auto *p = P(impl);
    if (!p) return 0;
    p->nodict_row_gap = row_gap;
    return 1;
}

long dmsoft::SetColGapNoDict(long col_gap) {
    auto *p = P(impl);
    if (!p) return 0;
    p->nodict_col_gap = col_gap;
    return 1;
}

long dmsoft::SetWordGapNoDict(long word_gap) {
    auto *p = P(impl);
    if (!p) return 0;
    p->nodict_word_gap = word_gap;
    return 1;
}

long dmsoft::SetWordLineHeightNoDict(long line_height) {
    auto *p = P(impl);
    if (!p) return 0;
    p->nodict_word_line_height = line_height;
    return 1;
}


long dmsoft::InitCri() {
    std::lock_guard<std::mutex> lock(g_cri_mutex);
    g_cri_owner = nullptr;
    return 1;
}

long dmsoft::EnterCri() {
    auto *p = P(impl);
    if (!p) return 0;
    std::lock_guard<std::mutex> lock(g_cri_mutex);
    if (g_cri_owner != nullptr) return 0;
    g_cri_owner = p;
    return 1;
}

long dmsoft::LeaveCri() {
    auto *p = P(impl);
    if (!p) return 0;
    std::lock_guard<std::mutex> lock(g_cri_mutex);
    if (g_cri_owner == p) g_cri_owner = nullptr;
    return 1;
}

long dmsoft::SetPicPwd(PCSTR pwd) {
    auto *p = P(impl);
    if (!p || !pwd) return 0;
    p->pic_password = pwd;
    return 1;
}

long dmsoft::SetDictPwd(PCSTR pwd) {
    auto *p = P(impl);
    if (!p || !pwd) return 0;
    p->dict_password = pwd;
    return 1;
}

long dmsoft::SetParam64ToPointer() {
    auto *p = P(impl);
    if (!p) return 0;
    p->param64_to_pointer = true;
    return 1;
}


long dmsoft::SetMemoryFindResultToFile(PCSTR file) {
    auto *p = P(impl);
    if (!p || !file) return 0;
    if (!*file) {
        p->memory_find_result_file.clear();
        return 1;
    }
    const std::string resolved = ResolveMemoryResultFileCompat(p, file);
    if (resolved.empty()) return 0;
    p->memory_find_result_file = resolved;
    return 1;
}

const char *dmsoft::FindIntEx(
    long hwnd, PCSTR addr_range,
    LONGLONG int_value_min, LONGLONG int_value_max,
    long type, long step, long multi_thread, long mode) {

    auto *p = P(impl);
    if (!p) return "";
    (void)multi_thread;

    if (int_value_min > int_value_max) {
        p->scratch.clear();
        SetNativeError(p, ERROR_INVALID_PARAMETER);
        return p->scratch.c_str();
    }

    size_t size = 0;
    switch (type) {
    case 0: size = sizeof(std::int32_t); break;
    case 1: size = sizeof(std::int16_t); break;
    case 2: size = sizeof(std::int8_t); break;
    case 3: size = sizeof(std::int64_t); break;
    default:
        p->scratch.clear();
        SetNativeError(p, ERROR_INVALID_PARAMETER);
        return p->scratch.c_str();
    }

    auto match = [=](const unsigned char *data) -> bool {
        LONGLONG value = 0;
        switch (type) {
        case 0: {
            std::int32_t v = 0;
            std::memcpy(&v, data, sizeof(v));
            value = v;
            break;
        }
        case 1: {
            std::int16_t v = 0;
            std::memcpy(&v, data, sizeof(v));
            value = v;
            break;
        }
        case 2: {
            std::int8_t v = 0;
            std::memcpy(&v, data, sizeof(v));
            value = v;
            break;
        }
        case 3: {
            std::int64_t v = 0;
            std::memcpy(&v, data, sizeof(v));
            value = v;
            break;
        }
        }
        return value >= int_value_min && value <= int_value_max;
    };

    const auto results = ScanMemoryCompat(
        p, hwnd, addr_range, size, step, mode, match);
    p->scratch = FinalizeMemoryFindCompat(p, results);
    return p->scratch.c_str();
}

const char *dmsoft::FindInt(
    long hwnd, PCSTR addr_range,
    LONGLONG int_value_min, LONGLONG int_value_max, long type) {
    return FindIntEx(
        hwnd, addr_range, int_value_min, int_value_max,
        type, 1, 1, 0);
}

const char *dmsoft::FindFloatEx(
    long hwnd, PCSTR addr_range,
    float float_value_min, float float_value_max,
    long step, long multi_thread, long mode) {

    auto *p = P(impl);
    if (!p) return "";
    (void)multi_thread;
    if (!(float_value_min <= float_value_max)) {
        p->scratch.clear();
        SetNativeError(p, ERROR_INVALID_PARAMETER);
        return p->scratch.c_str();
    }

    auto match = [=](const unsigned char *data) {
        float value = 0.0f;
        std::memcpy(&value, data, sizeof(value));
        return !std::isnan(value) &&
               value >= float_value_min &&
               value <= float_value_max;
    };

    const auto results = ScanMemoryCompat(
        p, hwnd, addr_range, sizeof(float), step, mode, match);
    p->scratch = FinalizeMemoryFindCompat(p, results);
    return p->scratch.c_str();
}

const char *dmsoft::FindFloat(
    long hwnd, PCSTR addr_range,
    float float_value_min, float float_value_max) {
    return FindFloatEx(
        hwnd, addr_range, float_value_min, float_value_max,
        1, 1, 0);
}

const char *dmsoft::FindDoubleEx(
    long hwnd, PCSTR addr_range,
    double double_value_min, double double_value_max,
    long step, long multi_thread, long mode) {

    auto *p = P(impl);
    if (!p) return "";
    (void)multi_thread;
    if (!(double_value_min <= double_value_max)) {
        p->scratch.clear();
        SetNativeError(p, ERROR_INVALID_PARAMETER);
        return p->scratch.c_str();
    }

    auto match = [=](const unsigned char *data) {
        double value = 0.0;
        std::memcpy(&value, data, sizeof(value));
        return !std::isnan(value) &&
               value >= double_value_min &&
               value <= double_value_max;
    };

    const auto results = ScanMemoryCompat(
        p, hwnd, addr_range, sizeof(double), step, mode, match);
    p->scratch = FinalizeMemoryFindCompat(p, results);
    return p->scratch.c_str();
}

const char *dmsoft::FindDouble(
    long hwnd, PCSTR addr_range,
    double double_value_min, double double_value_max) {
    return FindDoubleEx(
        hwnd, addr_range, double_value_min, double_value_max,
        1, 1, 0);
}

const char *dmsoft::FindDataEx(
    long hwnd, PCSTR addr_range, PCSTR data,
    long step, long multi_thread, long mode) {

    auto *p = P(impl);
    if (!p) return "";
    (void)multi_thread;

    MemoryBytePatternCompat pattern;
    if (!ParseMemoryPatternCompat(data, pattern)) {
        p->scratch.clear();
        SetNativeError(p, ERROR_INVALID_DATA);
        return p->scratch.c_str();
    }

    auto match = [&pattern](const unsigned char *value) {
        return pattern.Match(value);
    };
    const auto results = ScanMemoryCompat(
        p, hwnd, addr_range, pattern.size(), step, mode, match);
    p->scratch = FinalizeMemoryFindCompat(p, results);
    return p->scratch.c_str();
}

const char *dmsoft::FindData(
    long hwnd, PCSTR addr_range, PCSTR data) {
    return FindDataEx(hwnd, addr_range, data, 1, 1, 0);
}

const char *dmsoft::FindStringEx(
    long hwnd, PCSTR addr_range, PCSTR string_value,
    long type, long step, long multi_thread, long mode) {

    auto *p = P(impl);
    if (!p || !string_value) return "";
    (void)multi_thread;

    std::vector<unsigned char> pattern;
    if (type == 0) {
        const size_t n = std::strlen(string_value);
        pattern.assign(
            reinterpret_cast<const unsigned char *>(string_value),
            reinterpret_cast<const unsigned char *>(string_value) + n);
    } else if (type == 1) {
        std::wstring wide = AcpToWideCompat(string_value);
        if (!wide.empty() && wide.back() == L'\0') wide.pop_back();
        const auto *begin =
            reinterpret_cast<const unsigned char *>(wide.data());
        pattern.assign(
            begin, begin + wide.size() * sizeof(wchar_t));
    } else if (type == 2) {
        const std::string utf8 = AcpToUtf8Compat(string_value);
        pattern.assign(
            reinterpret_cast<const unsigned char *>(utf8.data()),
            reinterpret_cast<const unsigned char *>(utf8.data()) +
                utf8.size());
    } else {
        p->scratch.clear();
        SetNativeError(p, ERROR_INVALID_PARAMETER);
        return p->scratch.c_str();
    }

    if (pattern.empty()) {
        p->scratch.clear();
        SetNativeError(p, ERROR_INVALID_PARAMETER);
        return p->scratch.c_str();
    }

    auto match = [&pattern](const unsigned char *value) {
        return std::memcmp(value, pattern.data(), pattern.size()) == 0;
    };
    const auto results = ScanMemoryCompat(
        p, hwnd, addr_range, pattern.size(), step, mode, match);
    p->scratch = FinalizeMemoryFindCompat(p, results);
    return p->scratch.c_str();
}

const char *dmsoft::FindString(
    long hwnd, PCSTR addr_range, PCSTR string_value, long type) {
    return FindStringEx(
        hwnd, addr_range, string_value, type, 1, 1, 0);
}


long dmsoft::ReadDataAddrToBin(long hwnd, LONGLONG addr, long len) {
    auto *p = P(impl);
    if (!p || len <= 0) return 0;

    void *buffer = AllocateLegacyBinBufferCompat(
        p, static_cast<SIZE_T>(len));
    if (!buffer) {
        SetNativeError(p, ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    if (!ReadRemoteBytesCompat(
            p, hwnd, addr, buffer, static_cast<SIZE_T>(len))) {
        FreeLegacyBinBufferCompat(p);
        return 0;
    }

    const ULONG_PTR ptr = reinterpret_cast<ULONG_PTR>(buffer);
    if (ptr > static_cast<ULONG_PTR>(LONG_MAX)) {
        FreeLegacyBinBufferCompat(p);
        SetNativeError(p, ERROR_ARITHMETIC_OVERFLOW);
        return 0;
    }
    return static_cast<long>(ptr);
}

long dmsoft::ReadDataToBin(long hwnd, PCSTR addr, long len) {
    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved))
        return 0;
    return ReadDataAddrToBin(hwnd, resolved, len);
}

long dmsoft::WriteDataAddrFromBin(
    long hwnd, LONGLONG addr, long data, long len) {

    auto *p = P(impl);
    if (!p || data == 0 || len <= 0) return 0;

    std::vector<unsigned char> bytes(static_cast<size_t>(len));
    if (!CopyFromLegacyPointerCompat(
            data, bytes.data(), bytes.size())) {
        SetNativeError(p, ERROR_INVALID_ADDRESS);
        return 0;
    }

    return WriteRemoteBytesCompat(
        p, hwnd, addr, bytes.data(), bytes.size()) ? 1 : 0;
}

long dmsoft::WriteDataFromBin(
    long hwnd, PCSTR addr, long data, long len) {

    LONGLONG resolved = 0;
    if (!ResolveAddressExprCompat(P(impl), hwnd, addr, resolved))
        return 0;
    return WriteDataAddrFromBin(hwnd, resolved, data, len);
}


long dmsoft::SetExcludeRegion(long type, PCSTR info) {
    auto *p = P(impl);
    if (!p) return 0;

    if (type == 2) {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        p->exclude_regions.clear();
        return 1;
    }

    if (type == 1) {
        if (!info) return 0;
        const std::string text(info);
        if (text.size() != 6) return 0;
        char *end = nullptr;
        const unsigned long value = std::strtoul(text.c_str(), &end, 16);
        if (!end || *end != '\0' || value > 0xFFFFFFUL) return 0;
        std::lock_guard<std::mutex> lock(p->state_mutex);
        p->exclude_region_rgb = static_cast<unsigned int>(value);
        return 1;
    }

    if (type != 0 || !info || !*info) return 0;

    std::vector<ExcludeRegionCompat> parsed;
    for (const auto &token : SplitCompat(info, '|')) {
        if (token.empty()) continue;
        long x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        char tail = 0;
        if (std::sscanf(
                token.c_str(), "%ld,%ld,%ld,%ld%c",
                &x1, &y1, &x2, &y2, &tail) != 4)
            return 0;
        parsed.push_back({x1, y1, x2, y2});
    }
    if (parsed.empty()) return 0;

    std::lock_guard<std::mutex> lock(p->state_mutex);
    p->exclude_regions.insert(
        p->exclude_regions.end(), parsed.begin(), parsed.end());
    return 1;
}


long dmsoft::LoadPicByte(long addr, long size, PCSTR name) {
    auto *p = P(impl);
    if (!p || !name || !*name || addr == 0 || size <= 0)
        return 0;

    std::vector<unsigned char> bytes;
    if (!CopyLegacyBytesCompat(addr, size, bytes)) return 0;

    auto image = std::make_shared<ScreenImageCompat>();
    if (!LoadBmp24MemoryCompat(bytes.data(), bytes.size(), *image))
        return 0;

    std::string key(name);
    std::transform(
        key.begin(), key.end(), key.begin(),
        [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
    std::lock_guard<std::mutex> lock(p->state_mutex);
    p->memory_pic_cache[key] = std::move(image);
    return 1;
}

const char *dmsoft::AppendPicAddr(
    PCSTR pic_info, long addr, long size) {
    auto *p = P(impl);
    if (!p || addr == 0 || size <= 0) return "";

    std::ostringstream oss;
    if (pic_info && *pic_info)
        oss << pic_info << '|';
    oss << static_cast<unsigned long>(
               static_cast<unsigned long>(addr))
        << ',' << size;
    p->scratch = oss.str();
    return p->scratch.c_str();
}

long dmsoft::FindPicMem(
    long x1, long y1, long x2, long y2,
    PCSTR pic_info, PCSTR delta_color,
    double sim, long dir, long *x, long *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    if (!x || !y) return -1;

    auto *p = P(impl);
    if (!p) return -1;
    const long threshold = static_cast<long>(
        std::ceil(std::clamp(sim, 0.0, 1.0) * 100.0));
    const auto matches = FindMemoryPicsAllCompat(
        p, x1, y1, x2, y2,
        pic_info, delta_color, threshold, dir, 1);
    if (matches.empty()) return -1;
    *x = matches.front().x;
    *y = matches.front().y;
    return matches.front().index;
}

const char *dmsoft::FindPicMemE(
    long x1, long y1, long x2, long y2,
    PCSTR pic_info, PCSTR delta_color,
    double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    long x = -1, y = -1;
    const long index = FindPicMem(
        x1, y1, x2, y2,
        pic_info, delta_color, sim, dir, &x, &y);
    p->scratch =
        std::to_string(index) + "|" +
        std::to_string(x) + "|" +
        std::to_string(y);
    return p->scratch.c_str();
}

const char *dmsoft::FindPicMemEx(
    long x1, long y1, long x2, long y2,
    PCSTR pic_info, PCSTR delta_color,
    double sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    const long threshold = static_cast<long>(
        std::ceil(std::clamp(sim, 0.0, 1.0) * 100.0));
    const auto matches = FindMemoryPicsAllCompat(
        p, x1, y1, x2, y2,
        pic_info, delta_color, threshold, dir, 1500);
    std::ostringstream oss;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i) oss << '|';
        oss << matches[i].index << ','
            << matches[i].x << ','
            << matches[i].y;
    }
    p->scratch = oss.str();
    return p->scratch.c_str();
}

long dmsoft::FindPicSimMem(
    long x1, long y1, long x2, long y2,
    PCSTR pic_info, PCSTR delta_color,
    long sim, long dir, long *x, long *y) {
    if (x) *x = -1;
    if (y) *y = -1;
    if (!x || !y) return -1;
    auto *p = P(impl);
    if (!p || sim < 0 || sim > 100) return -1;

    const auto matches = FindMemoryPicsAllCompat(
        p, x1, y1, x2, y2,
        pic_info, delta_color, sim, dir, 1);
    if (matches.empty()) return -1;
    *x = matches.front().x;
    *y = matches.front().y;
    return matches.front().index;
}

const char *dmsoft::FindPicSimMemE(
    long x1, long y1, long x2, long y2,
    PCSTR pic_info, PCSTR delta_color,
    long sim, long dir) {
    auto *p = P(impl);
    if (!p) return "";
    long x = -1, y = -1;
    const long index = FindPicSimMem(
        x1, y1, x2, y2,
        pic_info, delta_color, sim, dir, &x, &y);
    p->scratch =
        std::to_string(index) + "|" +
        std::to_string(x) + "|" +
        std::to_string(y);
    return p->scratch.c_str();
}

const char *dmsoft::FindPicSimMemEx(
    long x1, long y1, long x2, long y2,
    PCSTR pic_info, PCSTR delta_color,
    long sim, long dir) {
    auto *p = P(impl);
    if (!p || sim < 0 || sim > 100) return "";
    const auto matches = FindMemoryPicsAllCompat(
        p, x1, y1, x2, y2,
        pic_info, delta_color, sim, dir, 1500);
    std::ostringstream oss;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i) oss << '|';
        oss << matches[i].index << ','
            << matches[i].x << ','
            << matches[i].y;
    }
    p->scratch = oss.str();
    return p->scratch.c_str();
}

long dmsoft::GetScreenData(
    long x1, long y1, long x2, long y2) {
    auto *p = P(impl);
    if (!p) return 0;

    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(
            p, x1, y1, x2, y2, image))
        return 0;

    const SIZE_T count = image.pixels.size();
    if (count == 0 ||
        count > std::numeric_limits<SIZE_T>::max() / sizeof(DWORD))
        return 0;
    const SIZE_T bytes = count * sizeof(DWORD);
    auto *memory = static_cast<DWORD *>(
        AllocateLegacyBinBufferCompat(p, bytes));
    if (!memory) return 0;

    for (size_t i = 0; i < count; ++i) {
        const auto &c = image.pixels[i];
        memory[i] =
            (static_cast<DWORD>(c.r) << 16) |
            (static_cast<DWORD>(c.g) << 8) |
            static_cast<DWORD>(c.b);
    }
    return static_cast<long>(
        reinterpret_cast<INT_PTR>(memory));
}

long dmsoft::GetScreenDataBmp(
    long x1, long y1, long x2, long y2,
    long *data, long *size) {
    if (data) *data = 0;
    if (size) *size = 0;
    if (!data || !size) return 0;

    auto *p = P(impl);
    if (!p) return 0;

    ScreenImageCompat image;
    if (!CaptureScreenRegionForObjectCompat(
            p, x1, y1, x2, y2, image))
        return 0;

    std::vector<unsigned char> bytes;
    if (!ScreenImageToBmpBytesCompat(image, bytes) ||
        bytes.empty() ||
        bytes.size() > static_cast<size_t>(LONG_MAX))
        return 0;

    void *memory =
        AllocateLegacyBinBufferCompat(p, bytes.size());
    if (!memory) return 0;
    std::memcpy(memory, bytes.data(), bytes.size());

    *data = static_cast<long>(
        reinterpret_cast<INT_PTR>(memory));
    *size = static_cast<long>(bytes.size());
    return 1;
}

long dmsoft::FreeScreenData(long handle) {
    auto *p = P(impl);
    if (!p) return 0;
    if (!p->legacy_bin_buffer) return 1;

    const long current = static_cast<long>(
        reinterpret_cast<INT_PTR>(p->legacy_bin_buffer));
    if (handle != 0 && handle != current) return 0;
    FreeLegacyBinBufferCompat(p);
    return 1;
}


long dmsoft::FindInputMethod(PCSTR id) {
    std::string klid;
    return FindKeyboardLayoutByTextCompat(id, klid) ? 1 : 0;
}

long dmsoft::CheckInputMethod(long hwnd, PCSTR id) {
    return ThreadUsesLayoutTextCompat(HwndFromLong(hwnd), id) ? 1 : 0;
}

long dmsoft::ActiveInputMethod(long hwnd, PCSTR id) {
    return ActivateLayoutTextCompat(HwndFromLong(hwnd), id) ? 1 : 0;
}


long dmsoft::BindWindow(
    long hwnd, PCSTR display, PCSTR mouse,
    PCSTR keypad, long mode) {
    return BindWindowEx(
        hwnd, display, mouse, keypad, "", mode);
}

long dmsoft::BindWindowEx(
    long hwnd, PCSTR display, PCSTR mouse,
    PCSTR keypad, PCSTR public_desc, long mode) {
    auto *p = P(impl);
    HWND target = HwndFromLong(hwnd);
    if (!p || !target || !::IsWindow(target) ||
        !display || !mouse || !keypad || !public_desc)
        return 0;

    const auto allowed_display =
        _stricmp(display,"normal")==0 ||
        _stricmp(display,"gdi")==0 ||
        _stricmp(display,"gdi2")==0;
    const auto allowed_mouse =
        _stricmp(mouse,"normal")==0 ||
        _stricmp(mouse,"windows")==0 ||
        _stricmp(mouse,"windows3")==0;
    const auto allowed_keypad =
        _stricmp(keypad,"normal")==0 ||
        _stricmp(keypad,"windows")==0;

    // DX/driver/injection modes are intentionally not claimed as recovered
    // until their original behavior is separately reconstructed.
    if (!allowed_display || !allowed_mouse || !allowed_keypad)
        return 0;
    if (mode != 0 && mode != 2)
        return 0;

    HWND old = nullptr;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        old = p->bound_hwnd;
        p->bound_hwnd = target;
        p->bind_display = display;
        p->bind_mouse = mouse;
        p->bind_keypad = keypad;
        p->bind_public = public_desc;
        p->bind_mode = mode;
        p->bind_enable = 1;

        POINT cursor{};
        if (::GetCursorPos(&cursor) &&
            ::ScreenToClient(target, &cursor)) {
            p->virtual_mouse_x = cursor.x;
            p->virtual_mouse_y = cursor.y;
        } else {
            p->virtual_mouse_x = 0;
            p->virtual_mouse_y = 0;
        }
    }

    if (old && old != target)
        UnregisterBoundWindowCompat(old);
    if (!old || old != target)
        RegisterBoundWindowCompat(target);
    return 1;
}

long dmsoft::UnBindWindow() {
    auto *p = P(impl);
    if (!p) return 0;
    HWND old = nullptr;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        old = p->bound_hwnd;
    }
    if (!old) return 0;
    ClearObjectBindingCompat(p);
    return 1;
}

long dmsoft::ForceUnBindWindow(long hwnd) {
    auto *p = P(impl);
    if (!p) return 0;
    const HWND target = HwndFromLong(hwnd);
    HWND current = nullptr;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        current = p->bound_hwnd;
    }
    if (!target || current != target) return 0;
    ClearObjectBindingCompat(p);
    return 1;
}

long dmsoft::GetBindWindow() {
    auto *p = P(impl);
    if (!p) return 0;
    std::lock_guard<std::mutex> lock(p->state_mutex);
    return static_cast<long>(
        reinterpret_cast<INT_PTR>(p->bound_hwnd));
}

long dmsoft::IsBind(long hwnd) {
    return IsWindowBoundCompat(HwndFromLong(hwnd)) ? 1 : 0;
}

long dmsoft::EnableBind(long en) {
    auto *p = P(impl);
    if (!p) return 0;
    if (en != -1 && en != 0 && en != 1 && en != 5)
        return 0;
    std::lock_guard<std::mutex> lock(p->state_mutex);
    if (!p->bound_hwnd) return 0;
    p->bind_enable = en;
    return 1;
}

long dmsoft::SwitchBindWindow(long hwnd) {
    auto *p = P(impl);
    const HWND target = HwndFromLong(hwnd);
    if (!p || !target || !::IsWindow(target)) return 0;

    DWORD old_pid = 0, new_pid = 0;
    HWND old = nullptr;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        old = p->bound_hwnd;
    }
    if (!old) return 0;
    ::GetWindowThreadProcessId(old, &old_pid);
    ::GetWindowThreadProcessId(target, &new_pid);
    if (!old_pid || old_pid != new_pid) return 0;

    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        p->bound_hwnd = target;
    }
    if (old != target) {
        UnregisterBoundWindowCompat(old);
        RegisterBoundWindowCompat(target);
    }
    return 1;
}


long dmsoft::GetCpuType() {
    int cpu[4]{};
    __cpuid(cpu, 0);
    char vendor[13]{};
    std::memcpy(vendor + 0, &cpu[1], 4);
    std::memcpy(vendor + 4, &cpu[3], 4);
    std::memcpy(vendor + 8, &cpu[2], 4);
    if (std::strcmp(vendor, "GenuineIntel") == 0) return 1;
    if (std::strcmp(vendor, "AuthenticAMD") == 0) return 2;
    return 0;
}

const char *dmsoft::GetCursorSpot() {
    auto *p = P(impl);
    if (!p) return "";

    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!::GetCursorInfo(&ci) || !ci.hCursor) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    ICONINFO ii{};
    if (!::GetIconInfo(ci.hCursor, &ii)) {
        p->scratch.clear();
        return p->scratch.c_str();
    }

    std::ostringstream oss;
    oss << ii.xHotspot << ',' << ii.yHotspot;
    p->scratch = oss.str();

    if (ii.hbmMask) ::DeleteObject(ii.hbmMask);
    if (ii.hbmColor) ::DeleteObject(ii.hbmColor);
    return p->scratch.c_str();
}

long dmsoft::SpeedNormalGraphic(long en) {
    auto *p = P(impl);
    if (!p || (en != 0 && en != 1)) return 0;
    std::lock_guard<std::mutex> lock(p->state_mutex);
    p->speed_normal_graphic = en != 0;
    return 1;
}

long dmsoft::IsDisplayDead(long x1, long y1, long x2, long y2, long t) {
    auto *p = P(impl);
    if (!p || x2 < x1 || y2 < y1 || t < 0) return 0;

    ScreenImageCompat first;
    if (!CaptureScreenRegionForObjectCompat(p, x1, y1, x2, y2, first))
        return 0;

    const ULONGLONG timeout =
        static_cast<ULONGLONG>(t) * 1000ULL;
    const ULONGLONG start = ::GetTickCount64();

    for (;;) {
        HWND bound = nullptr;
        {
            std::lock_guard<std::mutex> lock(p->state_mutex);
            bound = p->bound_hwnd;
        }
        if (bound && !::IsWindow(bound)) return 1;

        if (::GetTickCount64() - start >= timeout)
            return 1;

        ::Sleep(50);

        ScreenImageCompat current;
        if (!CaptureScreenRegionForObjectCompat(
                p, x1, y1, x2, y2, current))
            return 0;

        if (current.width != first.width ||
            current.height != first.height ||
            current.pixels.size() != first.pixels.size())
            return 0;

        if (!std::equal(
                current.pixels.begin(),
                current.pixels.end(),
                first.pixels.begin(),
                [](const RgbColorCompat &a, const RgbColorCompat &b) {
                    return a.r == b.r &&
                           a.g == b.g &&
                           a.b == b.b;
                }))
            return 0;
    }
}

long dmsoft::LockDisplay(long lock) {
    auto *p = P(impl);
    if (!p || (lock != 0 && lock != 1)) return 0;

    if (lock == 0) {
        std::lock_guard<std::mutex> guard(p->state_mutex);
        p->display_locked = false;
        p->locked_display.reset();
        return 1;
    }

    HWND hwnd = nullptr;
    std::string display;
    {
        std::lock_guard<std::mutex> guard(p->state_mutex);
        hwnd = p->bound_hwnd;
        display = p->bind_display;
    }
    if (!hwnd || !::IsWindow(hwnd)) return 0;

    RECT client{};
    if (!::GetClientRect(hwnd, &client)) return 0;
    const long width = client.right - client.left;
    const long height = client.bottom - client.top;
    if (width <= 0 || height <= 0) return 0;

    ScreenImageCompat captured;
    if (!CaptureBoundClientRegionCompat(
            hwnd, display, 0, 0, width - 1, height - 1, captured))
        return 0;

    {
        std::lock_guard<std::mutex> guard(p->state_mutex);
        p->locked_display =
            std::make_shared<ScreenImageCompat>(std::move(captured));
        p->display_locked = true;
    }
    return 1;
}


long dmsoft::SetDict(long index, PCSTR dict_name) {
    auto *p = P(impl);
    if (!p || index < 0 || index >= 100 ||
        !dict_name || !*dict_name)
        return 0;

    // Encrypted dictionaries are a separate legacy path. Do not silently
    // treat encrypted bytes as plaintext.
    if (!p->dict_password.empty())
        return 0;

    const std::filesystem::path path =
        ResolveObjectFilePathCompat(p, dict_name);
    std::vector<LegacyDictEntryCompat> entries;
    if (!ReadLegacyDictFileCompat(path, entries))
        return 0;

    std::lock_guard<std::mutex> lock(p->state_mutex);
    p->dictionaries[static_cast<size_t>(index)] =
        std::move(entries);
    p->dictionary_sources[static_cast<size_t>(index)] =
        path.string();
    return 1;
}

const char *dmsoft::GetDict(long index, long font_index) {
    auto *p = P(impl);
    if (!p || index < 0 || index >= 100 ||
        font_index < 0) return "";

    std::lock_guard<std::mutex> lock(p->state_mutex);
    const auto &dict =
        p->dictionaries[static_cast<size_t>(index)];
    if (static_cast<size_t>(font_index) >= dict.size()) {
        p->scratch.clear();
        return p->scratch.c_str();
    }
    p->scratch =
        dict[static_cast<size_t>(font_index)].raw;
    return p->scratch.c_str();
}

long dmsoft::SetDictMem(long index, long addr, long size) {
    auto *p = P(impl);
    if (!p || index < 0 || index >= 100 ||
        addr == 0 || size <= 0)
        return 0;

    const char *data = reinterpret_cast<const char *>(
        static_cast<ULONG_PTR>(
            static_cast<unsigned long>(addr)));

    std::vector<LegacyDictEntryCompat> entries;
    __try {
        if (!LoadLegacyDictTextCompat(
                data, static_cast<size_t>(size), entries))
            return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(p->state_mutex);
    p->dictionaries[static_cast<size_t>(index)] =
        std::move(entries);
    p->dictionary_sources[static_cast<size_t>(index)].clear();
    return 1;
}

long dmsoft::AddDict(long index, PCSTR dict_info) {
    auto *p = P(impl);
    if (!p || index < 0 || index >= 100 ||
        !dict_info || !*dict_info)
        return 0;

    std::lock_guard<std::mutex> lock(p->state_mutex);
    auto &dict = p->dictionaries[static_cast<size_t>(index)];
    if (dict.size() >= 12000)
        return 0;
    return UpsertLegacyDictEntryCompat(dict, dict_info);
}

long dmsoft::SaveDict(long index, PCSTR file) {
    auto *p = P(impl);
    if (!p || index < 0 || index >= 100 ||
        !file || !*file)
        return 0;

    std::vector<LegacyDictEntryCompat> snapshot;
    {
        std::lock_guard<std::mutex> lock(p->state_mutex);
        snapshot =
            p->dictionaries[static_cast<size_t>(index)];
    }

    const std::filesystem::path path =
        ResolveObjectFilePathCompat(p, file);
    return SaveLegacyDictFileCompat(path, snapshot) ? 1 : 0;
}

long dmsoft::ClearDict(long index) {
    auto *p = P(impl);
    if (!p || index < 0 || index >= 100)
        return 0;
    std::lock_guard<std::mutex> lock(p->state_mutex);
    p->dictionaries[static_cast<size_t>(index)].clear();
    p->dictionary_sources[static_cast<size_t>(index)].clear();
    return 1;
}

long dmsoft::GetDictCount(long index) {
    auto *p = P(impl);
    if (!p || index < 0 || index >= 100)
        return 0;
    std::lock_guard<std::mutex> lock(p->state_mutex);
    return static_cast<long>(
        p->dictionaries[static_cast<size_t>(index)].size());
}

#include "legacy_dm_generated.inc"
