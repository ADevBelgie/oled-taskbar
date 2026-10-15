// OLED Taskbar native diagnostics, symbol preparation, and targeted shell bridge loader.
#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include "NativeProtocol.h"

static std::wstring directory() { wchar_t p[32768]; GetModuleFileNameW(nullptr,p,32768); std::wstring s=p; return s.substr(0,s.find_last_of(L'\\')); }
static std::wstring systemFile(const wchar_t* name) { wchar_t p[MAX_PATH]; GetSystemDirectoryW(p,MAX_PATH); return std::wstring(p)+L"\\"+name; }
struct Image {
 HANDLE file=INVALID_HANDLE_VALUE,mapping=nullptr; BYTE* data=nullptr;
 IMAGE_NT_HEADERS64* nt=nullptr;
 explicit Image(const std::wstring& path) {
  file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
  if(file==INVALID_HANDLE_VALUE) return;
  mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);
  if(!mapping)return; data=(BYTE*)MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);
  if(data) {auto dos=(IMAGE_DOS_HEADER*)data; if(dos->e_magic==IMAGE_DOS_SIGNATURE) nt=(IMAGE_NT_HEADERS64*)(data+dos->e_lfanew);}
 }
 ~Image(){if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
 BYTE* rva(DWORD value) {
  if(!nt)return nullptr; auto sec=IMAGE_FIRST_SECTION(nt);
  for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++)if(value>=sec[i].VirtualAddress&&value<sec[i].VirtualAddress+std::max(sec[i].Misc.VirtualSize,sec[i].SizeOfRawData))return data+sec[i].PointerToRawData+value-sec[i].VirtualAddress;
  return nullptr;
 }
};
static int pdbInfo() {
 Image im(systemFile(L"taskbar.dll")); if(!im.nt)return 1;
 auto d=im.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG]; auto entries=(IMAGE_DEBUG_DIRECTORY*)im.rva(d.VirtualAddress);
 for(unsigned i=0;entries&&i<d.Size/sizeof(*entries);i++) if(entries[i].Type==IMAGE_DEBUG_TYPE_CODEVIEW) {
  auto cv=im.data+entries[i].PointerToRawData; if(memcmp(cv,"RSDS",4))continue;
  GUID g; memcpy(&g,cv+4,sizeof(g)); DWORD age;memcpy(&age,cv+20,4);
  char key[64];sprintf_s(key,"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",g.Data1,g.Data2,g.Data3,g.Data4[0],g.Data4[1],g.Data4[2],g.Data4[3],g.Data4[4],g.Data4[5],g.Data4[6],g.Data4[7],age);
  std::string name=(char*)(cv+24);name=name.substr(name.find_last_of("\\/")+1);
  std::cout<<"{\"Pdb\":\""<<name<<"\",\"Key\":\""<<key<<"\",\"Timestamp\":"<<im.nt->FileHeader.TimeDateStamp<<",\"Size\":"<<im.nt->OptionalHeader.SizeOfImage<<"}";return 0;
 }
 return 2;
}
struct SymbolMatch {DWORD64 base=0,rva=0,multiRva=0;int count=0,multiCount=0;};
static BOOL CALLBACK symbolCallback(PSYMBOL_INFOW s,ULONG,void* ctx) {
 auto m=(SymbolMatch*)ctx; std::wstring name(s->Name);
 if(name==L"TrayUI::_SetStuckMonitor" || name.find(L"?_SetStuckMonitor@TrayUI@@")==0) {m->rva=s->Address-m->base;m->count++;std::wcerr<<name<<L" RVA "<<m->rva<<L"\n";}
 if(name==L"TrayCommon::GetMultiMonTaskbarState" || name.find(L"?GetMultiMonTaskbarState@TrayCommon@@")==0) {m->multiRva=s->Address-m->base;m->multiCount++;std::wcerr<<name<<L" RVA "<<m->multiRva<<L"\n";}return TRUE;
}
static int resolve(const wchar_t* symbolDir) {
 HANDLE p=GetCurrentProcess();SymSetOptions(SYMOPT_UNDNAME|SYMOPT_FAIL_CRITICAL_ERRORS|SYMOPT_NO_PROMPTS|SYMOPT_LOAD_LINES);
 if(!SymInitializeW(p,symbolDir,FALSE)){std::cerr<<"SymInitialize "<<GetLastError();return 1;}
 auto path=systemFile(L"taskbar.dll");Image im(path);
 DWORD64 base=SymLoadModuleExW(p,nullptr,path.c_str(),nullptr,0,0,nullptr,0);
 if(!base){std::cerr<<"SymLoadModule "<<GetLastError();SymCleanup(p);return 2;}
 SymbolMatch m;m.base=base;SymEnumSymbolsW(p,base,L"*SetStuckMonitor*",symbolCallback,&m);
 SymEnumSymbolsW(p,base,L"*GetMultiMonTaskbarState*",symbolCallback,&m);
 if(m.count!=1||m.multiCount!=1||!im.nt||m.rva>=im.nt->OptionalHeader.SizeOfImage||m.multiRva>=im.nt->OptionalHeader.SizeOfImage){std::cerr<<"Unique symbols not found: "<<m.count<<","<<m.multiCount;SymCleanup(p);return 3;}
 auto ini=directory()+L"\\bridge.ini";
 WritePrivateProfileStringW(L"Symbols",L"TaskbarRva",std::to_wstring(m.rva).c_str(),ini.c_str());
 WritePrivateProfileStringW(L"Symbols",L"MultiMonRva",std::to_wstring(m.multiRva).c_str(),ini.c_str());
 WritePrivateProfileStringW(L"Symbols",L"TaskbarTimestamp",std::to_wstring(im.nt->FileHeader.TimeDateStamp).c_str(),ini.c_str());
 WritePrivateProfileStringW(L"Symbols",L"TaskbarSize",std::to_wstring(im.nt->OptionalHeader.SizeOfImage).c_str(),ini.c_str());
 SymCleanup(p);return 0;
}
// Windows updates replace taskbar.dll; the bridge then rightly refuses the stale
// RVAs. Report whether bridge.ini still describes the installed image (exit 6 if not).
static int symbolsStatus() {
 Image im(systemFile(L"taskbar.dll")); if(!im.nt)return 1;
 auto ini=directory()+L"\\bridge.ini";
 auto number=[&](const wchar_t* name){wchar_t v[80];GetPrivateProfileStringW(L"Symbols",name,L"0",v,80,ini.c_str());return wcstoul(v,nullptr,0);};
 DWORD timestamp=im.nt->FileHeader.TimeDateStamp,size=im.nt->OptionalHeader.SizeOfImage;
 bool match=number(L"TaskbarTimestamp")==timestamp&&number(L"TaskbarSize")==size&&number(L"TaskbarRva")&&number(L"MultiMonRva");
 std::wcout<<L"Taskbar Timestamp="<<timestamp<<L" Size="<<size<<L" Configured Timestamp="<<number(L"TaskbarTimestamp")<<L" Size="<<number(L"TaskbarSize")<<L" match="<<match<<L"\n";
 return match?0:6;
}
static bool shellName(const wchar_t* s){return !_wcsicmp(s,L"explorer.exe")||!_wcsicmp(s,L"ShellHost.exe")||!_wcsicmp(s,L"StartMenuExperienceHost.exe");}
static std::vector<DWORD> shellProcesses() {
 DWORD session;ProcessIdToSessionId(GetCurrentProcessId(),&session);
 DWORD desktopPid=0;GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd",nullptr),&desktopPid);
 std::vector<DWORD> result;HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);PROCESSENTRY32W e{sizeof(e)};
 if(Process32FirstW(snap,&e))do{DWORD other;if(shellName(e.szExeFile)&&(!_wcsicmp(e.szExeFile,L"explorer.exe")?e.th32ProcessID==desktopPid:true)&&ProcessIdToSessionId(e.th32ProcessID,&other)&&other==session)result.push_back(e.th32ProcessID);}while(Process32NextW(snap,&e));CloseHandle(snap);return result;
}
static int configure(bool enabled,DWORD owner,DWORD originalAutoHide) {
 auto dll=directory()+L"\\ShellBridge.dll";
 HMODULE module=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
 if(!module){std::cerr<<"Load bridge "<<GetLastError();return 1;}
 auto proc=(HOOKPROC)GetProcAddress(module,"MessageHook");if(!proc)return 2;
 UINT message=RegisterWindowMessageW(L"OledTaskbar.Native.Configure");bool explorerOk=false;int failures=0;
 for(DWORD pid:shellProcesses()) {
  using namespace OledTaskbarNative;
  std::wstring mappingName=L"Local\\OledTaskbar.Native.Status."+std::to_wstring(pid);
  HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(NativeStatus),mappingName.c_str());
  auto status=mapping?(NativeStatus*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(NativeStatus)):nullptr;
  if(!status){if(mapping)CloseHandle(mapping);std::wcerr<<L"Status mapping unavailable for PID "<<pid<<L"\n";failures++;continue;}
  LARGE_INTEGER counter{};QueryPerformanceCounter(&counter);ULONGLONG requestId=(ULONGLONG)counter.QuadPart ^ ((ULONGLONG)GetCurrentProcessId()<<32);if(!requestId)requestId=1;
  ZeroMemory(status,sizeof(*status));status->Magic=kProtocolMagic;status->Version=kProtocolVersion;status->Size=sizeof(*status);
  status->TargetPid=pid;status->OwnerPid=owner;status->DesiredEnabled=enabled?1:0;status->OriginalAutoHide=originalAutoHide&1;status->RequestId=requestId;MemoryBarrier();
  std::wstring eventName=L"Local\\OledTaskbar.Native.Ready."+std::to_wstring(pid);
  HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str());ResetEvent(ready);
  DWORD taskPidBefore=0;DWORD taskThread=GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd",nullptr),&taskPidBefore);
  std::vector<HHOOK> hooks;HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 e{sizeof(e)};
  if(Thread32First(snap,&e))do{if(e.th32OwnerProcessID!=pid)continue;GUITHREADINFO info{sizeof(info)};if(!GetGUIThreadInfo(e.th32ThreadID,&info))continue;
    if(pid==taskPidBefore && e.th32ThreadID!=taskThread)continue;
    HHOOK hook=SetWindowsHookExW(WH_GETMESSAGE,proc,module,e.th32ThreadID);
    if(hook){hooks.push_back(hook);if(PostThreadMessageW(e.th32ThreadID,message,(WPARAM)requestId,kProtocolVersion) && WaitForSingleObject(ready,1000)==WAIT_OBJECT_0)break;}
  }while(Thread32Next(snap,&e));CloseHandle(snap);
  bool ok=!hooks.empty()&&ready&&WaitForSingleObject(ready,10000)==WAIT_OBJECT_0;
  for(HHOOK hook:hooks)UnhookWindowsHookEx(hook);CloseHandle(ready);
  MemoryBarrier();bool complete=InterlockedCompareExchange(&status->Completion,0,0)==kResponseComplete&&status->ResponseRequestId==requestId;
  std::wstring error=complete?std::wstring(status->Error,wcsnlen_s(status->Error,kErrorCapacity)):L"No matching status received";
  DWORD value=status->Active;ok=ok&&complete&&status->ResultCode==ResultSuccess&&error.empty()&&value==(enabled?1u:0u);
  DWORD taskPid=0;GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd",nullptr),&taskPid);if(pid==taskPid)explorerOk=ok;
  std::wcout<<L"PID "<<pid<<L" hooks "<<hooks.size()<<L" acknowledged "<<ok<<L" error "<<error<<L" enabled "<<value<<L"\n";
  if(!ok && (!hooks.empty()||pid==taskPid))failures++;
  UnmapViewOfFile(status);CloseHandle(mapping);
 }
 FreeLibrary(module);return explorerOk&&failures==0?0:5;
}
static BOOL CALLBACK monitors(HMONITOR h,HDC,LPRECT,LPARAM){MONITORINFOEXW i{};i.cbSize=sizeof(i);if(GetMonitorInfoW(h,&i)){std::wcout<<i.szDevice<<L" primary="<<!!(i.dwFlags&MONITORINFOF_PRIMARY)<<L" bounds="<<i.rcMonitor.left<<L","<<i.rcMonitor.top<<L","<<i.rcMonitor.right<<L","<<i.rcMonitor.bottom<<L" work="<<i.rcWork.left<<L","<<i.rcWork.top<<L","<<i.rcWork.right<<L","<<i.rcWork.bottom<<L" display="<<i.szDevice<<L"\n";}return TRUE;}
static BOOL CALLBACK windows(HWND w,LPARAM){wchar_t cls[128];GetClassNameW(w,cls,128);if(!wcscmp(cls,L"Shell_TrayWnd")||!wcscmp(cls,L"Shell_SecondaryTrayWnd")){RECT r;GetWindowRect(w,&r);DWORD pid;GetWindowThreadProcessId(w,&pid);std::wcout<<cls<<L" hwnd="<<(UINT_PTR)w<<L" pid="<<pid<<L" visible="<<IsWindowVisible(w)<<L" rect="<<r.left<<L","<<r.top<<L","<<r.right<<L","<<r.bottom<<L"\n";}return TRUE;}
static BOOL CALLBACK testWorkArea(HMONITOR monitor,HDC,LPRECT,LPARAM context) {
 MONITORINFOEXW info{};info.cbSize=sizeof(info);if(!GetMonitorInfoW(monitor,&info)){*(bool*)context=false;return TRUE;}
 HWND test=CreateWindowExW(WS_EX_NOACTIVATE,L"OledTaskbar.GeometryTest",L"OLED Taskbar geometry test (closes automatically)",WS_OVERLAPPEDWINDOW,
  info.rcWork.left+80,info.rcWork.top+80,500,300,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
 if(!test){*(bool*)context=false;return TRUE;}
 ShowWindow(test,SW_MAXIMIZE);UpdateWindow(test);
 for(int i=0;i<20;i++){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}Sleep(10);}
 RECT actual{};HRESULT hr=DwmGetWindowAttribute(test,DWMWA_EXTENDED_FRAME_BOUNDS,&actual,sizeof(actual));
 bool ok=SUCCEEDED(hr)&&EqualRect(&actual,&info.rcWork);
 std::wcout<<info.szDevice<<L" standard-maximized-window="<<(ok?L"PASS":L"FAIL")<<L" frame="<<actual.left<<L","<<actual.top<<L","<<actual.right<<L","<<actual.bottom<<L"\n";
 if(!ok)*(bool*)context=false;DestroyWindow(test);return TRUE;
}
int wmain(int argc,wchar_t** argv) {
 SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
 if(argc<2)return 1;
 if(!wcscmp(argv[1],L"pdb-info"))return pdbInfo();
 if(!wcscmp(argv[1],L"resolve")&&argc==3)return resolve(argv[2]);
 if(!wcscmp(argv[1],L"symbols-status"))return symbolsStatus();
 if(!wcscmp(argv[1],L"configure")&&argc==5)return configure(_wtoi(argv[2])!=0,(DWORD)_wtol(argv[3]),(DWORD)_wtol(argv[4]));
 if(!wcscmp(argv[1],L"probe")){APPBARDATA data{};data.cbSize=sizeof(data);std::wcout<<L"AutoHide="<<(SHAppBarMessage(ABM_GETSTATE,&data)&ABS_AUTOHIDE)<<L"\n";EnumDisplayMonitors(nullptr,nullptr,monitors,0);EnumWindows(windows,0);return 0;}
 if(!wcscmp(argv[1],L"test-workarea")){WNDCLASSW c{};c.hInstance=GetModuleHandleW(nullptr);c.lpfnWndProc=DefWindowProcW;c.lpszClassName=L"OledTaskbar.GeometryTest";c.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);if(!RegisterClassW(&c))return 1;bool ok=true;EnumDisplayMonitors(nullptr,nullptr,testWorkArea,(LPARAM)&ok);return ok?0:5;}
 return 1;
}
