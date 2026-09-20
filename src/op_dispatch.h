#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleauto.h>

#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

namespace hcbyj64 {

struct OpArg {
    enum class Kind { I4, I8, R4, R8, String, I4Ref };

    Kind kind = Kind::I4;
    long i4 = 0;
    LONGLONG i8 = 0;
    float r4 = 0.0f;
    double r8 = 0.0;
    long *i4ref = nullptr;
    std::string str;

    static OpArg From(long v);
    static OpArg From(LONGLONG v);
    static OpArg From(float v);
    static OpArg From(double v);
    static OpArg From(PCSTR v);
    static OpArg From(long *v);
};

class OpRuntime {
public:
    static bool Configure(PCSTR path);
    static bool ConfigureW(PCWSTR path);
    static void Reset();
    static long LastRuntimeError();

private:
    friend class OpObject;
    static bool EnsureReady();
};

class OpObject {
public:
    OpObject();
    ~OpObject();
    OpObject(const OpObject &) = delete;
    OpObject &operator=(const OpObject &) = delete;
    bool IsValid() const;
    long LastError() const;
    long InvokeLong(const char *name, std::initializer_list<OpArg> args = {});
    LONGLONG InvokeInt64(const char *name, std::initializer_list<OpArg> args = {});
    float InvokeFloat(const char *name, std::initializer_list<OpArg> args = {});
    double InvokeDouble(const char *name, std::initializer_list<OpArg> args = {});
    const char *InvokeString(const char *name, std::initializer_list<OpArg> args = {});

private:
    bool InvokeRaw(const char *name, std::initializer_list<OpArg> args, VARIANT *result);
    void SetError(long code) const;
    IDispatch *dispatch_ = nullptr;
    bool com_initialized_here_ = false;
    mutable long last_error_ = 0;
    std::string last_string_;
};

inline OpArg A(long v) { return OpArg::From(v); }
inline OpArg A(LONGLONG v) { return OpArg::From(v); }
inline OpArg A(float v) { return OpArg::From(v); }
inline OpArg A(double v) { return OpArg::From(v); }
inline OpArg A(PCSTR v) { return OpArg::From(v); }
inline OpArg A(long *v) { return OpArg::From(v); }

} // namespace hcbyj64
