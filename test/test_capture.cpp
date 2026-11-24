// test_capture.cpp — Loads ArchDiView.wlx64, renders an archive, and captures
// the GDI-rendered plugin window to a PNG/BMP via PrintWindow.
//   usage: test_capture <archive> <out.bmp> [width] [height] [dark]
#include <windows.h>
#include <stdio.h>
#include <tchar.h>
#include <vector>

typedef HWND(__stdcall* ListLoadA_t)(HWND, char*, int);
typedef void(__stdcall* ListCloseWindow_t)(HWND);

// Minimal BMP writer (32-bit BGRA, rows bottom-up).
static void writeBmp(const char* path, int w, int h, const unsigned char* bgra) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    int row = ((w * 4 + 3) / 4) * 4;
    unsigned int dataSize = row * h;
    unsigned int fileSize = 54 + dataSize;
    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = fileSize; hdr[3] = fileSize >> 8; hdr[4] = fileSize >> 16; hdr[5] = fileSize >> 24;
    hdr[10] = 54;
    hdr[14] = 40; hdr[18] = w; hdr[19] = w >> 8; hdr[20] = w >> 16; hdr[21] = w >> 24;
    hdr[22] = h; hdr[23] = h >> 8; hdr[24] = h >> 16; hdr[25] = h >> 24;
    hdr[26] = 1; hdr[28] = 32;
    hdr[34] = dataSize; hdr[35] = dataSize >> 8; hdr[36] = dataSize >> 16; hdr[37] = dataSize >> 24;
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> rowBuf(row, 0);
    for (int y = h - 1; y >= 0; y--) {
        memcpy(rowBuf.data(), bgra + (size_t)y * w * 4, (size_t)w * 4);
        fwrite(rowBuf.data(), 1, row, f);
    }
    fclose(f);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        wprintf(L"usage: test_capture <archive> <out.bmp> [width] [height] [dark]\n");
        return 2;
    }
    char pathA[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, argv[1], -1, pathA, MAX_PATH, nullptr, nullptr);
    char outA[MAX_PATH] = {0};
    WideCharToMultiByte(CP_ACP, 0, argv[2], -1, outA, MAX_PATH, nullptr, nullptr);
    int width  = argc >= 4 ? _wtoi(argv[3]) : 900;
    int height = argc >= 5 ? _wtoi(argv[4]) : 640;
    int dark   = argc >= 6 ? _wtoi(argv[5]) : 0;

    HMODULE mod = LoadLibraryW(L"ArchDiView.wlx64");
    if (!mod) { wprintf(L"FAIL: LoadLibrary (%lu)\n", GetLastError()); return 1; }
    auto listLoad  = (ListLoadA_t)GetProcAddress(mod, "ListLoad");
    auto listClose = (ListCloseWindow_t)GetProcAddress(mod, "ListCloseWindow");
    if (!listLoad) { wprintf(L"FAIL: ListLoad\n"); return 1; }

    HWND parent = CreateWindowExW(0, L"STATIC", L"capture", WS_OVERLAPPEDWINDOW,
                                  0, 0, width, height, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
    ShowWindow(parent, SW_SHOW);
    UpdateWindow(parent);
    HWND viewer = listLoad(parent, pathA, dark ? 128 : 0);
    if (!viewer) { wprintf(L"FAIL: ListLoad returned null\n"); return 1; }

    // Let it paint.
    MSG msg;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 800) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }

    // Capture the viewer window's client area via PrintWindow.
    RECT rc; GetClientRect(viewer, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC wdc = GetWindowDC(viewer);
    HDC mem = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    PrintWindow(viewer, mem, PW_CLIENTONLY);
    BITMAPINFO bi = {sizeof(bi)};
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> px((size_t)w * h * 4);
    GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    writeBmp(outA, w, h, px.data());
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(viewer, wdc);

    wprintf(L"OK: captured %dx%d to %hs\n", w, h, outA);
    listClose(viewer);
    FreeLibrary(mod);
    return 0;
}
