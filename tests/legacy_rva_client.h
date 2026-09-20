#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
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

    long SetMemoryHwndAsProcessId(long en) { using F=long(WINAPI*)(long,long); return at<F>(123088)(obj_,en); }
    LONGLONG ReadIntAddr(long hwnd,LONGLONG addr,long type) { using F=LONGLONG(WINAPI*)(long,long,LONGLONG,long); return at<F>(103696)(obj_,hwnd,addr,type); }
    long WriteIntAddr(long hwnd,LONGLONG addr,long type,LONGLONG v) { using F=long(WINAPI*)(long,long,LONGLONG,long,LONGLONG); return at<F>(109184)(obj_,hwnd,addr,type,v); }
    float ReadFloatAddr(long hwnd,LONGLONG addr) { using F=float(WINAPI*)(long,long,LONGLONG); return at<F>(117648)(obj_,hwnd,addr); }
    long WriteFloatAddr(long hwnd,LONGLONG addr,float v) { using F=long(WINAPI*)(long,long,LONGLONG,float); return at<F>(104864)(obj_,hwnd,addr,v); }
    double ReadDoubleAddr(long hwnd,LONGLONG addr) { using F=double(WINAPI*)(long,long,LONGLONG); return at<F>(117376)(obj_,hwnd,addr); }
    long WriteDoubleAddr(long hwnd,LONGLONG addr,double v) { using F=long(WINAPI*)(long,long,LONGLONG,double); return at<F>(123328)(obj_,hwnd,addr,v); }
    const char *ReadDataAddr(long hwnd,LONGLONG addr,long len) { using F=PCSTR(WINAPI*)(long,long,LONGLONG,long); return at<F>(111360)(obj_,hwnd,addr,len); }
    long WriteDataAddr(long hwnd,LONGLONG addr,PCSTR data) { using F=long(WINAPI*)(long,long,LONGLONG,PCSTR); return at<F>(121184)(obj_,hwnd,addr,data); }
    LONGLONG GetModuleBaseAddr(long hwnd,PCSTR name) { using F=LONGLONG(WINAPI*)(long,long,PCSTR); return at<F>(114560)(obj_,hwnd,name); }
    long GetModuleSize(long hwnd,PCSTR name) { using F=long(WINAPI*)(long,long,PCSTR); return at<F>(110688)(obj_,hwnd,name); }

    long GetWindowProcessId(long hwnd) { using F=long(WINAPI*)(long,long); return at<F>(126240)(obj_,hwnd); }
    long GetWindowThreadId(long hwnd) { using F=long(WINAPI*)(long,long); return at<F>(113568)(obj_,hwnd); }
    long GetWindowRect(long hwnd,long *x1,long *y1,long *x2,long *y2) { using F=long(WINAPI*)(long,long,long*,long*,long*,long*); return at<F>(102640)(obj_,hwnd,x1,y1,x2,y2); }
    long GetClientRect(long hwnd,long *x1,long *y1,long *x2,long *y2) { using F=long(WINAPI*)(long,long,long*,long*,long*,long*); return at<F>(118016)(obj_,hwnd,x1,y1,x2,y2); }
    long GetClientSize(long hwnd,long *w,long *h) { using F=long(WINAPI*)(long,long,long*,long*); return at<F>(113280)(obj_,hwnd,w,h); }
    long SetWindowSize(long hwnd,long w,long h) { using F=long(WINAPI*)(long,long,long,long); return at<F>(111632)(obj_,hwnd,w,h); }
    long SetClientSize(long hwnd,long w,long h) { using F=long(WINAPI*)(long,long,long,long); return at<F>(111136)(obj_,hwnd,w,h); }
    long SetWindowText(long hwnd,PCSTR text) { using F=long(WINAPI*)(long,long,PCSTR); return at<F>(123648)(obj_,hwnd,text); }
    const char *GetWindowTitle(long hwnd) { using F=PCSTR(WINAPI*)(long,long); return at<F>(103584)(obj_,hwnd); }
    const char *GetWindowClass(long hwnd) { using F=PCSTR(WINAPI*)(long,long); return at<F>(117168)(obj_,hwnd); }
    long ClientToScreen(long hwnd,long *x,long *y) { using F=long(WINAPI*)(long,long,long*,long*); return at<F>(105664)(obj_,hwnd,x,y); }
    long ScreenToClient(long hwnd,long *x,long *y) { using F=long(WINAPI*)(long,long,long*,long*); return at<F>(102992)(obj_,hwnd,x,y); }
    const char *GetWindowProcessPath(long hwnd) { using F=PCSTR(WINAPI*)(long,long); return at<F>(126480)(obj_,hwnd); }
    const char *GetRealPath(PCSTR path) { using F=PCSTR(WINAPI*)(long,PCSTR); return at<F>(108208)(obj_,path); }

    const char *Md5(PCSTR str) { using F=PCSTR(WINAPI*)(long,PCSTR); return at<F>(103168)(obj_,str); }
    long GetResultCount(PCSTR str) { using F=long(WINAPI*)(long,PCSTR); return at<F>(116496)(obj_,str); }
    long GetResultPos(PCSTR str,long index,long *x,long *y) { using F=long(WINAPI*)(long,PCSTR,long,long*,long*); return at<F>(122784)(obj_,str,index,x,y); }
    const char *IntToData(LONGLONG v,long type) { using F=PCSTR(WINAPI*)(long,LONGLONG,long); return at<F>(101968)(obj_,v,type); }
    const char *FloatToData(float v) { using F=PCSTR(WINAPI*)(long,float); return at<F>(122848)(obj_,v); }
    const char *DoubleToData(double v) { using F=PCSTR(WINAPI*)(long,double); return at<F>(108144)(obj_,v); }
    const char *StringToData(PCSTR v,long type) { using F=PCSTR(WINAPI*)(long,PCSTR,long); return at<F>(111008)(obj_,v,type); }
    long GetLocale() { using F=long(WINAPI*)(long); return at<F>(104336)(obj_); }
    long CheckUAC() { using F=long(WINAPI*)(long); return at<F>(113680)(obj_); }

    long GetWindowState(long hwnd,long flag) { using F=long(WINAPI*)(long,long,long); return at<F>(104432)(obj_,hwnd,flag); }
    long GetWindow(long hwnd,long flag) { using F=long(WINAPI*)(long,long,long); return at<F>(109264)(obj_,hwnd,flag); }
    long GetForegroundFocus() { using F=long(WINAPI*)(long); return at<F>(110480)(obj_); }
    long SetWindowState(long hwnd,long flag) { using F=long(WINAPI*)(long,long,long); return at<F>(114752)(obj_,hwnd,flag); }
    const char *EnumProcess(PCSTR name) { using F=PCSTR(WINAPI*)(long,PCSTR); return at<F>(126688)(obj_,name); }
    const char *EnumWindow(long parent,PCSTR title,PCSTR class_name,long filter) { using F=PCSTR(WINAPI*)(long,long,PCSTR,PCSTR,long); return at<F>(121360)(obj_,parent,title,class_name,filter); }
    long FindWindowByProcessId(long pid,PCSTR class_name,PCSTR title_name) { using F=long(WINAPI*)(long,long,PCSTR,PCSTR); return at<F>(113728)(obj_,pid,class_name,title_name); }
    long FindWindowByProcess(PCSTR process_name,PCSTR class_name,PCSTR title_name) { using F=long(WINAPI*)(long,PCSTR,PCSTR,PCSTR); return at<F>(126416)(obj_,process_name,class_name,title_name); }
    const char *EnumWindowByProcessId(long pid,PCSTR title,PCSTR class_name,long filter) { using F=PCSTR(WINAPI*)(long,long,PCSTR,PCSTR,long); return at<F>(122400)(obj_,pid,title,class_name,filter); }
    const char *EnumWindowByProcess(PCSTR process_name,PCSTR title,PCSTR class_name,long filter) { using F=PCSTR(WINAPI*)(long,PCSTR,PCSTR,PCSTR,long); return at<F>(124816)(obj_,process_name,title,class_name,filter); }

    long OpenProcess(long pid) { using F=long(WINAPI*)(long,long); return at<F>(108880)(obj_,pid); }
    long TerminateProcess(long pid) { using F=long(WINAPI*)(long,long); return at<F>(106096)(obj_,pid); }
    long FreeProcessMemory(long hwnd) { using F=long(WINAPI*)(long,long); return at<F>(106272)(obj_,hwnd); }
    long VirtualProtectEx(long hwnd,LONGLONG addr,long size,long type,long old_protect) { using F=long(WINAPI*)(long,long,LONGLONG,long,long,long); return at<F>(120496)(obj_,hwnd,addr,size,type,old_protect); }
    const char *VirtualQueryEx(long hwnd,LONGLONG addr,long pmbi) { using F=PCSTR(WINAPI*)(long,long,LONGLONG,long); return at<F>(110832)(obj_,hwnd,addr,pmbi); }

    long GetKeyState(long vk) { using F=long(WINAPI*)(long,long); return at<F>(105872)(obj_,vk); }
    long GetMouseSpeed() { using F=long(WINAPI*)(long); return at<F>(110640)(obj_); }
    long SetMouseSpeed(long speed) { using F=long(WINAPI*)(long,long); return at<F>(118928)(obj_,speed); }
    long SetWindowTransparent(long hwnd,long trans) { using F=long(WINAPI*)(long,long,long); return at<F>(121024)(obj_,hwnd,trans); }
    long Beep(long fre,long delay) { using F=long(WINAPI*)(long,long,long); return at<F>(104624)(obj_,fre,delay); }
    long RunApp(PCSTR path,long mode) { using F=long(WINAPI*)(long,PCSTR,long); return at<F>(116544)(obj_,path,mode); }

    long SetPath(PCSTR path) { using F=long(WINAPI*)(long,PCSTR); return at<F>(111856)(obj_,path); }
    const char *GetPath() { using F=PCSTR(WINAPI*)(long); return at<F>(122912)(obj_); }
    const char *GetBasePath() { using F=PCSTR(WINAPI*)(long); return at<F>(103536)(obj_); }
    long GetID() { using F=long(WINAPI*)(long); return at<F>(105056)(obj_); }
    long GetDmCount() { using F=long(WINAPI*)(long); return at<F>(114944)(obj_); }
    long SetEnumWindowDelay(long delay) { using F=long(WINAPI*)(long,long); return at<F>(109088)(obj_,delay); }
    long SetShowErrorMsg(long show) { using F=long(WINAPI*)(long,long); return at<F>(118560)(obj_,show); }

    LONGLONG ReadInt(long hwnd,PCSTR addr,long type) { using F=LONGLONG(WINAPI*)(long,long,PCSTR,long); return at<F>(123856)(obj_,hwnd,addr,type); }
    long WriteInt(long hwnd,PCSTR addr,long type,LONGLONG v) { using F=long(WINAPI*)(long,long,PCSTR,long,LONGLONG); return at<F>(122064)(obj_,hwnd,addr,type,v); }
    float ReadFloat(long hwnd,PCSTR addr) { using F=float(WINAPI*)(long,long,PCSTR); return at<F>(118864)(obj_,hwnd,addr); }
    long WriteFloat(long hwnd,PCSTR addr,float v) { using F=long(WINAPI*)(long,long,PCSTR,float); return at<F>(119440)(obj_,hwnd,addr,v); }
    double ReadDouble(long hwnd,PCSTR addr) { using F=double(WINAPI*)(long,long,PCSTR); return at<F>(115088)(obj_,hwnd,addr); }
    long WriteDouble(long hwnd,PCSTR addr,double v) { using F=long(WINAPI*)(long,long,PCSTR,double); return at<F>(106512)(obj_,hwnd,addr,v); }
    const char *ReadString(long hwnd,PCSTR addr,long type,long len) { using F=PCSTR(WINAPI*)(long,long,PCSTR,long,long); return at<F>(120960)(obj_,hwnd,addr,type,len); }
    long WriteString(long hwnd,PCSTR addr,long type,PCSTR v) { using F=long(WINAPI*)(long,long,PCSTR,long,PCSTR); return at<F>(109936)(obj_,hwnd,addr,type,v); }
    const char *ReadStringAddr(long hwnd,LONGLONG addr,long type,long len) { using F=PCSTR(WINAPI*)(long,long,LONGLONG,long,long); return at<F>(119312)(obj_,hwnd,addr,type,len); }
    long WriteStringAddr(long hwnd,LONGLONG addr,long type,PCSTR v) { using F=long(WINAPI*)(long,long,LONGLONG,long,PCSTR); return at<F>(107808)(obj_,hwnd,addr,type,v); }
    const char *ReadData(long hwnd,PCSTR addr,long len) { using F=PCSTR(WINAPI*)(long,long,PCSTR,long); return at<F>(119744)(obj_,hwnd,addr,len); }
    long WriteData(long hwnd,PCSTR addr,PCSTR data) { using F=long(WINAPI*)(long,long,PCSTR,PCSTR); return at<F>(120080)(obj_,hwnd,addr,data); }

    const char *ExcludePos(PCSTR all_pos,long type,long x1,long y1,long x2,long y2) { using F=PCSTR(WINAPI*)(long,PCSTR,long,long,long,long,long); return at<F>(112432)(obj_,all_pos,type,x1,y1,x2,y2); }
    const char *FindNearestPos(PCSTR all_pos,long type,long x,long y) { using F=PCSTR(WINAPI*)(long,PCSTR,long,long,long); return at<F>(115152)(obj_,all_pos,type,x,y); }
    const char *SortPosDistance(PCSTR all_pos,long type,long x,long y) { using F=PCSTR(WINAPI*)(long,PCSTR,long,long,long); return at<F>(117776)(obj_,all_pos,type,x,y); }

    long GetWordResultCount(PCSTR str) { using F=long(WINAPI*)(long,PCSTR); return at<F>(118768)(obj_,str); }
    long GetWordResultPos(PCSTR str,long index,long *x,long *y) { using F=long(WINAPI*)(long,PCSTR,long,long*,long*); return at<F>(115520)(obj_,str,index,x,y); }
    const char *GetWordResultStr(PCSTR str,long index) { using F=PCSTR(WINAPI*)(long,PCSTR,long); return at<F>(122608)(obj_,str,index); }
    long WaitKey(long key_code,long time_out) { using F=long(WINAPI*)(long,long,long); return at<F>(119248)(obj_,key_code,time_out); }
    long SetKeypadDelay(PCSTR type,long delay) { using F=long(WINAPI*)(long,PCSTR,long); return at<F>(119152)(obj_,type,delay); }
    long SetMouseDelay(PCSTR type,long delay) { using F=long(WINAPI*)(long,PCSTR,long); return at<F>(111472)(obj_,type,delay); }
    long KeyPress(long vk) { using F=long(WINAPI*)(long,long); return at<F>(104224)(obj_,vk); }
    long KeyDown(long vk) { using F=long(WINAPI*)(long,long); return at<F>(110784)(obj_,vk); }
    long KeyUp(long vk) { using F=long(WINAPI*)(long,long); return at<F>(119696)(obj_,vk); }
    long LeftClick() { using F=long(WINAPI*)(long); return at<F>(105440)(obj_); }
    long RightClick() { using F=long(WINAPI*)(long); return at<F>(101920)(obj_); }
    long MiddleClick() { using F=long(WINAPI*)(long); return at<F>(105824)(obj_); }
    long LeftDoubleClick() { using F=long(WINAPI*)(long); return at<F>(113792)(obj_); }
    long LeftDown() { using F=long(WINAPI*)(long); return at<F>(104384)(obj_); }
    long LeftUp() { using F=long(WINAPI*)(long); return at<F>(122736)(obj_); }
    long RightDown() { using F=long(WINAPI*)(long); return at<F>(123712)(obj_); }
    long RightUp() { using F=long(WINAPI*)(long); return at<F>(126640)(obj_); }
    long MiddleDown() { using F=long(WINAPI*)(long); return at<F>(124176)(obj_); }
    long MiddleUp() { using F=long(WINAPI*)(long); return at<F>(101264)(obj_); }
    long WheelUp() { using F=long(WINAPI*)(long); return at<F>(126896)(obj_); }
    long WheelDown() { using F=long(WINAPI*)(long); return at<F>(117600)(obj_); }
    long MoveTo(long x,long y) { using F=long(WINAPI*)(long,long,long); return at<F>(119072)(obj_,x,y); }
    long MoveR(long rx,long ry) { using F=long(WINAPI*)(long,long,long); return at<F>(116688)(obj_,rx,ry); }
    const char *MoveToEx(long x,long y,long w,long h) { using F=PCSTR(WINAPI*)(long,long,long,long,long); return at<F>(115328)(obj_,x,y,w,h); }

    long KeyDownChar(PCSTR key_str) { using F=long(WINAPI*)(long,PCSTR); return at<F>(103328)(obj_,key_str); }
    long KeyUpChar(PCSTR key_str) { using F=long(WINAPI*)(long,PCSTR); return at<F>(103776)(obj_,key_str); }
    long KeyPressChar(PCSTR key_str) { using F=long(WINAPI*)(long,PCSTR); return at<F>(104064)(obj_,key_str); }
    long KeyPressStr(PCSTR key_str,long delay) { using F=long(WINAPI*)(long,PCSTR,long); return at<F>(112176)(obj_,key_str,delay); }
    long SendPaste(long hwnd) { using F=long(WINAPI*)(long,long); return at<F>(107936)(obj_,hwnd); }

    long SendString(long hwnd,PCSTR str) { using F=long(WINAPI*)(long,long,PCSTR); return at<F>(119888)(obj_,hwnd,str); }

    const char *GetDir(long type) { using F=PCSTR(WINAPI*)(long,long); return at<F>(114624)(obj_,type); }
    long GetOsType() { using F=long(WINAPI*)(long); return at<F>(126800)(obj_); }
    const char *GetProcessInfo(long pid) { using F=PCSTR(WINAPI*)(long,long); return at<F>(106576)(obj_,pid); }

    long GetSpecialWindow(long flag) { using F=long(WINAPI*)(long,long); return at<F>(111904)(obj_,flag); }
    const char *GetCommandLine(long hwnd) { using F=PCSTR(WINAPI*)(long,long); return at<F>(102432)(obj_,hwnd); }
    const char *GetDiskModel(long index) { using F=PCSTR(WINAPI*)(long,long); return at<F>(124624)(obj_,index); }
    const char *GetDiskReversion(long index) { using F=PCSTR(WINAPI*)(long,long); return at<F>(117120)(obj_,index); }
    const char *GetDiskSerial(long index) { using F=PCSTR(WINAPI*)(long,long); return at<F>(101376)(obj_,index); }
    const char *GetDisplayInfo() { using F=PCSTR(WINAPI*)(long); return at<F>(113968)(obj_); }

    long EnableGetColorByCapture(long enable) { using F=long(WINAPI*)(long,long); return at<F>(109584)(obj_,enable); }
    const char *GetColor(long x,long y) { using F=PCSTR(WINAPI*)(long,long,long); return at<F>(112784)(obj_,x,y); }
    const char *GetColorBGR(long x,long y) { using F=PCSTR(WINAPI*)(long,long,long); return at<F>(122464)(obj_,x,y); }
    const char *GetColorHSV(long x,long y) { using F=PCSTR(WINAPI*)(long,long,long); return at<F>(105600)(obj_,x,y); }
    const char *GetAveRGB(long x1,long y1,long x2,long y2) { using F=PCSTR(WINAPI*)(long,long,long,long,long); return at<F>(126016)(obj_,x1,y1,x2,y2); }
    const char *GetAveHSV(long x1,long y1,long x2,long y2) { using F=PCSTR(WINAPI*)(long,long,long,long,long); return at<F>(113344)(obj_,x1,y1,x2,y2); }
    long CmpColor(long x,long y,PCSTR color,double sim) { using F=long(WINAPI*)(long,long,long,PCSTR,double); return at<F>(120256)(obj_,x,y,color,sim); }
    long GetColorNum(long x1,long y1,long x2,long y2,PCSTR color,double sim) { using F=long(WINAPI*)(long,long,long,long,long,PCSTR,double); return at<F>(105296)(obj_,x1,y1,x2,y2,color,sim); }
    long FindColor(long x1,long y1,long x2,long y2,PCSTR color,double sim,long dir,long *x,long *y) { using F=long(WINAPI*)(long,long,long,long,long,PCSTR,double,long,long*,long*); return at<F>(115584)(obj_,x1,y1,x2,y2,color,sim,dir,x,y); }
    const char *FindColorE(long x1,long y1,long x2,long y2,PCSTR color,double sim,long dir) { using F=PCSTR(WINAPI*)(long,long,long,long,long,PCSTR,double,long); return at<F>(121088)(obj_,x1,y1,x2,y2,color,sim,dir); }
    const char *FindColorEx(long x1,long y1,long x2,long y2,PCSTR color,double sim,long dir) { using F=PCSTR(WINAPI*)(long,long,long,long,long,PCSTR,double,long); return at<F>(114304)(obj_,x1,y1,x2,y2,color,sim,dir); }

    long FindMultiColor(long x1,long y1,long x2,long y2,PCSTR first_color,PCSTR offset_color,double sim,long dir,long *x,long *y) { using F=long(WINAPI*)(long,long,long,long,long,PCSTR,PCSTR,double,long,long*,long*); return at<F>(120864)(obj_,x1,y1,x2,y2,first_color,offset_color,sim,dir,x,y); }
    const char *FindMultiColorE(long x1,long y1,long x2,long y2,PCSTR first_color,PCSTR offset_color,double sim,long dir) { using F=PCSTR(WINAPI*)(long,long,long,long,long,PCSTR,PCSTR,double,long); return at<F>(105104)(obj_,x1,y1,x2,y2,first_color,offset_color,sim,dir); }
    const char *FindMultiColorEx(long x1,long y1,long x2,long y2,PCSTR first_color,PCSTR offset_color,double sim,long dir) { using F=PCSTR(WINAPI*)(long,long,long,long,long,PCSTR,PCSTR,double,long); return at<F>(109792)(obj_,x1,y1,x2,y2,first_color,offset_color,sim,dir); }
    long FindShape(long x1,long y1,long x2,long y2,PCSTR offset_color,double sim,long dir,long *x,long *y) { using F=long(WINAPI*)(long,long,long,long,long,PCSTR,double,long,long*,long*); return at<F>(123456)(obj_,x1,y1,x2,y2,offset_color,sim,dir,x,y); }
    const char *FindShapeE(long x1,long y1,long x2,long y2,PCSTR offset_color,double sim,long dir) { using F=PCSTR(WINAPI*)(long,long,long,long,long,PCSTR,double,long); return at<F>(123760)(obj_,x1,y1,x2,y2,offset_color,sim,dir); }
    const char *FindShapeEx(long x1,long y1,long x2,long y2,PCSTR offset_color,double sim,long dir) { using F=PCSTR(WINAPI*)(long,long,long,long,long,PCSTR,double,long); return at<F>(102704)(obj_,x1,y1,x2,y2,offset_color,sim,dir); }
    long FindMulColor(long x1,long y1,long x2,long y2,PCSTR color,double sim) { using F=long(WINAPI*)(long,long,long,long,long,PCSTR,double); return at<F>(102144)(obj_,x1,y1,x2,y2,color,sim); }

private:
    template<class T>
    T at(ULONG_PTR rva) const {
        return reinterpret_cast<T>(reinterpret_cast<ULONG_PTR>(module_) + rva);
    }

    HMODULE module_ = nullptr;
    long obj_ = 0;
};
