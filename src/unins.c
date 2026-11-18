/* unins.exe - 系统监控 卸载程序 */
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wchar.h>

static void GetExeDir(wchar_t *out) {
    GetModuleFileNameW(NULL, out, MAX_PATH);
    wchar_t *s = wcsrchr(out, L'\\');
    if (s) *s = 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    const wchar_t *cmd = GetCommandLineW();
    BOOL quiet = (wcsstr(cmd, L"/q") || wcsstr(cmd, L"/Q")) ? TRUE : FALSE;

    if (!quiet) {
        int r = MessageBoxW(NULL,
            L"确定要卸载「系统监控」吗？\n\n将删除程序文件，以及桌面和开始菜单的快捷方式。",
            L"卸载 系统监控", MB_YESNO | MB_ICONQUESTION);
        if (r != IDYES) return 0;
    }

    wchar_t dir[MAX_PATH];
    GetExeDir(dir);

    /* 关闭正在运行的监控（控制窗类 SysMonCtrl） */
    HWND h = FindWindowW(L"SysMonCtrl", NULL);
    if (h) {
        PostMessageW(h, WM_CLOSE, 0, 0);
        Sleep(600);
    }

    /* 删除快捷方式 */
    wchar_t p[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, p))) {
        wchar_t lnk[MAX_PATH];
        swprintf(lnk, MAX_PATH, L"%s\\系统监控.lnk", p);
        DeleteFileW(lnk);
    }
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, p))) {
        wchar_t lnk[MAX_PATH];
        swprintf(lnk, MAX_PATH, L"%s\\系统监控.lnk", p);
        DeleteFileW(lnk);
    }

    /* 删除开机自启（任务管理器 → 启动） */
    HKEY run;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &run) == ERROR_SUCCESS) {
        RegDeleteValueW(run, L"SysMonitor");
        RegCloseKey(run);
    }

    /* 删除设置与卸载注册项 */
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\SysMonitor");
    RegDeleteTreeW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SysMonitor");

    if (!quiet) {
        MessageBoxW(NULL, L"卸载完成。", L"卸载 系统监控", MB_OK | MB_ICONINFORMATION);
    }

    /* 退出后删除整个安装目录（含 unins.exe 自身） */
    wchar_t cmdline[MAX_PATH + 96];
    swprintf(cmdline, MAX_PATH + 96,
             L"/c ping -n 2 127.0.0.1 >nul & rmdir /s /q \"%s\"", dir);

    wchar_t sys[MAX_PATH], cmdExe[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(cmdExe, MAX_PATH, L"%s\\cmd.exe", sys);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    CreateProcessW(cmdExe, cmdline, NULL, NULL, FALSE,
                   CREATE_NO_WINDOW, NULL, sys, &si, &pi);
    if (pi.hProcess) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }

    return 0;
}
