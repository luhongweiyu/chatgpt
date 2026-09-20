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

#include <algorithm>
#include <cctype>
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
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using hcbyj64::A;

namespace {

struct DmImpl {
    hcbyj64::OpObject op;
    bool hwnd_is_pid = false;
    long native_error = 0;
    std::string scratch;
    std::map<std::pair<long, std::string>, std::string> env;
    std::mutex state_mutex;
    ULONGLONG prev_idle = 0;
    ULONGLONG prev_kernel = 0;
    ULONGLONG prev_user = 0;
    bool cpu_sample_valid = false;
};

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

} // namespace

extern "C" HCBYJ64_API BOOL LoadDm(PCSTR path) { return hcbyj64::OpRuntime::Configure(path) ? TRUE : FALSE; }
extern "C" HCBYJ64_API BOOL LoadDmW(PCWSTR path) { return hcbyj64::OpRuntime::ConfigureW(path) ? TRUE : FALSE; }
extern "C" HCBYJ64_API BOOL FreeDm(void) { hcbyj64::OpRuntime::Reset(); return TRUE; }

dmsoft::dmsoft() : impl(new DmImpl()) {}
dmsoft::~dmsoft() { delete P(impl); impl = nullptr; }
bool dmsoft::IsValid() const { return impl != nullptr && P(impl)->op.IsValid(); }

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
    POINT pt{};
    if (!::GetCursorPos(&pt)) return 0;
    *x = pt.x; *y = pt.y; return 1;
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

#include "legacy_dm_generated.inc"
