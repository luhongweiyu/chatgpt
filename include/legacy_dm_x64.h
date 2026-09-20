/** hcbyj x64 恢复版接口：保留旧 dmsoft 方法名/参数/返回类型；内部不再按 x86 RVA 跳转。 */
#pragma once
#ifndef __INCLUDE_OBJ_H__
#define __INCLUDE_OBJ_H__
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>

#if !defined(_WIN64)
#error hcbyj_x64 requires a 64-bit Windows build.
#endif

#ifdef HCBYJ64_BUILD
#define HCBYJ64_API __declspec(dllexport)
#else
#define HCBYJ64_API __declspec(dllimport)
#endif

#if !defined(__cplusplus)
#error C++ compiler required
#endif
extern "C" HCBYJ64_API BOOL LoadDm(PCSTR path);
extern "C" HCBYJ64_API BOOL LoadDmW(PCWSTR path);
extern "C" HCBYJ64_API BOOL FreeDm(void);

class HCBYJ64_API dmsoft
{
private:
    void *impl;
public:
    dmsoft();
    virtual ~dmsoft();
    bool IsValid() const;
    #include "legacy_dm_x64_decl1.inc"
    #include "legacy_dm_x64_decl2.inc"
    #include "legacy_dm_x64_decl3.inc"
    #include "legacy_dm_x64_decl4.inc"
    #include "legacy_dm_x64_decl5.inc"
    #include "legacy_dm_x64_decl6.inc"
    #include "legacy_dm_x64_decl7.inc"
    #include "legacy_dm_x64_decl8.inc"
};
#endif
