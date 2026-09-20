#include "legacy_dm_x64.h"
#include <cstdio>

int main(int argc, char **argv) {
    const char *backend = argc > 1 ? argv[1] : nullptr;
    if (!LoadDm(backend)) {
        std::puts("LoadDm failed");
        return 2;
    }
    dmsoft dm;
    if (!dm.IsValid()) {
        std::printf("dmsoft backend init failed, error=%ld\n", dm.GetLastError());
        return 3;
    }
    std::printf("backend Ver=%s\n", dm.Ver());
    std::printf("Is64Bit=%ld\n", dm.Is64Bit());
    std::printf("Screen=%ldx%ld\n", dm.GetScreenWidth(), dm.GetScreenHeight());
    FreeDm();
    return 0;
}
