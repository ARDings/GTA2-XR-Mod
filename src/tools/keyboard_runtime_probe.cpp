// Explicit private device diagnostic. Never bundled in the public mod package.
// Sends one held Enter only to a foreground process named gta2.exe, then exits.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>

int main(int argc,char** argv) {
    if (argc!=2 || std::strcmp(argv[1],"--test-gta2-menu")!=0) return 2;
    FILE* log=std::fopen("gta2ceiling_keyboard_probe.log","w");
    if (!log) return 3;
    Sleep(12000); // The launcher starts us immediately before GTA2.
    HWND window=GetForegroundWindow(); DWORD pid=0;
    GetWindowThreadProcessId(window,&pid);
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    char image[MAX_PATH]{}; DWORD length=MAX_PATH;
    const bool known=process && QueryFullProcessImageNameA(process,0,image,&length);
    if(process) CloseHandle(process);
    const char* leaf=std::strrchr(image,'\\'); leaf=leaf?leaf+1:image;
    std::fprintf(log,"foreground=%p pid=%lu image=%s asyncEnter=%x\n",window,pid,image,
                 static_cast<unsigned short>(GetAsyncKeyState(VK_RETURN)));
    std::fflush(log);
    if (!known || _stricmp(leaf,"gta2.exe")!=0) { std::fclose(log); return 4; }
    char threadName[128]{},inputName[128]{}; DWORD needed=0;
    HDESK threadDesktop=GetThreadDesktop(GetCurrentThreadId());
    HDESK inputDesktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS|DESKTOP_SWITCHDESKTOP);
    GetUserObjectInformationA(threadDesktop,UOI_NAME,threadName,sizeof(threadName),&needed);
    if(inputDesktop) GetUserObjectInformationA(inputDesktop,UOI_NAME,inputName,sizeof(inputName),&needed);
    std::fprintf(log,"threadDesktop=%s inputDesktop=%s inputHandle=%p error=%lu\n",threadName,inputName,inputDesktop,GetLastError());
    if(inputDesktop) CloseDesktop(inputDesktop);
    // Explicit diagnostic only: switch the virtual Wine desktop that already
    // owns the confirmed game foreground; no Windows desktop changes on a PC.
    if(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version")) {
        const BOOL switched=SwitchDesktop(threadDesktop);
        std::fprintf(log,"Wine SwitchDesktop(current)=%d error=%lu\n",switched,GetLastError());
    }
    INPUT event{}; event.type=INPUT_KEYBOARD; event.ki.wScan=0x1C; event.ki.dwFlags=KEYEVENTF_SCANCODE;
    const UINT down=SendInput(1,&event,sizeof(event));
    Sleep(100);
    std::fprintf(log,"downAccepted=%u error=%lu asyncEnterHeld=%x\n",down,GetLastError(),
                 static_cast<unsigned short>(GetAsyncKeyState(VK_RETURN)));
    std::fflush(log);
    Sleep(400);
    event.ki.dwFlags|=KEYEVENTF_KEYUP;
    const UINT up=SendInput(1,&event,sizeof(event));
    Sleep(100);
    std::fprintf(log,"upAccepted=%u error=%lu asyncEnterReleased=%x\n",up,GetLastError(),
                 static_cast<unsigned short>(GetAsyncKeyState(VK_RETURN)));
    std::fflush(log);
    Sleep(2200);
    DWORD currentPid=0; GetWindowThreadProcessId(GetForegroundWindow(),&currentPid);
    if(currentPid==pid && down==1 && up==1) {
        event.ki.dwFlags=KEYEVENTF_SCANCODE;
        const UINT secondDown=SendInput(1,&event,sizeof(event)); Sleep(500);
        event.ki.dwFlags|=KEYEVENTF_KEYUP;
        const UINT secondUp=SendInput(1,&event,sizeof(event));
        std::fprintf(log,"startArea down=%u up=%u\n",secondDown,secondUp);
    }
    std::fclose(log);
    return down==1 && up==1?0:5;
}
