#include "legacy_dm_x64.h"
#include "op_dispatch.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <psapi.h>

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
    MODULEENTRY32A me{};me.dwSize=sizeof(me);LONGLONG result=0;
    if(Module32FirstA(snap,&me)){do{if(!module_name||!*module_name||_stricmp(me.szModule,module_name)==0||_stricmp(me.szExePath,module_name)==0){result=static_cast<LONGLONG>(reinterpret_cast<ULONG_PTR>(me.modBaseAddr));break;}}while(Module32NextA(snap,&me));}
    CloseHandle(snap);SetNativeError(p,result?0:ERROR_MOD_NOT_FOUND);return result;
}

long dmsoft::GetModuleSize(long hwnd, PCSTR module_name) {
    auto *p=P(impl);if(!p)return 0;DWORD pid=ResolvePid(p,hwnd);if(!pid)return 0;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    if(snap==INVALID_HANDLE_VALUE){SetNativeError(p,static_cast<long>(::GetLastError()));return 0;}
    MODULEENTRY32A me{};me.dwSize=sizeof(me);DWORD result=0;
    if(Module32FirstA(snap,&me)){do{if(!module_name||!*module_name||_stricmp(me.szModule,module_name)==0||_stricmp(me.szExePath,module_name)==0){result=me.modBaseSize;break;}}while(Module32NextA(snap,&me));}
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

#include "legacy_dm_x64_part1.inc"
#include "legacy_dm_x64_part2.inc"
#include "legacy_dm_x64_part3.inc"
#include "legacy_dm_x64_part4.inc"
#include "legacy_dm_x64_part5.inc"
#include "legacy_dm_x64_part6.inc"
#include "legacy_dm_x64_part7.inc"
#include "legacy_dm_x64_part8.inc"
