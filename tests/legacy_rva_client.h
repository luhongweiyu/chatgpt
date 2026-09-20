#pragma once
#include <windows.h>
#include <stdexcept>
#include <string>

class LegacyRvaClient {
public:
    explicit LegacyRvaClient(const char *dll_path) {
        module_ = ::LoadLibraryA(dll_path);
        if (!module_) throw std::runtime_error("LoadLibraryA(hcbyj.dll) failed");
        using CreateObj = long (WINAPI *)(void);
        auto fn = at<CreateObj>(101072);
        obj_ = fn();
        if (!obj_) throw std::runtime_error("legacy CreateObj failed");
    }

    ~LegacyRvaClient() {
        if (module_ && obj_) {
            using ReleaseObj = long (WINAPI *)(long);
            at<ReleaseObj>(101168)(obj_);
        }
        if (module_) ::FreeLibrary(module_);
    }

    LegacyRvaClient(const LegacyRvaClient&) = delete;
    LegacyRvaClient& operator=(const LegacyRvaClient&) = delete;

    bool valid() const { return module_ && obj_; }

    const char *Hex32(long v) { using F=PCSTR(WINAPI*)(long,long); return at<F>(126368)(obj_,v); }
    const char *Hex64(LONGLONG v) { using F=PCSTR(WINAPI*)(long,LONGLONG); return at<F>(111568)(obj_,v); }
    const char *RGB2BGR(PCSTR v) { using F=PCSTR(WINAPI*)(long,PCSTR); return at<F>(121488)(obj_,v); }
    const char *BGR2RGB(PCSTR v) { using F=PCSTR(WINAPI*)(long,PCSTR); return at<F>(101856)(obj_,v); }
    long StrStr(PCSTR s, PCSTR v) { using F=long(WINAPI*)(long,PCSTR,PCSTR); return at<F>(114496)(obj_,s,v); }

    long GetScreenWidth() { using F=long(WINAPI*)(long); return at<F>(119200)(obj_); }
    long GetScreenHeight() { using F=long(WINAPI*)(long); return at<F>(115824)(obj_); }
    long GetScreenDepth() { using F=long(WINAPI*)(long); return at<F>(101664)(obj_); }
    long GetDPI() { using F=long(WINAPI*)(long); return at<F>(108768)(obj_); }
    long GetOsBuildNumber() { using F=long(WINAPI*)(long); return at<F>(112240)(obj_); }
    long Is64Bit() { using F=long(WINAPI*)(long); return at<F>(125008)(obj_); }
    long CheckFontSmooth() { using F=long(WINAPI*)(long); return at<F>(113136)(obj_); }
    long GetTime() { using F=long(WINAPI*)(long); return at<F>(112288)(obj_); }

    long IsFileExist(PCSTR file) { using F=long(WINAPI*)(long,PCSTR); return at<F>(120448)(obj_,file); }
    long IsFolderExist(PCSTR folder) { using F=long(WINAPI*)(long,PCSTR); return at<F>(110592)(obj_,folder); }
    long CreateFolder(PCSTR folder) { using F=long(WINAPI*)(long,PCSTR); return at<F>(101808)(obj_,folder); }
    long DeleteFolder(PCSTR folder) { using F=long(WINAPI*)(long,PCSTR); return at<F>(118416)(obj_,folder); }
    long DeleteFile(PCSTR file) { using F=long(WINAPI*)(long,PCSTR); return at<F>(110240)(obj_,file); }
    long MoveFile(PCSTR src, PCSTR dst) { using F=long(WINAPI*)(long,PCSTR,PCSTR); return at<F>(101440)(obj_,src,dst); }
    long CopyFile(PCSTR src, PCSTR dst, long over) { using F=long(WINAPI*)(long,PCSTR,PCSTR,long); return at<F>(112848)(obj_,src,dst,over); }
    long GetFileLength(PCSTR file) { using F=long(WINAPI*)(long,PCSTR); return at<F>(108928)(obj_,file); }
    long WriteFile(PCSTR file, PCSTR content) { using F=long(WINAPI*)(long,PCSTR,PCSTR); return at<F>(101600)(obj_,file,content); }
    const char *ReadFile(PCSTR file) { using F=PCSTR(WINAPI*)(long,PCSTR); return at<F>(124000)(obj_,file); }
    const char *ReadFileData(PCSTR file,long start,long end) { using F=PCSTR(WINAPI*)(long,PCSTR,long,long); return at<F>(123936)(obj_,file,start,end); }

    long WriteIni(PCSTR sec,PCSTR key,PCSTR val,PCSTR file) { using F=long(WINAPI*)(long,PCSTR,PCSTR,PCSTR,PCSTR); return at<F>(126128)(obj_,sec,key,val,file); }
    const char *ReadIni(PCSTR sec,PCSTR key,PCSTR file) { using F=PCSTR(WINAPI*)(long,PCSTR,PCSTR,PCSTR); return at<F>(106032)(obj_,sec,key,file); }
    long DeleteIni(PCSTR sec,PCSTR key,PCSTR file) { using F=long(WINAPI*)(long,PCSTR,PCSTR,PCSTR); return at<F>(125632)(obj_,sec,key,file); }
    const char *EnumIniKey(PCSTR sec,PCSTR file) { using F=PCSTR(WINAPI*)(long,PCSTR,PCSTR); return at<F>(125264)(obj_,sec,file); }
    const char *EnumIniSection(PCSTR file) { using F=PCSTR(WINAPI*)(long,PCSTR); return at<F>(102224)(obj_,file); }

    long SetEnv(long index,PCSTR name,PCSTR value) { using F=long(WINAPI*)(long,long,PCSTR,PCSTR); return at<F>(106688)(obj_,index,name,value); }
    const char *GetEnv(long index,PCSTR name) { using F=PCSTR(WINAPI*)(long,long,PCSTR); return at<F>(125376)(obj_,index,name); }
    long DelEnv(long index,PCSTR name) { using F=long(WINAPI*)(long,long,PCSTR); return at<F>(126992)(obj_,index,name); }

private:
    template<class T>
    T at(ULONG_PTR rva) const {
        return reinterpret_cast<T>(reinterpret_cast<ULONG_PTR>(module_) + rva);
    }

    HMODULE module_ = nullptr;
    long obj_ = 0;
};
