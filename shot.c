/* shot.exe <输出.bmp> —— 抓整屏，用于生成 README 效果图 */
#include <windows.h>
#include <stdio.h>
int main(int argc, char **argv) {
    SetProcessDPIAware();
    if (argc < 2) return 2;
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=w; bi.bmiHeader.biHeight=-h;
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32;
    bi.bmiHeader.biCompression=BI_RGB;
    void *bits=NULL;
    HBITMAP bmp=CreateDIBSection(mem,&bi,DIB_RGB_COLORS,&bits,NULL,0);
    SelectObject(mem,bmp);
    BitBlt(mem,0,0,w,h,screen,0,0,SRCCOPY|CAPTUREBLT);
    BITMAPFILEHEADER bf; ZeroMemory(&bf,sizeof(bf));
    bf.bfType=0x4D42; bf.bfOffBits=sizeof(bf)+sizeof(BITMAPINFOHEADER);
    bf.bfSize=bf.bfOffBits+(DWORD)w*h*4;
    BITMAPINFOHEADER ih=bi.bmiHeader; ih.biSizeImage=(DWORD)w*h*4;
    HANDLE f=CreateFileA(argv[1],GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (f!=INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(f,&bf,sizeof(bf),&wr,NULL);
        WriteFile(f,&ih,sizeof(ih),&wr,NULL); WriteFile(f,bits,(DWORD)w*h*4,&wr,NULL); CloseHandle(f); }
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL,screen);
    printf("shot %dx%d -> %s\n", w, h, argv[1]);
    return 0;
}
