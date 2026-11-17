/* SysMonitor Setup - 安装向导
 * 纯 Win32 + C。选择安装盘符/目录 -> 写入程序 -> 创建快捷方式 -> 注册卸载信息。
 * 支持静默安装： SysMonitorSetup.exe /S [/D=安装目录]
 */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <objbase.h>
#include <wchar.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")

#define RES_APP   100
#define RES_UNINS 101

#define ID_DRIVE   1001
#define ID_PATH    1002
#define ID_BROWSE  1003
#define ID_INSTALL 1004
#define ID_CANCEL  1005
#define ID_RUN     1006
#define ID_PROG    1007
#define ID_STATUS  1008
#define ID_INFO    1009
#define ID_LBL     1010

#define TM_STEP 200

static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

enum { ST_CHOOSE, ST_INSTALL, ST_DONE, ST_ERROR };

/* 已知 GUID（不依赖头文件声明） */
static const CLSID MY_CLSID_ShellLink = {0x00021401,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const IID   MY_IID_IShellLinkW = {0x000214F9,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
static const IID   MY_IID_IPersistFile = {0x0000010B,0x0000,0x0000,{0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

static HINSTANCE g_hInst;
static HWND  g_hwnd;
static HICON g_hIcon;
static HFONT g_fUI, g_fBtn, g_fHeader;

static HWND hDrive, hPath, hBrowse, hInfo, hInstall, hCancel, hProg, hStatus, hRun;

static int  g_state = ST_CHOOSE;
static int  g_step  = 0;
static wchar_t g_dir[MAX_PATH] = {0};

static const wchar_t *STEP_TEXT[7] = {
    L"", L"正在创建安装目录…", L"正在写入程序文件…", L"正在写入卸载程序…",
    L"正在创建桌面快捷方式…", L"正在创建开始菜单快捷方式…", L"正在注册应用信息…"
};

/* ---------------- 工具 ---------------- */
static void TrimW(wchar_t *s) {
    wchar_t *p = s;
    while (*p == L' ' || *p == L'\t') p++;
    if (p != s) memmove(s, p, (wcslen(p)+1)*sizeof(wchar_t));
    size_t n = wcslen(s);
    while (n > 0 && (s[n-1]==L' '||s[n-1]==L'\t'||s[n-1]==L'\r'||s[n-1]==L'\n')) s[--n]=0;
}

static void JoinPath(wchar_t *out, const wchar_t *dir, const wchar_t *name) {
    size_t n = wcslen(dir);
    if (n && (dir[n-1]==L'\\'||dir[n-1]==L'/'))
        swprintf(out, MAX_PATH, L"%s%s", dir, name);
    else
        swprintf(out, MAX_PATH, L"%s\\%s", dir, name);
}

static BOOL EnsureDir(const wchar_t *path) {
    wchar_t tmp[MAX_PATH];
    wcsncpy(tmp, path, MAX_PATH-1); tmp[MAX_PATH-1]=0;
    size_t len = wcslen(tmp);
    while (len > 3 && (tmp[len-1]==L'\\'||tmp[len-1]==L'/')) tmp[--len]=0;
    for (wchar_t *p = tmp + 3; *p; ++p) {
        if (*p==L'\\' || *p==L'/') { wchar_t c=*p; *p=0; CreateDirectoryW(tmp,NULL); *p=c; }
    }
    CreateDirectoryW(tmp, NULL);
    DWORD a = GetFileAttributesW(tmp);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL WriteRes(int id, const wchar_t *dst) {
    HRSRC r = FindResourceW(g_hInst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10)); /* RT_RCDATA */
    if (!r) return FALSE;
    DWORD sz = SizeofResource(g_hInst, r);
    HGLOBAL hg = LoadResource(g_hInst, r);
    const void *p = LockResource(hg);
    if (!p || !sz) return FALSE;
    HANDLE f = CreateFileW(dst, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wr = 0; BOOL ok = WriteFile(f, p, sz, &wr, NULL);
    CloseHandle(f);
    return ok && wr == sz;
}

static BOOL MakeShortcut(const wchar_t *lnk, const wchar_t *target, const wchar_t *desc) {
    IShellLinkW *psl = NULL;
    HRESULT hr = CoCreateInstance(&MY_CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                  &MY_IID_IShellLinkW, (void**)&psl);
    if (FAILED(hr) || !psl) return FALSE;

    psl->lpVtbl->SetPath(psl, target);
    wchar_t dir[MAX_PATH]; wcsncpy(dir, target, MAX_PATH-1); dir[MAX_PATH-1]=0;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    psl->lpVtbl->SetWorkingDirectory(psl, dir);
    if (desc) psl->lpVtbl->SetDescription(psl, desc);
    psl->lpVtbl->SetIconLocation(psl, target, 0);

    IPersistFile *ppf = NULL;
    hr = psl->lpVtbl->QueryInterface(psl, &MY_IID_IPersistFile, (void**)&ppf);
    BOOL ok = FALSE;
    if (SUCCEEDED(hr) && ppf) {
        ok = SUCCEEDED(ppf->lpVtbl->Save(ppf, lnk, TRUE));
        ppf->lpVtbl->Release(ppf);
    }
    psl->lpVtbl->Release(psl);
    return ok;
}

static BOOL GetSpecialPath(int csidl, wchar_t *out) {
    return SUCCEEDED(SHGetFolderPathW(NULL, csidl, NULL, 0, out));
}

static void SetRegSz(HKEY k, const wchar_t *name, const wchar_t *val) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val, (DWORD)((wcslen(val)+1)*sizeof(wchar_t)));
}
static void SetRegDw(HKEY k, const wchar_t *name, DWORD v) {
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
}

static void WriteUninstallInfo(void) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SysMonitor",
            0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;

    wchar_t app[MAX_PATH], unins[MAX_PATH], uninsCmd[MAX_PATH+8];
    JoinPath(app, g_dir, L"SysMonitor.exe");
    JoinPath(unins, g_dir, L"unins.exe");
    swprintf(uninsCmd, MAX_PATH+8, L"\"%s\"", unins);

    SetRegSz(k, L"DisplayName", L"系统监控 (SysMonitor)");
    SetRegSz(k, L"DisplayVersion", L"1.1.0");
    SetRegSz(k, L"Publisher", L"SysMonitor");
    SetRegSz(k, L"InstallLocation", g_dir);
    SetRegSz(k, L"DisplayIcon", app);
    SetRegSz(k, L"UninstallString", uninsCmd);
    SetRegDw(k, L"NoModify", 1);
    SetRegDw(k, L"NoRepair", 1);
    SetRegDw(k, L"EstimatedSize", 64);
    RegCloseKey(k);
}

/* ---------------- 安装步骤 ---------------- */
static BOOL StepWork(int step, wchar_t *err, int errc) {
    wchar_t p[MAX_PATH];
    switch (step) {
    case 1:
        if (!EnsureDir(g_dir)) {
            swprintf(err, errc, L"无法创建安装目录，请换一个位置或盘符。");
            return FALSE;
        }
        return TRUE;
    case 2:
        JoinPath(p, g_dir, L"SysMonitor.exe");
        if (!WriteRes(RES_APP, p)) {
            swprintf(err, errc, L"写入程序文件失败（目标目录可能没有写入权限）。");
            return FALSE;
        }
        return TRUE;
    case 3:
        JoinPath(p, g_dir, L"unins.exe");
        WriteRes(RES_UNINS, p);
        return TRUE;
    case 4: {
        wchar_t desk[MAX_PATH], lnk[MAX_PATH], app[MAX_PATH];
        if (GetSpecialPath(CSIDL_DESKTOPDIRECTORY, desk)) {
            swprintf(lnk, MAX_PATH, L"%s\\系统监控.lnk", desk);
            JoinPath(app, g_dir, L"SysMonitor.exe");
            MakeShortcut(lnk, app, L"系统监控 - CPU / 内存 / GPU");
        }
        return TRUE;
    }
    case 5: {
        wchar_t sm[MAX_PATH], lnk[MAX_PATH], app[MAX_PATH];
        if (GetSpecialPath(CSIDL_PROGRAMS, sm)) {
            swprintf(lnk, MAX_PATH, L"%s\\系统监控.lnk", sm);
            JoinPath(app, g_dir, L"SysMonitor.exe");
            MakeShortcut(lnk, app, L"系统监控");
        }
        return TRUE;
    }
    case 6:
        WriteUninstallInfo();
        return TRUE;
    }
    return TRUE;
}

static void DefaultDir(wchar_t *out) {
    wchar_t base[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
        swprintf(out, MAX_PATH, L"%s\\Programs\\SysMonitor", base);
    else {
        wchar_t win[MAX_PATH];
        GetWindowsDirectoryW(win, MAX_PATH);
        wchar_t root[4] = { win[0], L':', L'\\', 0 };
        swprintf(out, MAX_PATH, L"%sSysMonitor", root);
    }
}

/* ---------------- 界面 ---------------- */
static HWND MkCtl(const wchar_t *cls, const wchar_t *txt, DWORD style, DWORD ex,
                  int x, int y, int w, int h, int id) {
    HWND hw = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style,
                              x, y, w, h, g_hwnd, (HMENU)(INT_PTR)id, g_hInst, NULL);
    if (hw) SendMessageW(hw, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    return hw;
}

static void OnDriveChanged(void) {
    int i = (int)SendMessageW(hDrive, CB_GETCURSEL, 0, 0);
    if (i < 0) return;
    wchar_t letter = (wchar_t)SendMessageW(hDrive, CB_GETITEMDATA, i, 0);
    wchar_t p[MAX_PATH];
    if (!letter) DefaultDir(p);
    else swprintf(p, MAX_PATH, L"%c:\\SysMonitor", letter);
    SetWindowTextW(hPath, p);
}

static void FillDrives(void) {
    DWORD mask = GetLogicalDrives();

    int sel = 0, idx = 0;

    /* 推荐位置：当前用户目录，无需管理员权限 */
    int rec = (int)SendMessageW(hDrive, CB_ADDSTRING, 0,
                                (LPARAM)L"推荐位置（当前用户，无需管理员）");
    SendMessageW(hDrive, CB_SETITEMDATA, rec, 0);
    sel = rec;
    idx = 1;

    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root);
        if (t != DRIVE_FIXED && t != DRIVE_REMOVABLE) continue;

        ULARGE_INTEGER fb, tb, tf;
        wchar_t label[128];
        if (GetDiskFreeSpaceExW(root, &fb, &tb, &tf))
            swprintf(label, 128, L"%c:    可用 %.1f GB", root[0], (double)fb.QuadPart / 1073741824.0);
        else
            swprintf(label, 128, L"%c:", root[0]);

        int n = (int)SendMessageW(hDrive, CB_ADDSTRING, 0, (LPARAM)label);
        SendMessageW(hDrive, CB_SETITEMDATA, n, (LPARAM)root[0]);
        idx++;
    }
    SendMessageW(hDrive, CB_SETDROPPEDWIDTH, S(430), 0);
    SendMessageW(hDrive, CB_SETCURSEL, sel, 0);
    OnDriveChanged();
}

static void CreateControls(void) {
    MkCtl(L"STATIC", L"选择安装位置（可切换盘符）：", SS_LEFT, 0,
          S(36), S(108), S(340), S(18), ID_LBL);

    hDrive = CreateWindowExW(0, L"COMBOBOX", NULL,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        S(36), S(128), S(230), S(300), g_hwnd, (HMENU)(INT_PTR)ID_DRIVE, g_hInst, NULL);
    SendMessageW(hDrive, WM_SETFONT, (WPARAM)g_fUI, TRUE);

    hPath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        S(36), S(166), S(476), S(26), g_hwnd, (HMENU)(INT_PTR)ID_PATH, g_hInst, NULL);
    SendMessageW(hPath, WM_SETFONT, (WPARAM)g_fUI, TRUE);

    hBrowse = MkCtl(L"BUTTON", L"浏览…", BS_OWNERDRAW, 0,
                    S(522), S(164), S(102), S(30), ID_BROWSE);
    SendMessageW(hBrowse, WM_SETFONT, (WPARAM)g_fBtn, TRUE);

    hInfo = MkCtl(L"STATIC",
        L"默认装到当前用户目录，无需管理员权限；也可切换到其他盘符。",
        SS_LEFT, 0, S(36), S(202), S(600), S(20), ID_INFO);

    hProg = CreateWindowExW(0, PROGRESS_CLASSW, NULL,
        WS_CHILD | PBS_SMOOTH, S(36), S(296), S(588), S(12),
        g_hwnd, (HMENU)(INT_PTR)ID_PROG, g_hInst, NULL);
    SendMessageW(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(hProg, PBM_SETPOS, 0, 0);
    ShowWindow(hProg, SW_HIDE);

    hStatus = MkCtl(L"STATIC", L"", SS_LEFT, 0, S(36), S(316), S(588), S(24), ID_STATUS);
    ShowWindow(hStatus, SW_HIDE);

    hRun = CreateWindowExW(0, L"BUTTON", L"立即启动 系统监控",
        WS_CHILD | BS_AUTOCHECKBOX, S(36), S(388), S(260), S(24),
        g_hwnd, (HMENU)(INT_PTR)ID_RUN, g_hInst, NULL);
    SendMessageW(hRun, WM_SETFONT, (WPARAM)g_fUI, TRUE);
    SendMessageW(hRun, BM_SETCHECK, BST_CHECKED, 0);
    ShowWindow(hRun, SW_HIDE);

    hInstall = MkCtl(L"BUTTON", L"安装", BS_OWNERDRAW, 0,
                     S(444), S(378), S(180), S(44), ID_INSTALL);
    SendMessageW(hInstall, WM_SETFONT, (WPARAM)g_fBtn, TRUE);

    hCancel = MkCtl(L"BUTTON", L"取消", BS_OWNERDRAW, 0,
                    S(340), S(378), S(92), S(44), ID_CANCEL);
    SendMessageW(hCancel, WM_SETFONT, (WPARAM)g_fBtn, TRUE);

    FillDrives();
}

static void StartInstall(void) {
    wchar_t path[MAX_PATH];
    GetWindowTextW(hPath, path, MAX_PATH);
    TrimW(path);
    if (!path[0]) {
        MessageBoxW(g_hwnd, L"请先选择安装位置。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }

    /* 预检可写性：被 UAC/权限拦住时，自动建议改为当前用户目录 */
    if (!EnsureDir(path)) {
        if (MessageBoxW(g_hwnd,
                L"该位置无法写入（通常需要管理员权限）。\n\n"
                L"是否改为安装到「当前用户」的推荐位置？\n"
                L"（无需管理员，马上就能用）",
                L"安装位置不可用", MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
        DefaultDir(path);
        SetWindowTextW(hPath, path);
        if (!EnsureDir(path)) {
            MessageBoxW(g_hwnd,
                L"推荐位置也无法写入，请手动选择一个文件夹后再试。",
                L"安装失败", MB_OK | MB_ICONERROR);
            return;
        }
    }

    wcsncpy(g_dir, path, MAX_PATH-1); g_dir[MAX_PATH-1] = 0;

    g_state = ST_INSTALL;
    EnableWindow(hDrive, FALSE);
    EnableWindow(hPath, FALSE);
    EnableWindow(hBrowse, FALSE);
    EnableWindow(hInstall, FALSE);
    EnableWindow(hCancel, FALSE);
    ShowWindow(hProg, SW_SHOW);
    ShowWindow(hStatus, SW_SHOW);
    SendMessageW(hProg, PBM_SETPOS, 5, 0);
    SetWindowTextW(hStatus, L"正在准备安装…");
    g_step = 0;
    SetTimer(g_hwnd, TM_STEP, 220, NULL);
}

static void FinishInstall(void) {
    KillTimer(g_hwnd, TM_STEP);
    g_state = ST_DONE;
    SendMessageW(hProg, PBM_SETPOS, 100, 0);
    SetWindowTextW(hStatus, L"安装完成！已在桌面创建「系统监控」快捷方式。");
    SetWindowTextW(hInstall, L"完成");
    EnableWindow(hInstall, TRUE);
    ShowWindow(hRun, SW_SHOW);
    SetFocus(hInstall);
}

static void FailInstall(const wchar_t *err) {
    KillTimer(g_hwnd, TM_STEP);
    g_state = ST_ERROR;
    SetWindowTextW(hStatus, err);
    SetWindowTextW(hInstall, L"重试");
    EnableWindow(hInstall, TRUE);
    EnableWindow(hCancel, TRUE);
}

static void OnInstallClick(void) {
    if (g_state == ST_DONE) {
        if (IsDlgButtonChecked(g_hwnd, ID_RUN) == BST_CHECKED) {
            wchar_t app[MAX_PATH];
            JoinPath(app, g_dir, L"SysMonitor.exe");
            ShellExecuteW(NULL, L"open", app, NULL, g_dir, SW_SHOWNORMAL);
        }
        DestroyWindow(g_hwnd);
        return;
    }
    if (g_state == ST_INSTALL) return;
    StartInstall();
}

/* ---------------- 窗口过程 ---------------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateControls();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        HBRUSH hb = CreateSolidBrush(RGB(0,120,215));
        RECT hd = { 0, 0, rc.right, S(92) };
        FillRect(hdc, &hd, hb);
        DeleteObject(hb);

        HBRUSH wb = CreateSolidBrush(RGB(255,255,255));
        RECT bd = { 0, S(92), rc.right, rc.bottom };
        FillRect(hdc, &bd, wb);
        DeleteObject(wb);

        SetBkMode(hdc, TRANSPARENT);

        DrawIconEx(hdc, S(36), S(26), g_hIcon, S(40), S(40), 0, NULL, DI_NORMAL);

        SelectObject(hdc, g_fHeader);
        SetTextColor(hdc, RGB(255,255,255));
        TextOutW(hdc, S(88), S(20), L"系统监控  ·  SysMonitor",
                 (int)wcslen(L"系统监控  ·  SysMonitor"));

        SelectObject(hdc, g_fUI);
        SetTextColor(hdc, RGB(214,232,250));
        TextOutW(hdc, S(88), S(56), L"Metro 磁贴桌面监控  ·  安装程序  v1.1",
                 (int)wcslen(L"Metro 磁贴桌面监控  ·  安装程序  v1.1"));

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetTextColor(hdc, RGB(72,76,86));
        SetBkColor(hdc, RGB(255,255,255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT d = (LPDRAWITEMSTRUCT)lParam;
        wchar_t txt[64];
        GetWindowTextW(d->hwndItem, txt, 64);
        BOOL dis = (d->itemState & ODS_DISABLED) ? TRUE : FALSE;

        COLORREF bg, fg;
        if (d->CtlID == ID_INSTALL) {
            bg = dis ? RGB(178,184,196) : RGB(0,120,215);
            fg = RGB(255,255,255);
        } else {
            bg = dis ? RGB(246,247,249) : RGB(238,240,244);
            fg = RGB(45,48,56);
        }

        HBRUSH b = CreateSolidBrush(bg);
        FillRect(d->hDC, &d->rcItem, b);
        DeleteObject(b);

        if (d->CtlID == ID_CANCEL || d->CtlID == ID_BROWSE) {
            HPEN p = CreatePen(PS_SOLID, 1, RGB(208,213,222));
            HGDIOBJ op = SelectObject(d->hDC, p);
            HGDIOBJ ob = SelectObject(d->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom);
            SelectObject(d->hDC, ob);
            SelectObject(d->hDC, op);
            DeleteObject(p);
        }

        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, fg);
        SelectObject(d->hDC, g_fBtn);
        DrawTextW(d->hDC, txt, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (d->itemState & ODS_FOCUS) DrawFocusRect(d->hDC, &d->rcItem);
        return TRUE;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam), code = HIWORD(wParam);
        if (id == ID_BROWSE && code == BN_CLICKED) {
            BROWSEINFOW bi = {0};
            wchar_t disp[MAX_PATH] = {0};
            bi.hwndOwner = hwnd;
            bi.pszDisplayName = disp;
            bi.lpszTitle = L"选择安装文件夹";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t sel[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, sel)) SetWindowTextW(hPath, sel);
                CoTaskMemFree(pidl);
            }
        } else if (id == ID_INSTALL && code == BN_CLICKED) {
            OnInstallClick();
        } else if (id == ID_CANCEL && code == BN_CLICKED) {
            DestroyWindow(hwnd);
        } else if (id == ID_DRIVE && code == CBN_SELCHANGE) {
            OnDriveChanged();
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == TM_STEP) {
            g_step++;
            if (g_step > 6) { FinishInstall(); return 0; }
            wchar_t err[300] = {0};
            if (!StepWork(g_step, err, 300)) { FailInstall(err); return 0; }
            SetWindowTextW(hStatus, STEP_TEXT[g_step]);
            SendMessageW(hProg, PBM_SETPOS, 12 + g_step * 13, 0);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

/* ---------------- 静默安装 ---------------- */
static BOOL CmdHas(const wchar_t *cmd, const wchar_t *sw) {
    wchar_t tok[128];
    const wchar_t *p = cmd;
    while (*p) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        int i = 0;
        while (*p && *p != L' ' && *p != L'\t' && i < 127) tok[i++] = *p++;
        tok[i] = 0;
        if (_wcsicmp(tok, sw) == 0) return TRUE;
    }
    return FALSE;
}

static BOOL GetDArg(const wchar_t *cmd, wchar_t *out) {
    const wchar_t *p = wcsstr(cmd, L"/D=");
    if (!p) p = wcsstr(cmd, L"/d=");
    if (!p) { p = wcsstr(cmd, L"/D:"); }
    if (!p) { p = wcsstr(cmd, L"/d:"); }
    if (!p) return FALSE;
    p += 3;
    wchar_t quote = 0;
    if (*p == L'"') { quote = L'"'; p++; }
    size_t i = 0;
    while (*p && i < MAX_PATH-1) {
        if (quote && *p == quote) { p++; break; }
        if (!quote && (*p == L' ' || *p == L'\t')) break;
        out[i++] = *p++;
    }
    out[i] = 0;
    TrimW(out);
    return i > 0;
}

static int SilentInstall(const wchar_t *dir) {
    wcsncpy(g_dir, dir, MAX_PATH-1); g_dir[MAX_PATH-1] = 0;
    wchar_t err[300];
    for (int s = 1; s <= 6; s++) {
        err[0] = 0;
        if (!StepWork(s, err, 300)) return 2;
    }
    return 0;
}

/* ---------------- 入口 ---------------- */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    g_hInst = hInstance;
    SetProcessDPIAware();

    HDC sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(NULL, sdc);
    if (g_dpi < 96) g_dpi = 96;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    const wchar_t *cmd = GetCommandLineW();

    if (CmdHas(cmd, L"/S") || CmdHas(cmd, L"/silent") || CmdHas(cmd, L"/VERYSILENT")) {
        wchar_t dir[MAX_PATH];
        if (!GetDArg(cmd, dir)) DefaultDir(dir);
        int rc = SilentInstall(dir);
        CoUninitialize();
        return rc;
    }

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    g_fUI     = CreateFontW(S(14),0,0,0,FW_NORMAL,  0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    g_fBtn    = CreateFontW(S(15),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");
    g_fHeader = CreateFontW(S(24),0,0,0,FW_SEMIBOLD,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Segoe UI");

    g_hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc = {0};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = L"SysMonSetup";
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.hIcon         = g_hIcon;
    wc.hIconSm       = g_hIcon;
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT r = { 0, 0, S(660), S(440) };
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - w) / 2, y = (sh - h) / 2;

    g_hwnd = CreateWindowExW(0, L"SysMonSetup", L"系统监控 安装程序",
                             style, x, y, w, h, NULL, NULL, hInstance, NULL);
    if (!g_hwnd) { CoUninitialize(); return 1; }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(g_fUI); DeleteObject(g_fBtn); DeleteObject(g_fHeader);
    CoUninitialize();
    return 0;
}
