#include "op_dispatch.h"

#include <algorithm>
#include <cwchar>
#include <vector>

namespace hcbyj64 {
namespace {

std::mutex g_runtime_mutex;
std::wstring g_backend_path;
HMODULE g_tools_module = nullptr;
bool g_setup_done = false;
long g_runtime_error = 0;

constexpr long ERR_RUNTIME_SETUP = 0x7101;
constexpr long ERR_COM_CREATE = 0x7102;
constexpr long ERR_NO_METHOD = 0x7103;
constexpr long ERR_INVOKE = 0x7104;
constexpr long ERR_CONVERT = 0x7105;

std::wstring AnsiToWide(PCSTR s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::string WideToAnsi(BSTR s) {
    if (!s) return {};
    int len = static_cast<int>(SysStringLen(s));
    if (len == 0) return {};
    int n = WideCharToMultiByte(CP_ACP, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, s, len, out.data(), n, nullptr, nullptr);
    return out;
}

bool Exists(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectory(const std::wstring &p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring DirName(const std::wstring &p) {
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    if (pos == 0) return p.substr(0, 1);
    return p.substr(0, pos);
}

std::wstring Join(const std::wstring &a, const wchar_t *b) {
    if (a.empty()) return b ? std::wstring(b) : std::wstring();
    std::wstring out = a;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    if (b) out += b;
    return out;
}

void FreeExcep(EXCEPINFO &ei) {
    if (ei.bstrSource) SysFreeString(ei.bstrSource);
    if (ei.bstrDescription) SysFreeString(ei.bstrDescription);
    if (ei.bstrHelpFile) SysFreeString(ei.bstrHelpFile);
    ZeroMemory(&ei, sizeof(ei));
}

} // namespace

OpArg OpArg::From(long v) {
    OpArg a; a.kind = Kind::I4; a.i4 = v; return a;
}
OpArg OpArg::From(LONGLONG v) {
    OpArg a; a.kind = Kind::I8; a.i8 = v; return a;
}
OpArg OpArg::From(float v) {
    OpArg a; a.kind = Kind::R4; a.r4 = v; return a;
}
OpArg OpArg::From(double v) {
    OpArg a; a.kind = Kind::R8; a.r8 = v; return a;
}
OpArg OpArg::From(PCSTR v) {
    OpArg a; a.kind = Kind::String; if (v) a.str = v; return a;
}
OpArg OpArg::From(long *v) {
    OpArg a; a.kind = Kind::I4Ref; a.i4ref = v; return a;
}

bool OpRuntime::Configure(PCSTR path) {
    if (!path || !*path) return ConfigureW(nullptr);
    auto w = AnsiToWide(path);
    return ConfigureW(w.c_str());
}

bool OpRuntime::ConfigureW(PCWSTR path) {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    g_backend_path = path ? path : L"";
    g_setup_done = false;
    g_runtime_error = 0;
    return true;
}

void OpRuntime::Reset() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    g_backend_path.clear();
    g_setup_done = false;
    g_runtime_error = 0;
}

long OpRuntime::LastRuntimeError() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    return g_runtime_error;
}

bool OpRuntime::EnsureReady() {
    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    if (g_setup_done) return true;
    if (g_backend_path.empty()) {
        g_setup_done = true;
        return true;
    }

    std::wstring op_dll;
    std::wstring dir;
    if (IsDirectory(g_backend_path)) {
        dir = g_backend_path;
        op_dll = Join(dir, L"op_x64.dll");
    } else {
        op_dll = g_backend_path;
        dir = DirName(g_backend_path);
    }
    std::wstring tools_dll = Join(dir, L"tools.dll");

    if (!Exists(op_dll) || !Exists(tools_dll)) {
        g_runtime_error = ERR_RUNTIME_SETUP;
        return false;
    }

    if (!g_tools_module) {
        g_tools_module = LoadLibraryW(tools_dll.c_str());
        if (!g_tools_module) {
            g_runtime_error = static_cast<long>(GetLastError());
            return false;
        }
    }

    using SetupW = int (__cdecl *)(PCWSTR);
    auto setup = reinterpret_cast<SetupW>(GetProcAddress(g_tools_module, "setupW"));
    if (!setup) {
        g_runtime_error = ERR_RUNTIME_SETUP;
        return false;
    }
    if (setup(op_dll.c_str()) != 1) {
        g_runtime_error = ERR_RUNTIME_SETUP;
        return false;
    }

    g_setup_done = true;
    g_runtime_error = 0;
    return true;
}

OpObject::OpObject() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        com_initialized_here_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        SetError(static_cast<long>(hr));
        return;
    }

    if (!OpRuntime::EnsureReady()) {
        SetError(OpRuntime::LastRuntimeError());
        return;
    }

    CLSID clsid{};
    hr = CLSIDFromProgID(L"op.opsoft", &clsid);
    if (FAILED(hr)) {
        SetError(static_cast<long>(hr));
        return;
    }

    hr = CoCreateInstance(
        clsid, nullptr, CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
        IID_IDispatch, reinterpret_cast<void **>(&dispatch_));
    if (FAILED(hr) || !dispatch_) {
        SetError(FAILED(hr) ? static_cast<long>(hr) : ERR_COM_CREATE);
        return;
    }

    SetError(0);
}

OpObject::~OpObject() {
    if (dispatch_) {
        dispatch_->Release();
        dispatch_ = nullptr;
    }
    if (com_initialized_here_) {
        CoUninitialize();
        com_initialized_here_ = false;
    }
}

bool OpObject::IsValid() const {
    return dispatch_ != nullptr;
}

long OpObject::LastError() const {
    return last_error_;
}

void OpObject::SetError(long code) const {
    last_error_ = code;
}

bool OpObject::InvokeRaw(const char *name, std::initializer_list<OpArg> args, VARIANT *result) {
    if (!dispatch_ || !name || !*name || !result) {
        SetError(ERR_INVOKE);
        return false;
    }

    auto wname = AnsiToWide(name);
    if (wname.empty()) {
        SetError(ERR_NO_METHOD);
        return false;
    }

    LPOLESTR names[1] = { const_cast<LPOLESTR>(wname.c_str()) };
    DISPID dispid = DISPID_UNKNOWN;
    HRESULT hr = dispatch_->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &dispid);
    if (FAILED(hr)) {
        SetError(ERR_NO_METHOD);
        return false;
    }

    std::vector<VARIANTARG> vargs(args.size());
    size_t i = 0;
    for (auto it = args.end(); it != args.begin();) {
        --it;
        VARIANTARG &v = vargs[i++];
        VariantInit(&v);
        switch (it->kind) {
        case OpArg::Kind::I4:
            V_VT(&v) = VT_I4; V_I4(&v) = it->i4; break;
        case OpArg::Kind::I8:
            V_VT(&v) = VT_I8; V_I8(&v) = it->i8; break;
        case OpArg::Kind::R4:
            V_VT(&v) = VT_R4; V_R4(&v) = it->r4; break;
        case OpArg::Kind::R8:
            V_VT(&v) = VT_R8; V_R8(&v) = it->r8; break;
        case OpArg::Kind::String: {
            V_VT(&v) = VT_BSTR;
            auto ws = AnsiToWide(it->str.c_str());
            V_BSTR(&v) = SysAllocStringLen(ws.data(), static_cast<UINT>(ws.size()));
            break;
        }
        case OpArg::Kind::I4Ref:
            V_VT(&v) = VT_I4 | VT_BYREF;
            V_I4REF(&v) = it->i4ref;
            break;
        }
    }

    DISPPARAMS dp{};
    dp.rgvarg = vargs.empty() ? nullptr : vargs.data();
    dp.cArgs = static_cast<UINT>(vargs.size());

    VariantInit(result);
    EXCEPINFO ei{};
    UINT arg_err = 0;
    hr = dispatch_->Invoke(
        dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
        &dp, result, &ei, &arg_err);

    for (auto &v : vargs) VariantClear(&v);
    FreeExcep(ei);

    if (FAILED(hr)) {
        VariantClear(result);
        SetError(static_cast<long>(hr));
        return false;
    }

    SetError(0);
    return true;
}

long OpObject::InvokeLong(const char *name, std::initializer_list<OpArg> args) {
    VARIANT r; VariantInit(&r);
    if (!InvokeRaw(name, args, &r)) return 0;
    VARIANT c; VariantInit(&c);
    HRESULT hr = VariantChangeType(&c, &r, 0, VT_I4);
    VariantClear(&r);
    if (FAILED(hr)) { SetError(ERR_CONVERT); return 0; }
    long out = V_I4(&c);
    VariantClear(&c);
    return out;
}

LONGLONG OpObject::InvokeInt64(const char *name, std::initializer_list<OpArg> args) {
    VARIANT r; VariantInit(&r);
    if (!InvokeRaw(name, args, &r)) return 0;
    VARIANT c; VariantInit(&c);
    HRESULT hr = VariantChangeType(&c, &r, 0, VT_I8);
    VariantClear(&r);
    if (FAILED(hr)) { SetError(ERR_CONVERT); return 0; }
    LONGLONG out = V_I8(&c);
    VariantClear(&c);
    return out;
}

float OpObject::InvokeFloat(const char *name, std::initializer_list<OpArg> args) {
    VARIANT r; VariantInit(&r);
    if (!InvokeRaw(name, args, &r)) return 0.0f;
    VARIANT c; VariantInit(&c);
    HRESULT hr = VariantChangeType(&c, &r, 0, VT_R4);
    VariantClear(&r);
    if (FAILED(hr)) { SetError(ERR_CONVERT); return 0.0f; }
    float out = V_R4(&c);
    VariantClear(&c);
    return out;
}

double OpObject::InvokeDouble(const char *name, std::initializer_list<OpArg> args) {
    VARIANT r; VariantInit(&r);
    if (!InvokeRaw(name, args, &r)) return 0.0;
    VARIANT c; VariantInit(&c);
    HRESULT hr = VariantChangeType(&c, &r, 0, VT_R8);
    VariantClear(&r);
    if (FAILED(hr)) { SetError(ERR_CONVERT); return 0.0; }
    double out = V_R8(&c);
    VariantClear(&c);
    return out;
}

const char *OpObject::InvokeString(const char *name, std::initializer_list<OpArg> args) {
    VARIANT r; VariantInit(&r);
    if (!InvokeRaw(name, args, &r)) {
        last_string_.clear();
        return last_string_.c_str();
    }
    VARIANT c; VariantInit(&c);
    HRESULT hr = VariantChangeType(&c, &r, 0, VT_BSTR);
    VariantClear(&r);
    if (FAILED(hr)) {
        SetError(ERR_CONVERT);
        last_string_.clear();
        return last_string_.c_str();
    }
    last_string_ = WideToAnsi(V_BSTR(&c));
    VariantClear(&c);
    return last_string_.c_str();
}

} // namespace hcbyj64
