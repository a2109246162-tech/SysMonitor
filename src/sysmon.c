/* SysMonitor v1.3 - 三块独立 Metro 贴片 (CPU / 内存 / GPU)
 * 每块可单独拖到桌面任意位置并记住位置；半透明。
 * v1.3: 去掉顶部色条；左上/左下/右下直角(仅右上圆角放图钉)；
 *       右上角图钉按钮切换置顶/沉底；透明度支持 0(无背景)；
 *       开机自启修复(RegCreateKeyEx + 校验当前 exe)；高 DPI 修正。
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
#define PIN_ON  RGB(255,190,60)
#define PIN_OFF RGB(170,176,188)

#define REG_APP L"Software\\SysMonitor"
#define REG_RUN L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_NM  L"SysMonitor"

#define ID_REFRESH  1001
#define ID_ALPHA    1002
#define ID_AUTORUN  1003
#define ID_CORNER   1004
#define ID_SHOW0    1010
#define ID_SHOW1    1011
#define ID_SHOW2    1012
#define ID_COLOR0   1020
#define ID_COLOR1   1021
#define ID_COLOR2   1022
#define ID_OK       1
#define ID_CANCEL   2

#define ID_AUTORUN_M 3001
#define ID_REFRESH_M 3002
#define ID_SETTINGS  3003
#define ID_ABOUT     3004
#define ID_EXIT      3005
#define ID_PIN       3006

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

/* 透明度四档：0=无背景(仅文字/进度条)、110=很透明、165=标准、215=更清晰 */
static int g_refreshMs = 1000;
static int g_alphaLevel = 2;
static int g_alpha = 165;
static const int g_alphaList[4] = { 0, 110, 165, 215 };
static int g_cornerMode = 0;   /* 0=直角(默认，仅右上图钉角圆角) 1=圆角(四角) */
static BOOL g_pinned[3] = { FALSE, FALSE, FALSE };  /* 每块独立固定 */
static int g_pinTarget = 0;    /* 右键菜单作用的贴片索引 */

static double g_cpu=0, g_mem=0, g_gpu=0;
static ULONGLONG g_totalPhys=0, g_availPhys=0;
static ULONGLONG g_prevIdle=0, g_prevTotal=0;
static int g_sheen = 0;

static PDH_HQUERY g_query = NULL;
static PDH_HCOUNTER g_gpuCounter = NULL;
static BOOL g_gpuReady = FALSE;

static NOTIFYICONDATAW g_nid;
static BOOL g_tray = FALSE;

static COLORREF g_bg[3] = { CPU_BG, MEM_BG, GPU_BG };
static COLORREF g_ac[3] = { C_CPU, C_MEM, C_GPU };
static BOOL g_show[3] = { TRUE, TRUE, TRUE };
static const wchar_t *g_nm[3] = { L"CPU", L"内存", L"GPU" };

/* 预设颜色（每个指标可独立选择） */
#define NCOLORS 8
static const COLORREF g_palette[NCOLORS] = {
    C_CPU, C_MEM, C_GPU,
    RGB(155,89,182), RGB(46,204,113), RGB(231,76,60),
    RGB(241,196,15), RGB(127,140,141)
};
static const wchar_t *g_paletteNm[NCOLORS] = {
    L"蓝色", L"青色", L"橙色", L"紫色", L"绿色", L"红色", L"黄色", L"灰色"
};
static int PaletteIndexOf(COLORREF c) {
    for (int i=0;i<NCOLORS;++i) if (g_palette[i]==c) return i;
    return 0;
}
static void ApplyColors(void) {
    /* 由强调色推导深色背景，让自定义颜色贯穿整块贴片 */
    for (int i=0;i<3;++i) {
        int r=GetRValue(g_ac[i]), g=GetGValue(g_ac[i]), b=GetBValue(g_ac[i]);
        g_bg[i] = RGB(r*45/100 + 6, g*45/100 + 6, b*45/100 + 6);
    }
}

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
static void ApplySettings(int refreshMs, int alphaLevel, BOOL autorun, int cornerMode,
                          BOOL show[3], COLORREF color[3]);
static void ApplyPin(void);

/* ---------------- DPI ---------------- */
typedef BOOL (WINAPI *PFN_SPDAC)(void *);
typedef UINT (WINAPI *PFN_GDFS)(void);
typedef BOOL (WINAPI *PFN_AWRED)(LPRECT, DWORD, BOOL, DWORD, UINT);
static void InitDpi(void) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PFN_SPDAC spdac = u32 ? (PFN_SPDAC)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
    if (spdac) spdac((void *)(INT_PTR)-4);   /* PER_MONITOR_AWARE_V2 */
    else SetProcessDPIAware();
    PFN_GDFS gdfs = u32 ? (PFN_GDFS)GetProcAddress(u32, "GetDpiForSystem") : NULL;
    g_dpi = gdfs ? (int)gdfs() : 96;
    if (g_dpi < 96) g_dpi = 96;
}
static void AdjustWindowRectDpi(LPRECT rc, DWORD style) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PFN_AWRED aw = u32 ? (PFN_AWRED)GetProcAddress(u32, "AdjustWindowRectExForDpi") : NULL;
    if (aw) aw(rc, style, FALSE, 0, (UINT)g_dpi);
    else AdjustWindowRect(rc, style, FALSE);
}

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
/* 开机自启：校验注册表值是否确实指向当前 exe，避免旧路径残留导致"偶发不生效" */
static BOOL GetAutoRun(void) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, KEY_READ, &k)!=ERROR_SUCCESS) return FALSE;
    wchar_t val[MAX_PATH+16]; DWORD sz=sizeof(val), type=0;
    LONG r = RegQueryValueExW(k, RUN_NM, NULL, &type, (BYTE*)val, &sz);
    RegCloseKey(k);
    if (r!=ERROR_SUCCESS || (type!=REG_SZ && type!=REG_EXPAND_SZ)) return FALSE;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    const wchar_t *p = val;
    if (*p==L'"') p++;
    size_t n = wcslen(p);
    while (n && p[n-1]==L'"') n--;
    return (n==wcslen(exe)) && (_wcsnicmp(p, exe, n)==0);
}
static BOOL SetAutoRun(BOOL on) {
    /* 关键修复：用 RegCreateKeyEx，Run 键缺失时也能创建 */
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_RUN, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)!=ERROR_SUCCESS)
        return FALSE;
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
    RegFlushKey(k);
    RegCloseKey(k);
    return ok;
}
static void LoadSettings(void) {
    DWORD v;
    if (RegGetDw(REG_APP, L"RefreshMs", &v) && (v==1000||v==2000||v==5000)) g_refreshMs=(int)v;
    /* AlphaV2 标记：旧版 0/1/2 映射到新版 1/2/3，避免升级后档位突变 */
    if (RegGetDw(REG_APP, L"AlphaLevel", &v)) {
        DWORD ver=0;
        if (!RegGetDw(REG_APP, L"AlphaV2", &ver)) {
            if (v<=2) v=v+1;
            RegSetDw(REG_APP, L"AlphaV2", 1);
        }
        if (v<=3) g_alphaLevel=(int)v;
    }
    if (g_alphaLevel<0 || g_alphaLevel>3) g_alphaLevel=2;
    g_alpha = g_alphaList[g_alphaLevel];
    /* 每块独立固定状态；兼容旧版单值 Pinned */
    if (RegGetDw(REG_APP, L"Pinned", &v)) {
        for (int i=0;i<3;++i) g_pinned[i] = (v!=0);
    }
    for (int i=0;i<3;++i) {
        wchar_t nm[32]; swprintf(nm,32,L"Pin%d",i);
        if (RegGetDw(REG_APP, nm, &v)) g_pinned[i] = (v!=0);
    }
    if (RegGetDw(REG_APP, L"CornerMode", &v)) g_cornerMode = (v==1)?1:0;
    for (int i=0;i<3;++i) {
        wchar_t nm[32];
        swprintf(nm,32,L"Show%d",i);
        if (RegGetDw(REG_APP, nm, &v)) g_show[i] = (v!=0);
        swprintf(nm,32,L"Color%d",i);
        if (RegGetDw(REG_APP, nm, &v)) g_ac[i] = (COLORREF)v;
    }
    ApplyColors();
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

/* ---------------- 采样（已验证，保持不变） ---------------- */
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
/* 边角样式：直角=仅右上圆角放图钉；圆角=四角全圆角 */
static void BuildRegion(TILECTX *tc) {
    int W=tc->w, H=tc->h, r=S(14); if (r<2) r=2;
    if (tc->region) free(tc->region);
    tc->region=(BYTE*)malloc((size_t)W*H);
    if (!tc->region) return;
    tc->baseKey=KeyOf(g_bg[tc->index]);
    for (int y=0;y<H;++y) {
        for (int x=0;x<W;++x) {
            BYTE v=1;
            if (g_cornerMode==1) {
                /* 圆角：四角 */
                int nearX = (x<r) || (x>W-r);
                int nearY = (y<r) || (y>H-r);
                if (nearX && nearY) {
                    int cx = (x<r) ? r : W-r;
                    int cy = (y<r) ? r : H-r;
                    int dx=x-cx, dy=y-cy;
                    if (dx*dx+dy*dy > r*r) v=0;
                }
            } else {
                /* 直角：仅右上角圆角放图钉 */
                if (x>W-r && y<r) {
                    int dx=x-(W-r), dy=y-r;
                    if (dx*dx+dy*dy > r*r) v=0;
                }
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
/* 右上角图钉：亮色=已固定(置顶)，灰色=未固定(沉底) */
static void DrawPin(HDC hdc,int w,int idx) {
    int cx=w-S(17), cy=S(17);
    COLORREF c = g_pinned[idx] ? PIN_ON : PIN_OFF;
    HGDIOBJ ob=SelectObject(hdc,GetStockObject(DC_BRUSH));
    SetDCBrushColor(hdc,c);
    Fill(hdc,cx-S(2),cy-S(1),cx+S(2),cy+S(11),c);              /* 针杆 */
    Ellipse(hdc,cx-S(6),cy-S(9),cx+S(5),cy+S(2));              /* 圆头 */
    SelectObject(hdc,ob);
}
static void DrawTileScene(TILECTX *tc) {
    HDC hdc=tc->memDC;
    int w=tc->w,h=tc->h,idx=tc->index;
    Fill(hdc,0,0,w,h,g_bg[idx]);
    /* v1.3: 去掉顶部多出的色条 */
    SetBkMode(hdc,TRANSPARENT);

    SelectObject(hdc,g_fName); SetTextColor(hdc,TXT);
    TextOutW(hdc,S(PAD),S(14),g_nm[idx],(int)wcslen(g_nm[idx]));

    double val = idx==0?g_cpu:(idx==1?g_mem:g_gpu);
    wchar_t buf[32];
    int n=swprintf(buf,32,L"%d%%",(int)(val+0.5));
    SelectObject(hdc,g_fNum); SetTextColor(hdc,TXT);
    TextOutW(hdc,S(PAD),S(40),buf,n);

    if (idx==1 && g_totalPhys) {
        double used=(double)(g_totalPhys-g_availPhys)/1073741824.0;
        double tot=(double)g_totalPhys/1073741824.0;
        swprintf(buf,32,L"%.1f / %.1f GB",used,tot);
        SelectObject(hdc,g_fSub); SetTextColor(hdc,TXT_DIM);
        TextOutW(hdc,S(PAD),S(84),buf,(int)wcslen(buf));
    }

    int bx=S(PAD), bw=w-S(PAD)*2, bh=S(7), by=h-S(28);
    Fill(hdc,bx,by,bx+bw,by+bh,TRACK);
    int fw=(int)(bw*(val>100?100:val)/100.0);
    if (fw>0) {
        Fill(hdc,bx,by,bx+fw,by+bh,g_ac[idx]);
        Fill(hdc,bx,by,bx+fw,by+(bh>S(3)?S(2):1),Blend(g_ac[idx],55));
        Sheen(hdc,bx,by,fw,bh,g_sheen,g_ac[idx]);
    }
    DrawPin(hdc,w,idx);
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
static void ApplyPinOne(int i) {
    if (!g_tileHwnd[i]) return;
    HWND ins = g_pinned[i] ? HWND_TOPMOST : HWND_BOTTOM;
    SetWindowPos(g_tileHwnd[i],ins,0,0,0,0,
                 SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
static void ApplyPin(void) {
    for (int i=0;i<3;++i) ApplyPinOne(i);
}
static void TogglePin(int idx) {
    if (idx<0||idx>2) return;
    g_pinned[idx] = !g_pinned[idx];
    wchar_t nm[32]; swprintf(nm,32,L"Pin%d",idx);
    RegSetDw(REG_APP, nm, g_pinned[idx]?1:0);
    ApplyPinOne(idx);
    if (g_tileHwnd[idx]) PostMessageW(g_tileHwnd[idx],WM_UPDATE,0,0);
}

/* ---------------- 设置窗口（代码创建，避开 rc 中文乱码） ---------------- */
#define ID_LBL1 101
#define ID_LBL2 102
static HWND g_setHwnd=NULL, g_refCombo=NULL, g_alphaCombo=NULL, g_autoChk=NULL;
static HWND g_cornerCombo=NULL, g_showChk[3]={0}, g_colorCombo[3]={0};

static HWND AddCtl(HWND parent,const wchar_t *cls,const wchar_t *txt,DWORD style,
                   int x,int y,int w,int h,int id,HFONT font) {
    HWND hw=CreateWindowExW(0,cls,txt,WS_CHILD|WS_VISIBLE|style,
                            S(x),S(y),S(w),S(h),parent,(HMENU)(INT_PTR)id,g_hInst,NULL);
    if (hw&&font) SendMessageW(hw,WM_SETFONT,(WPARAM)font,TRUE);
    return hw;
}
static void ApplySettings(int refreshMs,int alphaLevel,BOOL autorun,int cornerMode,
                          BOOL show[3], COLORREF color[3]) {
    g_refreshMs=refreshMs; g_alphaLevel=alphaLevel; g_cornerMode=cornerMode;
    g_alpha=g_alphaList[alphaLevel];
    for (int i=0;i<3;++i) { g_show[i]=show[i]; g_ac[i]=color[i]; }
    ApplyColors();
    RegSetDw(REG_APP,L"RefreshMs",(DWORD)refreshMs);
    RegSetDw(REG_APP,L"AlphaLevel",(DWORD)alphaLevel);
    RegSetDw(REG_APP,L"AlphaV2",1);
    RegSetDw(REG_APP,L"CornerMode",(DWORD)cornerMode);
    for (int i=0;i<3;++i) {
        wchar_t nm[32];
        swprintf(nm,32,L"Show%d",i);
        RegSetDw(REG_APP,nm,(DWORD)g_show[i]);
        swprintf(nm,32,L"Color%d",i);
        RegSetDw(REG_APP,nm,(DWORD)g_ac[i]);
    }
    SetAutoRun(autorun);
    if (g_ctrl) {
        KillTimer(g_ctrl,1);
        SetTimer(g_ctrl,1,(UINT)g_refreshMs,NULL);
    }
    for (int i=0;i<3;++i) {
        if (!g_tileHwnd[i]) continue;
        if (g_show[i]) {
            BuildRegion(&g_tile[i]);      /* 重建圆角/直角区域 */
            ShowWindow(g_tileHwnd[i], SW_SHOW);
            PostMessageW(g_tileHwnd[i],WM_UPDATE,0,0);  /* 立即刷新外观 */
        } else {
            ShowWindow(g_tileHwnd[i], SW_HIDE);
        }
    }
}
static INT_PTR CALLBACK SettingsProc(HWND h,UINT m,WPARAM wParam,LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        const wchar_t *r3[]={L"1 秒",L"2 秒",L"5 秒"};
        const wchar_t *a4[]={L"无背景（0）",L"很透明",L"标准",L"更清晰"};
        const wchar_t *c2[]={L"直角（默认）",L"圆角"};
        int y1=18, y2=62, y3=106, y4=150, yHdr=196;
        /* 常规 */
        AddCtl(h,L"STATIC",L"刷新间隔",SS_LEFT,20,y1+4,110,24,ID_LBL1,g_fDlg);
        g_refCombo=AddCtl(h,L"COMBOBOX",NULL,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
                          135,y1,235,180,ID_REFRESH,g_fDlg);
        AddCtl(h,L"STATIC",L"透明度",SS_LEFT,20,y2+4,110,24,ID_LBL2,g_fDlg);
        g_alphaCombo=AddCtl(h,L"COMBOBOX",NULL,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
                            135,y2,235,180,ID_ALPHA,g_fDlg);
        AddCtl(h,L"STATIC",L"边角样式",SS_LEFT,20,y3+4,110,24,ID_LBL1,g_fDlg);
        g_cornerCombo=AddCtl(h,L"COMBOBOX",NULL,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
                             135,y3,235,180,ID_CORNER,g_fDlg);
        g_autoChk=AddCtl(h,L"BUTTON",L"开机自动启动",BS_AUTOCHECKBOX|WS_TABSTOP,
                         20,y4,350,28,ID_AUTORUN,g_fDlg);

        for (int i=0;i<3;++i) SendMessageW(g_refCombo,CB_ADDSTRING,0,(LPARAM)r3[i]);
        for (int i=0;i<4;++i) SendMessageW(g_alphaCombo,CB_ADDSTRING,0,(LPARAM)a4[i]);
        for (int i=0;i<2;++i) SendMessageW(g_cornerCombo,CB_ADDSTRING,0,(LPARAM)c2[i]);
        SendMessageW(g_refCombo,CB_SETCURSEL,
                     g_refreshMs==2000?1:(g_refreshMs==5000?2:0),0);
        SendMessageW(g_alphaCombo,CB_SETCURSEL,g_alphaLevel,0);
        SendMessageW(g_cornerCombo,CB_SETCURSEL,g_cornerMode,0);
        SendMessageW(g_autoChk,BM_SETCHECK,GetAutoRun()?BST_CHECKED:BST_UNCHECKED,0);

        /* 每个指标的显示开关与颜色 */
        AddCtl(h,L"STATIC",L"指标              显示     颜色",SS_LEFT,20,yHdr,360,22,ID_LBL2,g_fDlg);
        for (int i=0;i<3;++i) {
            int ry=yHdr+28+i*36;
            AddCtl(h,L"STATIC",g_nm[i],SS_LEFT,20,ry+4,110,24,ID_LBL1,g_fDlg);
            g_showChk[i]=AddCtl(h,L"BUTTON",L"",BS_AUTOCHECKBOX|WS_TABSTOP,
                                135,ry+3,28,26,ID_SHOW0+i,g_fDlg);
            SendMessageW(g_showChk[i],BM_SETCHECK,g_show[i]?BST_CHECKED:BST_UNCHECKED,0);
            g_colorCombo[i]=AddCtl(h,L"COMBOBOX",NULL,CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,
                                   185,ry,185,150,ID_COLOR0+i,g_fDlg);
            for (int k=0;k<NCOLORS;++k)
                SendMessageW(g_colorCombo[i],CB_ADDSTRING,0,(LPARAM)g_paletteNm[k]);
            SendMessageW(g_colorCombo[i],CB_SETCURSEL,PaletteIndexOf(g_ac[i]),0);
        }
        AddCtl(h,L"BUTTON",L"确定",BS_DEFPUSHBUTTON|WS_TABSTOP,95,yHdr+142,110,36,ID_OK,g_fDlg);
        AddCtl(h,L"BUTTON",L"取消",BS_PUSHBUTTON|WS_TABSTOP,225,yHdr+142,110,36,ID_CANCEL,g_fDlg);
        return 0;
    }
    case WM_COMMAND: {
        int id=LOWORD(wParam);
        if (id==ID_OK) {
            static const int ms[3]={1000,2000,5000};
            int ri=(int)SendMessageW(g_refCombo,CB_GETCURSEL,0,0);
            int ai=(int)SendMessageW(g_alphaCombo,CB_GETCURSEL,0,0);
            int ci=(int)SendMessageW(g_cornerCombo,CB_GETCURSEL,0,0);
            if (ri<0||ri>2) ri=0;
            if (ai<0||ai>3) ai=2;
            if (ci<0||ci>1) ci=0;
            BOOL show[3]; COLORREF color[3];
            for (int i=0;i<3;++i) {
                show[i] = SendMessageW(g_showChk[i],BM_GETCHECK,0,0)==BST_CHECKED;
                int k=(int)SendMessageW(g_colorCombo[i],CB_GETCURSEL,0,0);
                if (k<0||k>=NCOLORS) k=PaletteIndexOf(g_ac[i]);
                color[i]=g_palette[k];
            }
            ApplySettings(ms[ri],ai,
                          SendMessageW(g_autoChk,BM_GETCHECK,0,0)==BST_CHECKED,
                          ci, show, color);
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
        g_cornerCombo=NULL;
        for (int i=0;i<3;++i) { g_showChk[i]=NULL; g_colorCombo[i]=NULL; }
        return 0;
    }
    return DefWindowProcW(h,m,wParam,l);
}
static void OpenSettings(void) {
    if (g_setHwnd) { SetForegroundWindow(g_setHwnd); return; }
    int sw=GetSystemMetrics(SM_CXSCREEN), sh=GetSystemMetrics(SM_CYSCREEN);
    /* 客户区 400×240 逻辑像素；用 AdjustWindowRectExForDpi 算外框，避免高 DPI 裁切 */
    RECT rc; rc.left=0; rc.top=0; rc.right=S(400); rc.bottom=S(392);
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU;
    AdjustWindowRectDpi(&rc, style);
    int w=rc.right-rc.left, h=rc.bottom-rc.top;
    g_setHwnd=CreateWindowExW(0,L"SysMonSettings",L"系统监控 · 设置",style,
                              (sw-w)/2,(sh-h)/2,w,h,NULL,NULL,g_hInst,NULL);
    if (g_setHwnd) ShowWindow(g_setHwnd,SW_SHOW);
}

/* ---------------- 控制窗 ---------------- */
static void ShowAllTiles(void) {
    for (int i=0;i<3;++i)
        if (g_tileHwnd[i] && g_show[i]) ShowWindow(g_tileHwnd[i],SW_SHOW);
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
    AppendMenuW(h,MF_STRING|(g_pinned[g_pinTarget]?MF_CHECKED:MF_UNCHECKED),ID_PIN,
                g_pinned[g_pinTarget]?L"取消固定（沉底）":L"固定到最前");
    AppendMenuW(h,MF_SEPARATOR,0,NULL);
    AppendMenuW(h,MF_STRING|(GetAutoRun()?MF_CHECKED:MF_UNCHECKED),ID_AUTORUN_M,L"开机自启");
    AppendMenuW(h,MF_STRING,ID_REFRESH_M,L"立即刷新");
    AppendMenuW(h,MF_SEPARATOR,0,NULL);
    AppendMenuW(h,MF_STRING,ID_SETTINGS,L"设置…");
    AppendMenuW(h,MF_STRING,ID_ABOUT,L"关于…");
    AppendMenuW(h,MF_SEPARATOR,0,NULL);
    AppendMenuW(h,MF_STRING,ID_EXIT,L"退出");
    if (g_ctrl) SetForegroundWindow(g_ctrl);
    int id=TrackPopupMenu(h,TPM_RIGHTBUTTON|TPM_RETURNCMD,p.x,p.y,0,g_ctrl,NULL);
    DestroyMenu(h);
    switch (id) {
    case ID_PIN: TogglePin(g_pinTarget); break;
    case ID_AUTORUN_M: SetAutoRun(!GetAutoRun()); break;
    case ID_REFRESH_M:
        if (!g_gpuReady) { InitGPU(); g_gpuReady=TRUE; }
        SampleCPU(); SampleMem(); SampleGPU();
        for (int i=0;i<3;++i)
            if (g_tileHwnd[i]) PostMessageW(g_tileHwnd[i],WM_UPDATE,0,0);
        break;
    case ID_SETTINGS: OpenSettings(); break;
    case ID_ABOUT:
        MessageBoxW(NULL,L"系统监控 v1.3\n\nCPU / 内存 / GPU 独立贴片，可任意拖动。\n右上角图钉：置顶 / 沉底。\n原生 Win32，无运行时依赖。",
                    L"关于",MB_OK|MB_ICONINFORMATION);
        break;
    case ID_EXIT:
        if (g_ctrl) DestroyWindow(g_ctrl);
        break;
    }
}

/* ---------------- 贴片窗口过程 ---------------- */
static BOOL InPinArea(HWND hwnd,int idx) {
    POINT pt; GetCursorPos(&pt);
    RECT r; if (!GetWindowRect(hwnd,&r)) return FALSE;
    int x=pt.x-r.left, y=pt.y-r.top;
    int w=g_tile[idx].w;
    return (x>=w-S(36) && y<=S(36));
}
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
        if (InPinArea(hwnd,idx)) return HTCLIENT;   /* 图钉区走普通点击 */
        return HTCAPTION;                           /* 其余整块当标题栏拖动 */
    case WM_LBUTTONDOWN:
        if (InPinArea(hwnd,idx)) { TogglePin(idx); return 0; }
        return 0;
    case WM_EXITSIZEMOVE:
        SavePos(idx);
        return 0;
    case WM_RBUTTONUP:
        g_pinTarget = idx;
        if (g_ctrl) PostMessageW(g_ctrl,WM_POPUP,idx,0);
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
                ShowWindow(g_tileHwnd[i], g_show[i]?SW_SHOW:SW_HIDE);
                if (g_show[i]) PresentTile(i);
            }
        }
        ApplyPin();
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
        g_pinTarget = (int)w;
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

/* ---------------- 干净示例图导出（纯色背景，不含壁纸） ---------------- */
static void SaveBmp32(const wchar_t *path,int W,int H,void *bits) {
    BITMAPFILEHEADER bf; ZeroMemory(&bf,sizeof(bf));
    BITMAPINFOHEADER bi; ZeroMemory(&bi,sizeof(bi));
    bi.biSize=sizeof(BITMAPINFOHEADER); bi.biWidth=W; bi.biHeight=-H;
    bi.biPlanes=1; bi.biBitCount=32; bi.biCompression=BI_RGB;
    bi.biSizeImage=W*H*4;
    bf.bfType=0x4D42; bf.bfOffBits=sizeof(bf)+sizeof(bi);
    bf.bfSize=bf.bfOffBits+bi.biSizeImage;
    HANDLE f=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (f==INVALID_HANDLE_VALUE) return;
    DWORD wr; WriteFile(f,&bf,sizeof(bf),&wr,NULL);
    WriteFile(f,&bi,sizeof(bi),&wr,NULL);
    WriteFile(f,bits,W*H*4,&wr,NULL);
    CloseHandle(f);
}
static void ExportDemo(const wchar_t *path) {
    /* 固定演示数值 */
    g_cpu=23; g_mem=68; g_gpu=7;
    g_totalPhys=16ULL*1024*1024*1024;
    g_availPhys=g_totalPhys*32/100;
    g_sheen=0;
    int gap=S(18), W=S(TILE_W)*3+gap*4, H=S(TILE_H)+gap*2;
    HDC scr=GetDC(NULL);
    HDC cv=CreateCompatibleDC(scr);
    BITMAPINFO bi; ZeroMemory(&bi,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=W; bi.bmiHeader.biHeight=-H;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    void *cbits=NULL;
    HBITMAP cbmp=CreateDIBSection(scr,&bi,DIB_RGB_COLORS,&cbits,NULL,0);
    SelectObject(cv,cbmp);
    Fill(cv,0,0,W,H,RGB(24,28,38));   /* 纯深灰蓝背景，无壁纸 */
    for (int i=0;i<3;++i) {
        TILECTX tc; ZeroMemory(&tc,sizeof(tc));
        tc.index=i; tc.w=S(TILE_W); tc.h=S(TILE_H);
        HDC mdc=CreateCompatibleDC(scr);
        void *mbits=NULL;
        HBITMAP mbmp=CreateDIBSection(scr,&bi,DIB_RGB_COLORS,&mbits,NULL,0);
        HGDIOBJ old=SelectObject(mdc,mbmp);
        tc.memDC=mdc; tc.bits=mbits;
        DrawTileScene(&tc);            /* 不透明场景，直接 BitBlt */
        BitBlt(cv,gap+i*(S(TILE_W)+gap),gap,tc.w,tc.h,mdc,0,0,SRCCOPY);
        SelectObject(mdc,old);
        DeleteObject(mbmp); DeleteDC(mdc);
    }
    SaveBmp32(path,W,H,cbits);
    DeleteObject(cbmp); DeleteDC(cv); ReleaseDC(NULL,scr);
}

int WINAPI WinMain(HINSTANCE h,HINSTANCE hp,LPSTR cmdline,int nshow) {
    g_hInst=h;
    InitDpi();
    const wchar_t *cmd=GetCommandLineW();
    if (wcsstr(cmd,L"/autorun=on"))    return SetAutoRun(TRUE)?10:11;
    if (wcsstr(cmd,L"/autorun=off"))   return SetAutoRun(FALSE)?20:21;
    if (wcsstr(cmd,L"/autorun=query")) return GetAutoRun()?1:0;

#define MFONT(hh,ww) CreateFontW(S(hh),0,0,0,ww,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI")
    g_fName=MFONT(13,FW_SEMIBOLD);
    g_fNum=MFONT(34,FW_SEMIBOLD);
    g_fSub=MFONT(11,FW_NORMAL);
    g_fDlg=CreateFontW(S(13),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,0,0,L"Microsoft YaHei UI");
#undef MFONT

    /* 干净示例图导出：SysMonitor.exe /export=<path.bmp> */
    const wchar_t *ep=wcsstr(cmd,L"/export=");
    if (ep) {
        const wchar_t *p=ep+8;
        wchar_t out[MAX_PATH]; int k=0;
        while (*p && k<MAX_PATH-1) { out[k++]=*p++; }
        out[k]=0;
        ExportDemo(out);
        return 0;
    }

    LoadSettings();

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
