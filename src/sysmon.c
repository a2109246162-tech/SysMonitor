/* SysMonitor v1.2 - 三块独立 Metro 贴片 (CPU / 内存 / GPU)
 * 每块可单独拖到桌面任意位置并记住位置；半透明、不置顶。
 * 右键菜单：开机自启 / 立即刷新 / 设置 / 关于 / 退出。
 * 纯 Win32 + C，仅 Win10+，无运行时依赖。
 */
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "pdh.lib")

#define TILE_W  148
#define TILE_H  148
#define PAD     14

#define CPU_BG  RGB(10,58,105)
#define MEM_BG  RGB(8,74,80)
#define GPU_BG  RGB(105,58,6)
#define C_CPU   RGB(0,120,215)
#define C_MEM   RGB(0,183,195)
#define C_GPU   RGB(255,140,0)
#define TRACK   RGB(46,50,62)
#define TXT     RGB(235,239,246)
#define TXT_DIM RGB(185,192,205)

#define REG_APP L"Software\\SysMonitor"
#define REG_RUN L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_NM  L"SysMonitor"

#define ID_REFRESH  1001
#define ID_ALPHA    1002
#define ID_AUTORUN  1003
#define ID_OK       1
#define ID_CANCEL   2

#define ID_AUTORUN_M 3001
#define ID_REFRESH_M 3002
#define ID_SETTINGS  3003
#define ID_ABOUT     3004
#define ID_EXIT      3005
#define ID_FRONT     3006

#define WM_TRAY   (WM_APP + 1)
#define WM_SHOW   (WM_APP + 2)
#define WM_UPDATE (WM_APP + 3)
#define WM_ANIM   (WM_APP + 4)
#define WM_POPUP  (WM_APP + 5)

static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HINSTANCE g_hInst = NULL;
static HWND  g_ctrl = NULL;
static HWND  g_tileHwnd[3] = {0};
static HFONT g_fName, g_fNum, g_fSub, g_fDlg;

static int g_refreshMs = 1000;
static int g_alphaLevel = 1;
static int g_alpha = 165;
static const int g_alphaList[3] = { 140, 165, 200 };

static double g_cpu=0, g_mem=0, g_gpu=0;
static ULONGLONG g_totalPhys=0, g_availPhys=0;
static ULONGLONG g_prevIdle=0, g_prevTotal=0;
static int g_sheen = 0;

static PDH_HQUERY g_query = NULL;
static PDH_HCOUNTER g_gpuCounter = NULL;
static BOOL g_gpuReady = FALSE;

static NOTIFYICONDATAW g_nid;
static BOOL g_tray = FALSE;

static const COLORREF g_bg[3] = { CPU_BG, MEM_BG, GPU_BG };
static const COLORREF g_ac[3] = { C_CPU, C_MEM, C_GPU };
static const wchar_t *g_nm[3] = { L"CPU", L"内存", L"GPU" };

typedef struct {
    int index;
    HDC memDC;
    HBITMAP dib;
    void *bits;
    BYTE *region;
    DWORD baseKey;
    int w, h;
} TILECTX;
static TILECTX g_tile[3];

static void PresentTile(int idx);
static void ApplySettings(int refreshMs, int alphaLevel, BOOL autorun);

/* ---------------- 注册表 ---------------- */
static BOOL RegGetDw(const wchar_t *sub, const wchar_t *name, DWORD *out) {
    HKEY k; DWORD sz=sizeof(DWORD), type=0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return FALSE;
    LONG r = RegQueryValueExW(k, name, NULL, &type, (BYTE*)out, &sz);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_DWORD;
}
static void RegSetDw(const wchar_t *sub, const wchar_t *name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)==ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}
static void RegSetStr(const wchar_t *sub, const wchar_t *name, const wchar_t *v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)==ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v, (DWORD)((wcslen(v)+1)*sizeof(wchar_t)));
        RegCloseKey(k);
    }
}
static BOOL RegGetStr(const wchar_t *sub, const wchar_t *name, wchar_t *out, DWORD cch) {
    HKEY k; DWORD sz = cch * sizeof(wchar_t), type=0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return FALSE;
    LONG r = RegQueryValueExW(k, name, NULL, &type, (BYTE*)out, &sz);
    RegCloseKey(k);
    return r == ERROR_SUCCESS && type == REG_SZ;
}
static BOOL GetAutoRun(void) {
    HKEY k; DWORD type=0, sz=0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, KEY_READ, &k)!=ERROR_SUCCESS) return FALSE;
    LONG r = RegQueryValueExW(k, RUN_NM, NULL, &type, NULL, &sz);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
static BOOL SetAutoRun(BOOL on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, KEY_SET_VALUE, &k)!=ERROR_SUCCESS) return FALSE;
    BOOL ok;
    if (!on) {
        LONG r = RegDeleteValueW(k, RUN_NM);
        ok = (r==ERROR_SUCCESS || r==ERROR_FILE_NOT_FOUND);
    } else {
        wchar_t exe[MAX_PATH], val[MAX_PATH+4];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(val, MAX_PATH+4, L"\"%s\"", exe);
        ok = RegSetValueExW(k, RUN_NM, 0, REG_SZ, (const BYTE*)val,
                            (DWORD)((wcslen(val)+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return ok;
}
static void LoadSettings(void) {
    DWORD v;
    if (RegGetDw(REG_APP, L"RefreshMs", &v) && (v==1000||v==2000||v==5000)) g_refreshMs=(int)v;
    if (RegGetDw(REG_APP, L"AlphaLevel", &v) && v<=2) g_alphaLevel=(int)v;
    if (g_alphaLevel<0 || g_alphaLevel>2) g_alphaLevel=1;
    g_alpha = g_alphaList[g_alphaLevel];
}
static void SavePos(int idx) {
    RECT r;
    if (!g_tileHwnd[idx] || !GetWindowRect(g_tileHwnd[idx], &r)) return;
    wchar_t buf[64], nm[32];
    swprintf(buf, 64, L"%d,%d", (int)r.left, (int)r.top);
    swprintf(nm, 32, L"Pos%d", idx);
    RegSetStr(REG_APP, nm, buf);
}
static BOOL LoadPos(int idx, int *x, int *y) {
    wchar_t nm[32], buf[64];
    swprintf(nm, 32, L"Pos%d", idx);
    if (!RegGetStr(REG_APP, nm, buf, 64)) return FALSE;
    return swscanf(buf, L"%d,%d", x, y) == 2;
}

/* ---------------- 采样 ---------------- */
static void SampleCPU(void) {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return;
    ULONGLONG i=((ULONGLONG)idle.dwHighDateTime<<32)|idle.dwLowDateTime;
    ULONGLONG k=((ULONGLONG)kernel.dwHighDateTime<<32)|kernel.dwLowDateTime;
    ULONGLONG u=((ULONGLONG)user.dwHighDateTime<<32)|user.dwLowDateTime;
    ULONGLONG total=k+u;
    if (g_prevTotal && total>g_prevTotal) {
        double v = (1.0 - (double)(i-g_prevIdle)/(double)(total-g_prevTotal))*100.0;
        if (v<0) v=0; if (v>100) v=100;
        g_cpu = v;
    }
    g_prevIdle=i; g_prevTotal=total;
}
static void SampleMem(void) {
    MEMORYSTATUSEX ms; ms.dwLength=sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        g_totalPhys=ms.ullTotalPhys; g_availPhys=ms.ullAvailPhys;
        if (g_totalPhys) g_mem=(double)(g_totalPhys-g_availPhys)*100.0/(double)g_totalPhys;
    }
}
static BOOL InitGPU(void) {
    if (PdhOpenQueryA(NULL,0,&g_query)!=ERROR_SUCCESS) return FALSE;
    PDH_STATUS st = PdhAddEnglishCounterA(g_query, "\\GPU Engine(*)\\Utilization Percentage",0,&g_gpuCounter);
    if (st!=ERROR_SUCCESS)
        st = PdhAddCounterA(g_query, "\\GPU Engine(*)\\Utilization Percentage",0,&g_gpuCounter);
    if (st!=ERROR_SUCCESS) { PdhCloseQuery(g_query); g_query=NULL; return FALSE; }
    PdhCollectQueryData(g_query);
    return TRUE;
}
static void SampleGPU(void) {
    if (!g_query || !g_gpuCounter) { g_gpu=0; return; }
    if (PdhCollectQueryData(g_query)!=ERROR_SUCCESS) return;
    DWORD sz=0, items=0;
    PDH_STATUS s = PdhGetFormattedCounterArrayA(g_gpuCounter, PDH_FMT_DOUBLE, &sz, &items, NULL);
    if (s!=PDH_MORE_DATA && s!=ERROR_SUCCESS) return;
    if (!sz) return;
    PDH_FMT_COUNTERVALUE_ITEM_A *buf=NULL; DWORD gotN=0, gotSz=0;
    for (int a=0; a<4 && sz; ++a) {
        buf=(PDH_FMT_COUNTERVALUE_ITEM_A*)malloc(sz);
        if (!buf) return;
        DWORD s2=sz, n2=items;
        s=PdhGetFormattedCounterArrayA(g_gpuCounter, PDH_FMT_DOUBLE, &s2, &n2, buf);
        if (s==ERROR_SUCCESS) { gotN=n2; gotSz=s2; break; }
        free(buf); buf=NULL;
        if (s!=PDH_MORE_DATA) return;
        sz=s2; items=n2;
    }
    if (!buf) return;
    typedef struct { char key[96]; double sum; } ENG;
    ENG eng[64]; int n=0;
    PDH_FMT_COUNTERVALUE_ITEM_A *arr=buf;
    DWORD maxN=gotSz/(DWORD)sizeof(PDH_FMT_COUNTERVALUE_ITEM_A);
    DWORD cnt=gotN<maxN?gotN:maxN;
    BYTE *b0=(BYTE*)buf, *b1=b0+gotSz;
    for (DWORD i=0;i<cnt;++i) {
        if (arr[i].FmtValue.CStatus!=PDH_CSTATUS_VALID_DATA) continue;
        const char *nm=arr[i].szName;
        if (!nm) continue;
        BYTE *np=(BYTE*)nm;
        if (np<b0||np>=b1) continue;
        double v=arr[i].FmtValue.doubleValue;
        if (!(v>0.0)) continue;
        if (v>100.0) v=100.0;
        const char *lu=strstr(nm,"_luid_");
        const char *key=lu?lu+1:nm;
        int idx=-1;
        for (int j=0;j<n;++j) if (!strcmp(eng[j].key,key)) { idx=j; break; }
        if (idx<0 && n<64) {
            idx=n++; strncpy(eng[idx].key,key,95); eng[idx].key[95]=0; eng[idx].sum=0;
        }
        if (idx>=0) eng[idx].sum+=v;
    }
    double best=0;
    for (int j=0;j<n;++j) if (eng[j].sum>best) best=eng[j].sum;
    if (best>100) best=100;
    g_gpu=best;
    free(buf);
}

/* ---------------- 绘制贴片 ---------------- */
static void Fill(HDC hdc,int l,int t,int r,int b,COLORREF c) {
    HBRUSH br=CreateSolidBrush(c);
    RECT rc={l,t,r,b};
    FillRect(hdc,&rc,br);
    DeleteObject(br);
}
static COLORREF Blend(COLORREF c,int amt) {
    int r=GetRValue(c),g=GetGValue(c),b=GetBValue(c);
    r+=(255-r)*amt/255; g+=(255-g)*amt/255; b+=(255-b)*amt/255;
    return RGB(r,g,b);
}
static void Sheen(HDC hdc,int x,int y,int fw,int h,int off,COLORREF ac) {
    if (fw<=0) return;
    int sw=fw/2+S(60), period=fw+sw, sx=(off%period)-sw;
    int step=sw/30<1?1:sw/30;
    HGDIOBJ ob=SelectObject(hdc,GetStockObject(DC_BRUSH));
    for (int i=0;i<30;++i) {
        int x0=sx+i*step, x1=x0+step;
        if (x1<=x||x0>=x+fw) continue;
        int d=i*2-29; if (d<0) d=-d;
        SetDCBrushColor(hdc,Blend(ac,(29-d)*80/29));
        RECT rr={x0<x?x:x0,y,x1>x+fw?x+fw:x1,y+h};
        FillRect(hdc,&rr,(HBRUSH)GetStockObject(DC_BRUSH));
    }
    SelectObject(hdc,ob);
}
static DWORD KeyOf(COLORREF c) {
    return (DWORD)GetBValue(c)|((DWORD)GetGValue(c)<<8)|((DWORD)GetRValue(c)<<16);
}
static void BuildRegion(TILECTX *tc) {
    int W=tc->w, H=tc->h, r=S(14); if (r<2) r=2;
    tc->region=(BYTE*)malloc((size_t)W*H);
    if (!tc->region) return;
    tc->baseKey=KeyOf(g_bg[tc->index]);
    for (int y=0;y<H;++y) {
        BOOL ye=(y<r)||(y>H-r);
        for (int x=0;x<W;++x) {
            BYTE v=1;
            if (ye||x<r||x>W-r) {
                BOOL in=TRUE;
                if (x<r) {
                    if (y<r)       { int dx=r-x,dy=r-y;      in=(dx*dx+dy*dy<=r*r); }
                    else if (y>H-r){ int dx=r-x,dy=y-(H-r);  in=(dx*dx+dy*dy<=r*r); }
                } else if (x>W-r) {
                    if (y<r)       { int dx=x-(W-r),dy=r-y;      in=(dx*dx+dy*dy<=r*r); }
                    else if (y>H-r){ int dx=x-(W-r),dy=y-(H-r);  in=(dx*dx+dy*dy<=r*r); }
                }
                if (!in) v=0;
            }
            tc->region[(size_t)y*W+x]=v;
        }
    }
}
static void AlphaPass(TILECTX *tc) {
    if (!tc->bits||!tc->region) return;
    DWORD *row=(DWORD*)tc->bits;
    size_t total=(size_t)tc->w*tc->h;
    for (size_t i=0;i<total;++i) {
        DWORD px=row[i];
        int a;
        if (!tc->region[i]) a=0;
        else if ((px&0xFFFFFF)==tc->baseKey) a=g_alpha;
        else a=255;
        if (a==255) row[i]=px|0xFF000000;
        else if (a==0) row[i]=0;
        else {
            DWORD b=(px&0xFF)*a/255;
            DWORD g=((px>>8)&0xFF)*a/255;
            DWORD r=((px>>16)&0xFF)*a/255;
            row[i]=b|(g<<8)|(r<<16)|((DWORD)a<<24);
        }
    }
}
static void DrawTileScene(TILECTX *tc) {
    HDC hdc=tc->memDC;
    int w=tc->w,h=tc->h,idx=tc->index;
    Fill(hdc,0,0,w,h,g_bg[idx]);
    Fill(hdc,0,0,w,S(4),g_ac[idx]);
    SetBkMode(hdc,TRANSPARENT);

    SelectObject(hdc,g_fName); SetTextColor(hdc,TXT);
    TextOutW(hdc,S(PAD),S(16),g_nm[idx],(int)wcslen(g_nm[idx]));

    double val = idx==0?g_cpu:(idx==1?g_mem:g_gpu);
    wchar_t buf[32];
    int n=swprintf(buf,32,L"%d%%",(int)(val+0.5));
    SelectObject(hdc,g_fNum); SetTextColor(hdc,TXT);
    TextOutW(hdc,S(PAD),S(42),buf,n);

    if (idx==1 && g_totalPhys) {
        double used=(double)(g_totalPhys-g_availPhys)/1073741824.0;
        double tot=(double)g_totalPhys/1073741824.0;
        swprintf(buf,32,L"%.1f / %.1f GB",used,tot);
        SelectObject(hdc,g_fSub); SetTextColor(hdc,TXT_DIM);
        TextOutW(hdc,S(PAD),S(86),buf,(int)wcslen(buf));
    }

    int bx=S(PAD), bw=w-S(PAD)*2, bh=S(7), by=h-S(28);
    Fill(hdc,bx,by,bx+bw,by+bh,TRACK);
    int fw=(int)(bw*(val>100?100:val)/100.0);
    if (fw>0) {
        Fill(hdc,bx,by,bx+fw,by+bh,g_ac[idx]);
        Fill(hdc,bx,by,bx+fw,by+(bh>S(3)?S(2):1),Blend(g_ac[idx],55));
        Sheen(hdc,bx,by,fw,bh,g_sheen,g_ac[idx]);
    }
}
static void PresentTile(int idx) {
    TILECTX *tc=&g_tile[idx];
    HWND hwnd=g_tileHwnd[idx];
    if (!hwnd||!tc->memDC) return;
    DrawTileScene(tc);
    AlphaPass(tc);
    RECT wr; GetWindowRect(hwnd,&wr);
    POINT dst={wr.left,wr.top}, src={0,0};
    SIZE sz={tc->w,tc->h};
    BLENDFUNCTION bf={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd,NULL,&dst,&sz,tc->memDC,&src,0,&bf,ULW_ALPHA);
}

/* ---------------- 设置窗口（代码创建，避开 rc 中文乱码） ---------------- */
#define ID_LBL1 101
#define ID_LBL2 102
static HWND g_setHwnd=NULL, g_refCombo=NULL, g_alphaCombo=NULL, g_autoChk=NULL;

static HWND AddCtl(HWND parent,const wchar_t *cls,const wchar_t *txt,DWORD style,
                   int x,int y,int w,int h,int id,HFONT font) {
    HWND hw=CreateWindowExW(0,cls,txt,WS_CHILD|WS_VISIBLE|style,
                            S(x),S(y),S(w),S(h),parent,(HMENU)(INT_PTR)id,g_hInst,NULL);
    if (hw&&font) SendMessageW(hw,WM_SETFONT,(WPARAM)font,TRUE);
    return hw;
}
static void ApplySettings(int refreshMs,int alphaLevel,BOOL autorun) {
    g_refreshMs=refreshMs; g_alphaLevel=alphaLevel;
    g_alpha=g_alphaList[alphaLevel];
    RegSetDw(REG_APP,L"RefreshMs",(DWORD)refreshMs);
    RegSetDw(REG_APP,L"AlphaLevel",(DWORD)alphaLevel);
    SetAutoRun(autorun);
    if (g_ctrl) {
        KillTimer(g_ctrl,1);
        SetTimer(g_ctrl,1,(UINT)g_refreshMs,NULL);
        for (int i=0;i<3;++i)
            if (g_tileHwnd[i]) PostMessageW(g_tileHwnd[i],WM_UPDATE,0,0);
    }
}
static INT_PTR CALLBACK SettingsProc(HWND h,UINT m,WPARAM wParam,LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        const wchar_t *r3[]={L"1 秒",L"2 秒",L"5 秒"};
        const wchar_t *a3[]={L"更透明",L"标准",L"更清晰"};
        AddCtl(h,L"STATIC",L"刷新间隔",SS_LEFT,20,22,100,24,ID_LBL1,g_fDlg);
        g_refCombo=AddCtl(h,L"COMBOBOX",NULL,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
                          130,18,260,180,ID_REFRESH,g_fDlg);
        AddCtl(h,L"STATIC",L"透明度",SS_LEFT,20,72,100,24,ID_LBL2,g_fDlg);
        g_alphaCombo=AddCtl(h,L"COMBOBOX",NULL,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
                            130,68,260,180,ID_ALPHA,g_fDlg);
        for (int i=0;i<3;++i) {
            SendMessageW(g_refCombo,CB_ADDSTRING,0,(LPARAM)r3[i]);
            SendMessageW(g_alphaCombo,CB_ADDSTRING,0,(LPARAM)a3[i]);
        }
        SendMessageW(g_refCombo,CB_SETCURSEL,
                     g_refreshMs==2000?1:(g_refreshMs==5000?2:0),0);
        SendMessageW(g_alphaCombo,CB_SETCURSEL,g_alphaLevel,0);
        g_autoChk=AddCtl(h,L"BUTTON",L"开机自动启动",BS_AUTOCHECKBOX|WS_TABSTOP,
                         20,124,370,28,ID_AUTORUN,g_fDlg);
        SendMessageW(g_autoChk,BM_SETCHECK,GetAutoRun()?BST_CHECKED:BST_UNCHECKED,0);
        AddCtl(h,L"BUTTON",L"确定",BS_DEFPUSHBUTTON|WS_TABSTOP,130,178,100,36,ID_OK,g_fDlg);
        AddCtl(h,L"BUTTON",L"取消",BS_PUSHBUTTON|WS_TABSTOP,250,178,100,36,ID_CANCEL,g_fDlg);
        return 0;
    }
    case WM_COMMAND: {
        int id=LOWORD(wParam);
        if (id==ID_OK) {
            static const int ms[3]={1000,2000,5000};
            int ri=(int)SendMessageW(g_refCombo,CB_GETCURSEL,0,0);
            int ai=(int)SendMessageW(g_alphaCombo,CB_GETCURSEL,0,0);
            if (ri<0||ri>2) ri=0;
            if (ai<0||ai>2) ai=1;
            ApplySettings(ms[ri],ai,SendMessageW(g_autoChk,BM_GETCHECK,0,0)==BST_CHECKED);
            DestroyWindow(h);
            return 0;
        }
        if (id==ID_CANCEL) { DestroyWindow(h); return 0; }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_setHwnd=NULL; g_refCombo=NULL; g_alphaCombo=NULL; g_autoChk=NULL;
        return 0;
    }
    return DefWindowProcW(h,m,wParam,l);
}
static void OpenSettings(void) {
    if (g_setHwnd) { SetForegroundWindow(g_setHwnd); return; }
    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    int w=S(400), h=S(280);
    g_setHwnd=CreateWindowExW(0,L"SysMonSettings",L"系统监控 · 设置",
                              WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
                              (sw-w)/2,(sh-h)/2,w,h,NULL,NULL,g_hInst,NULL);
    if (g_setHwnd) ShowWindow(g_setHwnd,SW_SHOW);
}

/* ---------------- 控制窗 ---------------- */
static void ShowAllTiles(void) {
    for (int i=0;i<3;++i)
        if (g_tileHwnd[i]) ShowWindow(g_tileHwnd[i],SW_SHOW);
    if (g_ctrl) SetForegroundWindow(g_ctrl);
}
static void AddTray(void) {
    ZeroMemory(&g_nid,sizeof(g_nid));
    g_nid.cbSize=sizeof(g_nid);
    g_nid.hWnd=g_ctrl; g_nid.uID=1;
    g_nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    g_nid.uCallbackMessage=WM_TRAY;
    g_nid.hIcon=LoadIconW(g_hInst,MAKEINTRESOURCEW(1));
    wcsncpy(g_nid.szTip,L"系统监控 · 右键设置开机自启 / 退出",127);
    Shell_NotifyIconW(NIM_ADD,&g_nid);
    g_tray=TRUE;
}
static void PopupMenu(void) {
    POINT p; GetCursorPos(&p);
    HMENU h=CreatePopupMenu();
    AppendMenuW(h,MF_STRING|(GetAutoRun()?MF_CHECKED:MF_UNCHECKED),ID_AUTORUN_M,L"开机自启");
    AppendMenuW(h,MF_STRING,ID_REFRESH_M,L"立即刷新");
    AppendMenuW(h,MF_SEPARATOR,0,NULL);
    AppendMenuW(h,MF_STRING,ID_SETTINGS,L"设置…");
    AppendMenuW(h,MF_STRING,ID_ABOUT,L"关于…");
    AppendMenuW(h,MF_SEPARATOR,0,NULL);
    AppendMenuW(h,MF_STRING,ID_FRONT,L"显示在桌面");
    AppendMenuW(h,MF_STRING,ID_EXIT,L"退出");
    if (g_ctrl) SetForegroundWindow(g_ctrl);
    int id=TrackPopupMenu(h,TPM_RIGHTBUTTON|TPM_RETURNCMD,p.x,p.y,0,g_ctrl,NULL);
    DestroyMenu(h);
    switch (id) {
    case ID_AUTORUN_M: SetAutoRun(!GetAutoRun()); break;
    case ID_REFRESH_M:
        if (!g_gpuReady) { InitGPU(); g_gpuReady=TRUE; }
        SampleCPU(); SampleMem(); SampleGPU();
        for (int i=0;i<3;++i)
            if (g_tileHwnd[i]) PostMessageW(g_tileHwnd[i],WM_UPDATE,0,0);
        break;
    case ID_SETTINGS: OpenSettings(); break;
    case ID_ABOUT:
        MessageBoxW(NULL,L"系统监控 v1.2\n\nCPU / 内存 / GPU 独立贴片，可任意拖动。\n原生 Win32，无运行时依赖。",
                    L"关于",MB_OK|MB_ICONINFORMATION);
        break;
    case ID_FRONT: ShowAllTiles(); break;
    case ID_EXIT:
        if (g_ctrl) DestroyWindow(g_ctrl);
        break;
    }
}

/* ---------------- 贴片窗口过程 ---------------- */
static LRESULT CALLBACK TileProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    int idx=(int)(INT_PTR)GetWindowLongPtr(hwnd,0);
    TILECTX *tc=&g_tile[idx];
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCT *cs=(CREATESTRUCT*)l;
        idx=(int)(INT_PTR)cs->lpCreateParams;
        SetWindowLongPtr(hwnd,0,(LONG_PTR)idx);
        tc=&g_tile[idx];
        tc->index=idx;
        tc->w=S(TILE_W); tc->h=S(TILE_H);
        HDC scr=GetDC(NULL);
        tc->memDC=CreateCompatibleDC(scr);
        BITMAPINFO bi; ZeroMemory(&bi,sizeof(bi));
        bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth=tc->w; bi.bmiHeader.biHeight=-tc->h;
        bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32;
        bi.bmiHeader.biCompression=BI_RGB;
        tc->dib=CreateDIBSection(scr,&bi,DIB_RGB_COLORS,&tc->bits,NULL,0);
        ReleaseDC(NULL,scr);
        if (tc->memDC&&tc->dib) SelectObject(tc->memDC,tc->dib);
        BuildRegion(tc);
        return 0;
    }
    case WM_UPDATE:
    case WM_ANIM:
        PresentTile(idx);
        return 0;
    case WM_NCHITTEST:
        /* 整块当标题栏，交给系统原生拖动（最稳定，支持任意位置） */
        return HTCAPTION;
    case WM_EXITSIZEMOVE:
        SavePos(idx);   /* 拖完记住位置 */
        return 0;
    case WM_RBUTTONUP:
        if (g_ctrl) PostMessageW(g_ctrl,WM_POPUP,0,0);
        return 0;
    case WM_DESTROY:
        free(tc->region); tc->region=NULL;
        if (tc->dib) DeleteObject(tc->dib);
        if (tc->memDC) DeleteDC(tc->memDC);
        tc->dib=NULL; tc->memDC=NULL;
        g_tileHwnd[idx]=NULL;
        return 0;
    }
    return DefWindowProcW(hwnd,msg,w,l);
}

/* ---------------- 控制窗口过程 ---------------- */
static LRESULT CALLBACK CtrlProc(HWND hwnd,UINT msg,WPARAM w,LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        g_ctrl=hwnd;
        AddTray();
        for (int i=0;i<3;++i) {
            int x,y,wid=S(TILE_W),hei=S(TILE_H);
            int sx=GetSystemMetrics(SM_CXSCREEN), sy=GetSystemMetrics(SM_CYSCREEN);
            if (!LoadPos(i,&x,&y)) {
                x=sx-wid-S(24);
                y=sy-(hei*3+S(12)*2)-S(70)+i*(hei+S(12));
            }
            g_tileHwnd[i]=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW,
                L"SysMonTile",NULL,WS_POPUP,x,y,wid,hei,
                NULL,NULL,g_hInst,(LPVOID)(INT_PTR)i);
            if (g_tileHwnd[i]) {
                ShowWindow(g_tileHwnd[i], SW_SHOW);
                PresentTile(i);
            }
        }
        SampleCPU(); SampleMem();
        SetTimer(hwnd,1,(UINT)g_refreshMs,NULL);
        SetTimer(hwnd,2,50,NULL);
        return 0;
    case WM_TIMER:
        if (w==1) {
            if (!g_gpuReady) { InitGPU(); g_gpuReady=TRUE; }
            SampleCPU(); SampleMem(); SampleGPU();
            for (int i=0;i<3;++i)
                if (g_tileHwnd[i]) PostMessageW(g_tileHwnd[i],WM_UPDATE,0,0);
        } else if (w==2) {
            g_sheen+=4;
            for (int i=0;i<3;++i)
                if (g_tileHwnd[i]) PostMessageW(g_tileHwnd[i],WM_ANIM,0,0);
        }
        return 0;
    case WM_TRAY:
        if (LOWORD(l)==WM_LBUTTONDBLCLK) ShowAllTiles();
        else if (LOWORD(l)==WM_RBUTTONUP) PopupMenu();
        return 0;
    case WM_POPUP:
        PopupMenu();
        return 0;
    case WM_SHOW:
        ShowAllTiles();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd,1); KillTimer(hwnd,2);
        if (g_tray) { Shell_NotifyIconW(NIM_DELETE,&g_nid); g_tray=FALSE; }
        for (int i=0;i<3;++i) if (g_tileHwnd[i]) DestroyWindow(g_tileHwnd[i]);
        if (g_query) PdhCloseQuery(g_query);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd,msg,w,l);
}

int WINAPI WinMain(HINSTANCE h,HINSTANCE hp,LPSTR cmdline,int nshow) {
    g_hInst=h;
    SetProcessDPIAware();
    const wchar_t *cmd=GetCommandLineW();
    if (wcsstr(cmd,L"/autorun=on"))    return SetAutoRun(TRUE)?10:11;
    if (wcsstr(cmd,L"/autorun=off"))   return SetAutoRun(FALSE)?20:21;
    if (wcsstr(cmd,L"/autorun=query")) return GetAutoRun()?1:0;

    HDC sdc=GetDC(NULL);
    g_dpi=GetDeviceCaps(sdc,LOGPIXELSX);
    ReleaseDC(NULL,sdc);
    if (g_dpi<96) g_dpi=96;
    LoadSettings();

#define MFONT(hh,ww) CreateFontW(S(hh),0,0,0,ww,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI")
    g_fName=MFONT(13,FW_SEMIBOLD);
    g_fNum=MFONT(34,FW_SEMIBOLD);
    g_fSub=MFONT(11,FW_NORMAL);
    g_fDlg=CreateFontW(S(13),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Microsoft YaHei UI");
#undef MFONT

    WNDCLASSEXW wc={0};
    wc.cbSize=sizeof(wc); wc.hInstance=h; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hIcon=LoadIconW(h,MAKEINTRESOURCEW(1)); wc.hIconSm=wc.hIcon;

    wc.lpfnWndProc=CtrlProc; wc.lpszClassName=L"SysMonCtrl";
    wc.cbWndExtra=0; wc.hbrBackground=NULL;
    RegisterClassExW(&wc);

    wc.lpfnWndProc=TileProc; wc.lpszClassName=L"SysMonTile";
    wc.cbWndExtra=sizeof(LONG_PTR)*3; wc.hbrBackground=NULL;
    RegisterClassExW(&wc);

    wc.lpfnWndProc=SettingsProc; wc.lpszClassName=L"SysMonSettings";
    wc.cbWndExtra=0; wc.hbrBackground=(HBRUSH)GetStockObject(WHITE_BRUSH);
    RegisterClassExW(&wc);

    if (wcsstr(cmd,L"/settings")) {
        OpenSettings();
    } else {
        HWND exist=FindWindowW(L"SysMonCtrl",NULL);
        if (exist) { PostMessageW(exist,WM_SHOW,0,0); return 0; }
        CreateWindowExW(0,L"SysMonCtrl",L"系统监控",WS_POPUP,0,0,0,0,
                        NULL,NULL,h,NULL);
    }

    MSG msg;
    while (GetMessageW(&msg,NULL,0,0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(g_fName); DeleteObject(g_fNum); DeleteObject(g_fSub); DeleteObject(g_fDlg);
    return 0;
}
