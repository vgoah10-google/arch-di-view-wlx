// plugin.cpp — WLX (Lister) plugin entry points for ArchDiView.
//
// Total Commander calls ListLoad to open an archive; we parse its metadata and
// host a GDI-drawn child window that shows the compression ratio, details and a
// tree of the contents. No WebView, no external archiver DLLs.
#include "wlxplugin.h"
#include "view.h"

#include <map>
#include <mutex>
#include <string>

using namespace archdiview;

namespace {

// Map the returned plugin window handle -> View instance.
std::mutex g_mutex;
std::map<HWND, View*> g_views;

View* ViewFromWnd(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_views.find(hwnd);
    return it == g_views.end() ? nullptr : it->second;
}

// Append a line to %TEMP%\\archdiview_debug.log for diagnosing load failures.
void DebugLogW(const wchar_t* msg) {
    wchar_t tmp[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp) == 0) return;
    std::wstring p = std::wstring(tmp) + L"archdiview_debug.log";
    HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, msg, (DWORD)(wcslen(msg) * 2), &written, nullptr);
    WriteFile(h, L"\n", 2, &written, nullptr);
    CloseHandle(h);
}

void RegisterView(HWND hwnd, View* v) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_views[hwnd] = v;
}

void UnregisterView(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_views.erase(hwnd);
}

} // namespace

extern "C" {

// Load `path` into a new plugin child window under `parentWin`.
DCPCALL HWND ListLoad(HWND parentWin, char* fileToLoad, int showFlags) {
    if (!parentWin || !fileToLoad)
        return nullptr;

    bool dark = (showFlags & lcp_darkmode) != 0;
    int wlen = MultiByteToWideChar(CP_ACP, 0, fileToLoad, -1, nullptr, 0);
    std::wstring path(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, fileToLoad, -1, &path[0], wlen);
    if (!path.empty() && path.back() == L'\0') path.pop_back();

    // Size to the parent client area (captured before creating the view).
    RECT rc;
    GetClientRect(parentWin, &rc);

    View* view = new View(parentWin, path, dark);
    if (!view->hwnd()) {
        wchar_t eb[256];
        int n = swprintf(eb, 256,
            L"ArchDiView: window creation failed parent=%p IsWindow=%d rect=%ldx%ld",
            (void*)parentWin, (int)IsWindow(parentWin),
            (long)rc.right, (long)rc.bottom);
        DebugLogW(eb);
        (void)n;
        delete view;
        return nullptr;
    }
    // Size to the parent client area.
    MoveWindow(view->hwnd(), rc.left, rc.top,
               rc.right - rc.left, rc.bottom - rc.top, TRUE);

    if (!view->Load()) {
        DebugLogW(L"ArchDiView: Load failed");
        DebugLogW(view->info().error.c_str());
        UnregisterView(view->hwnd());
        delete view;
        return nullptr;
    }
    DebugLogW(L"ArchDiView: Load OK");
    RegisterView(view->hwnd(), view);
    InvalidateRect(view->hwnd(), nullptr, TRUE);
    return view->hwnd();
}

DCPCALL void ListCloseWindow(HWND listWin) {
    if (!listWin) return;
    View* view = ViewFromWnd(listWin);
    UnregisterView(listWin);
    delete view;
}

DCPCALL void ListGetDetectString(char* detectString, int maxlen) {
    // Formats ArchDiView parses natively (no external DLLs).
    lstrcpynA(detectString,
              "EXT=\"ZIP\"|EXT=\"JAR\"|EXT=\"TAR\"|EXT=\"GZ\"|EXT=\"TGZ\"|"
              "EXT=\"TBZ2\"|EXT=\"TAR.GZ\"",
              maxlen);
}

DCPCALL int ListSendCommand(HWND listWin, int command, int parameter) {
    (void)listWin;
    (void)command;
    (void)parameter;
    return LISTPLUGIN_OK;
}

DCPCALL void ListSetDefaultParams(ListDefaultParamStruct* dps) {
    (void)dps;
}

// DLL entry point. Capture this DLL's HINSTANCE so the View can register its
// window class against the correct module (not the host EXE).
BOOL APIENTRY DllMain(HANDLE hModule, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH)
        archdiview::g_hInst = (HINSTANCE)hModule;
    return TRUE;
}

} // extern "C"
