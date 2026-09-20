#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

namespace {

constexpr ULONG_PTR kCreateObjRva  = 101072;
constexpr ULONG_PTR kReleaseObjRva = 101168;
constexpr int kVtableEntries = 417; // destructor + 416 public virtual methods

bool IsReadableProtection(DWORD protect) {
    if (protect & PAGE_GUARD) return false;
    const DWORD p = protect & 0xff;
    return p == PAGE_READONLY ||
           p == PAGE_READWRITE ||
           p == PAGE_WRITECOPY ||
           p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}

std::string BytesAt(ULONG_PTR address, size_t count) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!::VirtualQuery(reinterpret_cast<LPCVOID>(address), &mbi, sizeof(mbi)))
        return {};
    if (!IsReadableProtection(mbi.Protect))
        return {};

    const ULONG_PTR region_end =
        reinterpret_cast<ULONG_PTR>(mbi.BaseAddress) + mbi.RegionSize;
    if (address >= region_end) return {};
    count = static_cast<size_t>(
        (std::min<ULONG_PTR>)(address + count, region_end) - address);

    const auto *p = reinterpret_cast<const unsigned char *>(address);
    std::ostringstream oss;
    for (size_t i = 0; i < count; ++i) {
        if (i) oss << ' ';
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<unsigned int>(p[i]);
    }
    return oss.str();
}

} // namespace

int main(int argc, char **argv) {
#if defined(_WIN64)
    std::puts("legacy_vtable_dump must be built as Win32.");
    return 200;
#else
    const char *dll_path = argc > 1 ? argv[1] : "hcbyj.dll";
    const char *out_path = argc > 2 ? argv[2] : "legacy_runtime_vtable.csv";

    HMODULE module = ::LoadLibraryA(dll_path);
    if (!module) {
        std::printf("LoadLibraryA failed: %lu\n", ::GetLastError());
        return 1;
    }

    using CreateObj = long (WINAPI *)(void);
    using ReleaseObj = long (WINAPI *)(long);

    auto create_obj = reinterpret_cast<CreateObj>(
        reinterpret_cast<ULONG_PTR>(module) + kCreateObjRva);
    auto release_obj = reinterpret_cast<ReleaseObj>(
        reinterpret_cast<ULONG_PTR>(module) + kReleaseObjRva);

    const long obj_value = create_obj();
    if (!obj_value) {
        std::puts("CreateObj returned 0.");
        ::FreeLibrary(module);
        return 2;
    }

    const ULONG_PTR object = static_cast<ULONG_PTR>(
        static_cast<unsigned long>(obj_value));
    const ULONG_PTR vtable = *reinterpret_cast<const ULONG_PTR *>(object);
    const ULONG_PTR base = reinterpret_cast<ULONG_PTR>(module);

    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::printf("Cannot open output: %s\n", out_path);
        release_obj(obj_value);
        ::FreeLibrary(module);
        return 3;
    }

    out << "slot,entry_va,entry_rva,module_base,vtable_va,protect,first32\r\n";

    for (int slot = 0; slot < kVtableEntries; ++slot) {
        const ULONG_PTR entry = reinterpret_cast<const ULONG_PTR *>(vtable)[slot];

        MEMORY_BASIC_INFORMATION mbi{};
        DWORD protect = 0;
        if (::VirtualQuery(reinterpret_cast<LPCVOID>(entry), &mbi, sizeof(mbi)))
            protect = mbi.Protect;

        out << slot << ','
            << "0x" << std::hex << std::uppercase << entry << ',';

        if (entry >= base)
            out << "0x" << std::hex << std::uppercase << (entry - base);
        else
            out << "OUTSIDE";

        out << ','
            << "0x" << std::hex << std::uppercase << base << ','
            << "0x" << std::hex << std::uppercase << vtable << ','
            << "0x" << std::hex << std::uppercase << protect << ','
            << '"' << BytesAt(entry, 32) << '"' << "\r\n";
    }

    out.close();

    std::printf("module=0x%08lX object=0x%08lX vtable=0x%08lX\n",
        static_cast<unsigned long>(base),
        static_cast<unsigned long>(object),
        static_cast<unsigned long>(vtable));
    std::printf("wrote %d runtime vtable entries -> %s\n",
        kVtableEntries, out_path);

    release_obj(obj_value);
    ::FreeLibrary(module);
    return 0;
#endif
}
