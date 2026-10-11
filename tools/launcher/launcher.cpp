// minecraftCplusplusW.exe: starts minecraftCplusplus.exe (next to it) with the same arguments but without a console
// window, for shortcuts and double-clicks (like pythonw.exe). The game then logs to
// %LOCALAPPDATA%\minecraftCplusplus\logs\latest.log (platform/launch.h).
#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t self[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, self, MAX_PATH)) return 1;
    std::wstring exe = self;
    exe = exe.substr(0, exe.find_last_of(L"\\/") + 1) + L"minecraftCplusplus.exe";
    // The arguments after the program name, exactly as given.
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"') {
        for (++p; *p && *p != L'"'; ++p) {
        }
        if (*p) ++p;
    } else {
        while (*p && *p != L' ' && *p != L'\t') ++p;
    }
    std::wstring cmd = L"\"" + exe + L"\"" + p;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        MessageBoxW(nullptr, (L"Could not start " + exe).c_str(), L"minecraftCplusplus", MB_OK | MB_ICONERROR);
        return 1;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
