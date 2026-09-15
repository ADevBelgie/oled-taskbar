// OLED Taskbar shell bridge. GPL-3.0-or-later.
// Monitor-selection hooks adapted from m417z's GPLv3 Windhawk mod:
// https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-primary-on-secondary-monitor.wh.cpp
// MinHook is separately licensed under its included BSD-style license.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <intrin.h>
#include <atomic>
#include <cwchar>
#include <cstdlib>
#include <cstring>
#include "NativeProtocol.h"
#include "vendor/minhook-1.3.4/include/MinHook.h"
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"wtsapi32.lib")
#pragma comment(lib,"shell32.lib")

namespace {
std::atomic<bool> enabled{false}, restoring{false};
std::atomic<DWORD> requestedOwner{0};
std::atomic<bool> requestedEnable{false};
std::atomic<DWORD> requestedOriginalAutoHide{0};
std::atomic<ULONG> requestedGeneration{0};
std::atomic<HANDLE> wakeEvent{nullptr};
std::atomic<HMONITOR> cachedTarget{nullptr};
INIT_ONCE once = INIT_ONCE_STATIC_INIT;
HMODULE selfModule;
bool explorer = false, startHost = false, hooksInstalled = false, hookFailure = false;
wchar_t directory[MAX_PATH] = {}, iniPath[MAX_PATH] = {};
SRWLOCK responseLock = SRWLOCK_INIT;
HANDLE responseMapping = nullptr;
OledTaskbarNative::NativeStatus* responseView = nullptr;
ULONG responseGeneration = 0;
ULONGLONG responseRequestId = 0;
decltype(&MonitorFromPoint) pointOriginal = MonitorFromPoint;
decltype(&MonitorFromRect) rectOriginal = MonitorFromRect;
decltype(&MonitorFromWindow) windowOriginal = MonitorFromWindow;
decltype(&EnumDisplayDevicesW) devicesOriginal = EnumDisplayDevicesW;
using StuckFn = HRESULT(WINAPI*)(void*, HMONITOR);
StuckFn stuckOriginal = nullptr;
// Matching Taskbar.pdb symbol:
//   TrayCommon::GetMultiMonTaskbarState(void)
//   ?GetMultiMonTaskbarState@TrayCommon@@YA?AW4MultiMonTaskbarState@1@XZ
// The current implementation returns 0 for a single-monitor system, 1 when
// multi-monitor taskbars are disabled, and 2 when they are enabled. The RVA is
// resolved into bridge.ini by symbol name; it is never hard-coded here.
using MultiMonStateFn = int(*)();
MultiMonStateFn multiMonStateOriginal = nullptr;

void Log(const wchar_t* message) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\native-%lu.log", directory, GetCurrentProcessId());
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t line[1200]; swprintf_s(line,L"%02u:%02u:%02u %s\r\n",t.wHour,t.wMinute,t.wSecond,message);
    DWORD written; WriteFile(f,line,(DWORD)(wcslen(line)*sizeof(wchar_t)),&written,nullptr); CloseHandle(f);
}
void SignalReady() {
    wchar_t name[100]; swprintf_s(name,L"Local\\OledTaskbar.Native.Ready.%lu",GetCurrentProcessId());
    HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,name); if(ready){SetEvent(ready);CloseHandle(ready);}
}
void Status(ULONG generation, bool active, DWORD result, const wchar_t* error) {
    bool published=false;
    AcquireSRWLockExclusive(&responseLock);
    if(responseView && responseGeneration==generation &&
       responseView->Magic==OledTaskbarNative::kProtocolMagic &&
       responseView->Version==OledTaskbarNative::kProtocolVersion &&
       responseView->Size==sizeof(*responseView) &&
       responseView->RequestId==responseRequestId) {
        responseView->Active=active?1:0;
        responseView->ResultCode=result;
        wcsncpy_s(responseView->Error,OledTaskbarNative::kErrorCapacity,error,_TRUNCATE);
        responseView->ResponseRequestId=responseRequestId;
        InterlockedExchange(&responseView->Completion,OledTaskbarNative::kResponseComplete);
        published=true;
    }
    ReleaseSRWLockExclusive(&responseLock);
    // Publish every response byte before waking the waiting controller.
    if(published) SignalReady();
    Log(error[0]?error:(active?L"Enabled":L"Disabled"));
}
void RejectConfiguration(HANDLE mapping, OledTaskbarNative::NativeStatus* view,
                         bool writableProtocol, const wchar_t* error) {
    if(writableProtocol) {
        view->Active=enabled.load()?1:0;
        view->ResultCode=OledTaskbarNative::ResultInvalidRequest;
        wcsncpy_s(view->Error,OledTaskbarNative::kErrorCapacity,error,_TRUNCATE);
        view->ResponseRequestId=view->RequestId;
        InterlockedExchange(&view->Completion,OledTaskbarNative::kResponseComplete);
    }
    UnmapViewOfFile(view);CloseHandle(mapping);
    SignalReady();Log(error);
}
ULONG AcceptConfiguration(WPARAM messageRequestId, LPARAM messageVersion) {
    wchar_t name[100];
    swprintf_s(name,L"Local\\OledTaskbar.Native.Status.%lu",GetCurrentProcessId());
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,name);
    if(!mapping) {SignalReady();Log(L"Native status mapping could not be opened");return 0;}
    auto view=(OledTaskbarNative::NativeStatus*)MapViewOfFile(mapping,FILE_MAP_READ|FILE_MAP_WRITE,
                                                             0,0,sizeof(OledTaskbarNative::NativeStatus));
    if(!view) {CloseHandle(mapping);SignalReady();Log(L"Native status mapping could not be viewed");return 0;}

    // Copy the complete immutable request before publishing any configuration
    // to the worker. The helper owns the mapping and must finish the request
    // before it posts the configure message.
    OledTaskbarNative::NativeStatus request{};
    memcpy(&request,view,sizeof(request));
    bool protocol=request.Magic==OledTaskbarNative::kProtocolMagic &&
                  request.Version==OledTaskbarNative::kProtocolVersion &&
                  request.Size==sizeof(request);
    // A delayed message can arrive after the helper has reused this process's
    // named mapping for a newer request. Never let it reject or overwrite that
    // newer response.
    if(protocol && request.RequestId!=(ULONGLONG)messageRequestId) {
        UnmapViewOfFile(view);CloseHandle(mapping);Log(L"Ignored stale native configuration message");return 0;
    }
    bool duplicate=false;
    AcquireSRWLockShared(&responseLock);
    duplicate=responseView && responseRequestId==request.RequestId;
    ReleaseSRWLockShared(&responseLock);
    if(protocol && request.Completion!=OledTaskbarNative::kResponsePending) {
        bool completed=request.Completion==OledTaskbarNative::kResponseComplete &&
                       request.ResponseRequestId==request.RequestId;
        UnmapViewOfFile(view);CloseHandle(mapping);
        if(completed) SignalReady();
        Log(L"Ignored completed native configuration message");return 0;
    }
    if(protocol && duplicate) {
        UnmapViewOfFile(view);CloseHandle(mapping);Log(L"Ignored duplicate native configuration message");return 0;
    }
    bool valid=protocol && messageVersion==OledTaskbarNative::kProtocolVersion &&
               request.RequestId!=0 && request.TargetPid==GetCurrentProcessId() &&
               request.OwnerPid!=0 &&
               (request.DesiredEnabled==0 || request.DesiredEnabled==1) &&
               (request.OriginalAutoHide==0 || request.OriginalAutoHide==ABS_AUTOHIDE) &&
               request.Reserved==0;
    if(!valid) {
        RejectConfiguration(mapping,view,protocol,L"Invalid or mismatched native configuration request");
        return 0;
    }

    AcquireSRWLockExclusive(&responseLock);
    if(responseView) UnmapViewOfFile(responseView);
    if(responseMapping) CloseHandle(responseMapping);
    responseView=view;responseMapping=mapping;responseRequestId=request.RequestId;
    requestedOwner=request.OwnerPid;
    requestedEnable=request.DesiredEnabled!=0;
    requestedOriginalAutoHide=request.OriginalAutoHide;
    ULONG generation=requestedGeneration.fetch_add(1)+1;
    responseGeneration=generation;
    // Reset only response fields. Request fields remain available for the
    // controller to correlate and audit after acknowledgement.
    responseView->Active=enabled.load()?1:0;
    responseView->ResultCode=OledTaskbarNative::ResultInternalFailure;
    responseView->Error[0]=0;
    responseView->ResponseRequestId=0;
    InterlockedExchange(&responseView->Completion,OledTaskbarNative::kResponsePending);
    ReleaseSRWLockExclusive(&responseLock);
    return generation;
}
struct ConfigurationSnapshot {
    ULONG Generation;
    DWORD Owner;
    bool Enable;
    DWORD OriginalAutoHide;
};
ConfigurationSnapshot CurrentConfiguration() {
    AcquireSRWLockShared(&responseLock);
    ConfigurationSnapshot result{requestedGeneration.load(),requestedOwner.load(),
                                 requestedEnable.load(),requestedOriginalAutoHide.load()};
    ReleaseSRWLockShared(&responseLock);
    return result;
}
HMONITOR FindTarget() {
    HMONITOR result=nullptr;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM data)->BOOL {
        MONITORINFOEXW info={}; info.cbSize=sizeof(info);
        if(GetMonitorInfoW(monitor,&info) && !(info.dwFlags&MONITORINFOF_PRIMARY)) {
            *(HMONITOR*)data=monitor; return FALSE;
        }
        return TRUE;
    },(LPARAM)&result);
    return result;
}
bool InModule(void* address,const wchar_t* name) {
    HMODULE module=nullptr;
    return address && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)address,&module) && module==GetModuleHandleW(name);
}
bool Locked() {
    WTSINFOEXW* info=nullptr; DWORD size=0; bool locked=false;
    if(WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,WTS_CURRENT_SESSION,WTSSessionInfoEx,(LPWSTR*)&info,&size)) {
        locked=size>=sizeof(WTSINFOEXW) && info->Level==1 && info->Data.WTSInfoExLevel1.SessionFlags==WTS_SESSIONSTATE_LOCK;
        WTSFreeMemory(info);
    }
    return locked;
}
HMONITOR Target(void* caller=nullptr,bool ignoreLock=false) {
    if(caller && InModule(caller,L"shell32.dll")) return nullptr;
    if(restoring.load()) return pointOriginal(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
    if(!enabled.load()) return nullptr;
    if(!ignoreLock && Locked()) return nullptr;
    HMONITOR monitor=cachedTarget.load(); MONITORINFO info={sizeof(info)};
    return monitor && GetMonitorInfoW(monitor,&info)?monitor:nullptr;
}
HMONITOR WINAPI PointHook(POINT p,DWORD flags) {
    if(p.x==0 && p.y==0) if(HMONITOR m=Target(_ReturnAddress())) return m;
    return pointOriginal(p,flags);
}
HMONITOR WINAPI RectHook(LPCRECT r,DWORD flags) {
    if(r && r->left==0 && r->top==0 && r->right==0 && r->bottom==0)
        if(HMONITOR m=Target(_ReturnAddress())) return m;
    return rectOriginal(r,flags);
}
HMONITOR WINAPI WindowHook(HWND w,DWORD flags) {
    void* caller=_ReturnAddress();
    if(flags==MONITOR_DEFAULTTOPRIMARY && (InModule(caller,L"StartMenu.dll") || InModule(caller,L"StartDocked.dll")))
        if(HMONITOR m=Target(caller)) return m;
    return windowOriginal(w,flags);
}
BOOL WINAPI DevicesHook(LPCWSTR name,DWORD index,PDISPLAY_DEVICEW device,DWORD flags) {
    BOOL result=devicesOriginal(name,index,device,flags);
    if(result && device && !name) if(HMONITOR m=Target(_ReturnAddress())) {
        MONITORINFOEXW info={}; info.cbSize=sizeof(info);
        if(GetMonitorInfoW(m,&info)) {
            if(wcscmp(device->DeviceName,info.szDevice)==0) device->StateFlags|=DISPLAY_DEVICE_PRIMARY_DEVICE;
            else device->StateFlags&=~DISPLAY_DEVICE_PRIMARY_DEVICE;
        }
    }
    return result;
}
HRESULT WINAPI StuckHook(void* object,HMONITOR monitor) {
    if(HMONITOR m=Target(nullptr,true)) monitor=m;
    return stuckOriginal(object,monitor);
}
int MultiMonStateHook() {
    // Keep Explorer's persisted preference untouched. While the bridge owns the
    // layout, make only taskbar.dll observe "show on all displays" as disabled.
    if(enabled.load() && !restoring.load()) return 1;
    return multiMonStateOriginal();
}
bool Refresh() {
    bool ok=true;
    if(explorer) {
        HWND bar=FindWindowW(L"Shell_TrayWnd",nullptr); DWORD pid=0;
        if(bar) GetWindowThreadProcessId(bar,&pid);
        if(bar && pid==GetCurrentProcessId()) {
            DWORD_PTR ignored=0;
            ok=SendMessageTimeoutW(bar,0x5B8,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,2500,&ignored)!=0;
        } else ok=false;
    }
    PostMessageW(HWND_BROADCAST,RegisterWindowMessageW(L"SHELLHOOK"),35,0);
    return ok;
}
bool RestoreSavedPreferences(DWORD originalAutoHide, bool originalKnown) {
    // The controller's persisted desired State remains Enabled for next logon.
    // The bridge receives the auto-hide baseline from its owning controller and
    // never reads or writes user preferences through the registry.
    if(!explorer) return true;
    if(!originalKnown || (originalAutoHide!=0 && originalAutoHide!=ABS_AUTOHIDE)) return false;
    APPBARDATA data={}; data.cbSize=sizeof(data);data.hWnd=FindWindowW(L"Shell_TrayWnd",nullptr);
    data.lParam=originalAutoHide;
    SHAppBarMessage(ABM_SETSTATE,&data);
    bool verified=(SHAppBarMessage(ABM_GETSTATE,&data)&ABS_AUTOHIDE)==originalAutoHide;
    return verified;
}
DWORD Number(const wchar_t* name) {
    wchar_t value[80]; GetPrivateProfileStringW(L"Symbols",name,L"0",value,80,iniPath);
    return wcstoul(value,nullptr,0);
}
void* TaskbarAddress(const wchar_t* rvaName) {
    HMODULE taskbar=GetModuleHandleW(L"taskbar.dll");
    if(!taskbar) return nullptr;
    auto dos=(IMAGE_DOS_HEADER*)taskbar;
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x100000) return nullptr;
    auto nt=(IMAGE_NT_HEADERS*)((BYTE*)taskbar+dos->e_lfanew);
    DWORD rva=Number(rvaName);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 ||
       nt->FileHeader.TimeDateStamp!=Number(L"TaskbarTimestamp") ||
       nt->OptionalHeader.SizeOfImage!=Number(L"TaskbarSize") || !rva || rva>=nt->OptionalHeader.SizeOfImage) return nullptr;
    auto section=IMAGE_FIRST_SECTION(nt);
    for(WORD i=0;i<nt->FileHeader.NumberOfSections;i++,section++) {
        if(rva>=section->VirtualAddress && rva-section->VirtualAddress<section->Misc.VirtualSize &&
           (section->Characteristics&IMAGE_SCN_MEM_EXECUTE)) return (BYTE*)taskbar+rva;
    }
    return nullptr;
}
template<class T> bool Hook(void* address,void* replacement,T* original) {
    return MH_CreateHook(address,replacement,(void**)original)==MH_OK;
}
bool InstallHooks() {
    if(hooksInstalled) return true;
    if(hookFailure) return false;
    void* stuck=explorer?TaskbarAddress(L"TaskbarRva"):nullptr;
    void* multiMonState=explorer?TaskbarAddress(L"MultiMonRva"):nullptr;
    if(explorer && (!stuck || !multiMonState)) {Log(L"Taskbar symbol identity/address validation failed");return false;}
    if(MH_Initialize()!=MH_OK) {hookFailure=true;return false;}
    bool ok=true;
    if(startHost) ok=Hook((void*)MonitorFromWindow,(void*)WindowHook,&windowOriginal);
    else {
        ok=Hook((void*)MonitorFromPoint,(void*)PointHook,&pointOriginal) &&
           Hook((void*)MonitorFromRect,(void*)RectHook,&rectOriginal) &&
           Hook((void*)EnumDisplayDevicesW,(void*)DevicesHook,&devicesOriginal);
        if(ok && explorer) ok=Hook(stuck,(void*)StuckHook,&stuckOriginal) &&
                              Hook(multiMonState,(void*)MultiMonStateHook,&multiMonStateOriginal);
    }
    // Keep all hooks inert until the complete set has been created successfully.
    if(ok) ok=MH_EnableHook(MH_ALL_HOOKS)==MH_OK;
    if(!ok) { hookFailure=true; Log(L"MinHook installation failed; behavior remains disabled");return false; }
    hooksInstalled=true; return true;
}
DWORD WINAPI Worker(void*) {
    GetModuleFileNameW(selfModule,directory,MAX_PATH);
    wchar_t* slash=wcsrchr(directory,L'\\'); if(slash)*slash=0;
    swprintf_s(iniPath,L"%s\\bridge.ini",directory);
    wchar_t process[MAX_PATH]; GetModuleFileNameW(nullptr,process,MAX_PATH);
    const wchar_t* base=wcsrchr(process,L'\\'); base=base?base+1:process;
    explorer=_wcsicmp(base,L"explorer.exe")==0;
    startHost=_wcsicmp(base,L"StartMenuExperienceHost.exe")==0;
    if(!explorer && !startHost && _wcsicmp(base,L"ShellHost.exe")!=0) {
        Status(requestedGeneration.load(),false,OledTaskbarNative::ResultInternalFailure,L"Unsupported process");return 0;
    }
    HANDLE owner=nullptr; DWORD ownerPid=0; ULONG ownerGeneration=0;
    DWORD ownerOriginalAutoHide=0; bool ownerOriginalKnown=false;
    for(;;) {
        HANDLE waits[2]={wakeEvent.load(),owner};
        ULONG waitedOwnerGeneration=ownerGeneration;
        DWORD wait=WaitForMultipleObjects(owner?2:1,waits,FALSE,INFINITE);
        ConfigurationSnapshot configuration=CurrentConfiguration();
        ULONG generation=configuration.Generation;
        if(wait==WAIT_FAILED) {
            enabled=false;restoring=true;
            RestoreSavedPreferences(ownerOriginalAutoHide,ownerOriginalKnown);Refresh();restoring=false;
            Status(generation,false,OledTaskbarNative::ResultInternalFailure,L"Native wait failed");return 0;
        }
        bool ownerDied=owner && wait==WAIT_OBJECT_0+1;
        DWORD desiredPid=configuration.Owner;
        if(ownerDied) {
            bool wasEnabled=enabled.exchange(false); restoring=wasEnabled;
            bool preferences=!wasEnabled || RestoreSavedPreferences(ownerOriginalAutoHide,ownerOriginalKnown);
            bool restored=(!wasEnabled || Refresh()) && preferences; restoring=false;
            Status(waitedOwnerGeneration,false,restored?OledTaskbarNative::ResultSuccess:OledTaskbarNative::ResultRefreshFailure,
                   restored?L"":L"Owner exited; shell placement or saved preferences could not be restored");
            CloseHandle(owner);owner=nullptr;ownerPid=0;
            ownerGeneration=0;ownerOriginalAutoHide=0;ownerOriginalKnown=false;
            continue;
        }
        if(desiredPid!=ownerPid) {
            if(owner)CloseHandle(owner);
            owner=desiredPid?OpenProcess(SYNCHRONIZE,FALSE,desiredPid):nullptr;
            ownerPid=owner?desiredPid:0;
            ownerGeneration=owner?generation:0;
            ownerOriginalAutoHide=configuration.OriginalAutoHide;
            ownerOriginalKnown=owner!=nullptr;
        } else if(owner) ownerGeneration=generation;
        // Each accepted command carries the controller's authoritative saved
        // baseline. Refresh it even when the owner PID stays the same across a
        // disable/re-enable cycle.
        if(owner) {
            ownerOriginalAutoHide=configuration.OriginalAutoHide;
            ownerOriginalKnown=true;
        }
        bool ownerUnavailable=configuration.Enable && (!owner || WaitForSingleObject(owner,0)!=WAIT_TIMEOUT);
        bool want=configuration.Enable && !ownerUnavailable;
        cachedTarget=FindTarget();
        if(want && !cachedTarget.load()) {
            enabled=false;restoring=true;Refresh();restoring=false;
            Status(generation,false,OledTaskbarNative::ResultTargetUnavailable,L"Secondary monitor unavailable");continue;
        }
        if(want && !InstallHooks()) {
            enabled=false;
            Status(generation,false,OledTaskbarNative::ResultHookFailure,L"Native hooks could not be installed; exact matching symbols are required");continue;
        }
        enabled=want;restoring=!want;
        bool refreshed=Refresh();restoring=false;
        if(!refreshed && want) {
            enabled=false;restoring=true;Refresh();restoring=false;
            Status(generation,false,OledTaskbarNative::ResultRefreshFailure,L"Taskbar refresh failed; behavior disabled");
        } else Status(generation,want,!refreshed?OledTaskbarNative::ResultRefreshFailure:
                      ownerUnavailable?OledTaskbarNative::ResultOwnerUnavailable:OledTaskbarNative::ResultSuccess,
                      !refreshed?L"Taskbar restoration could not be confirmed":
                      ownerUnavailable?L"Controller process is unavailable":L"");
    }
}
BOOL CALLBACK Initialize(PINIT_ONCE,PVOID,PVOID*) {
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
       (LPCWSTR)&Initialize,&selfModule)) return FALSE;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); if(!event)return FALSE;
    wakeEvent=event;
    HANDLE worker=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
    if(!worker){wakeEvent=nullptr;CloseHandle(event);return FALSE;}
    CloseHandle(worker);return TRUE;
}
}

extern "C" __declspec(dllexport) LRESULT CALLBACK MessageHook(int code,WPARAM wParam,LPARAM lParam) {
    if(code>=0 && wParam==PM_REMOVE && lParam) {
        MSG* message=(MSG*)lParam;
        static UINT configure=RegisterWindowMessageW(L"OledTaskbar.Native.Configure");
        if(message->message==configure) {
            ULONG generation=AcceptConfiguration(message->wParam,message->lParam);
            if(generation) {
                if(InitOnceExecuteOnce(&once,Initialize,nullptr,nullptr)) SetEvent(wakeEvent.load());
                else Status(generation,false,OledTaskbarNative::ResultInternalFailure,L"Native worker initialization failed");
            }
        } else if(message->message==WM_DISPLAYCHANGE && wakeEvent.load()) {
            SetEvent(wakeEvent.load());
        }
    }
    return CallNextHookEx(nullptr,code,wParam,lParam);
}
