// view.cpp — GDI rendering of the ratio bar, detail table and vector tree.
#include "view.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace archdiview {

// HINSTANCE of this DLL, captured in DllMain. Window classes must be
// registered against the DLL's own module (not the host EXE's).
HINSTANCE g_hInst = nullptr;

// Plugin identity, shown in the right-click About box.
const wchar_t kPluginName[] = L"ArchDiView";
const wchar_t kPluginVersion[] = L"3.1";
const wchar_t kPluginAuthor[] = L"ArchDiView contributors";
const wchar_t kPluginDesc[] =
    L"Archive info Lister: compression-ratio bar, details and a contents tree.";

namespace {

const wchar_t kWndClass[] = L"ArchDiViewWnd";

// Right-click context-menu command id (About).
const int kMenuAbout = 1;

int Scale(int v, int dpi) { return MulDiv(v, dpi, 96); }

// Interpolate two colors by t in [0,1].
COLORREF LerpColor(COLORREF a, COLORREF b, double t) {
    t = std::max(0.0, std::min(1.0, t));
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    return RGB(r, g, bl);
}

} // namespace

View::View(HWND parent, const std::wstring& path, bool dark)
    : parent_(parent), dark_(dark), path_(path) {
    dpi_ = GetDeviceCaps(GetDC(parent), LOGPIXELSY);

    // Theme palette.
    if (dark_) {
        colBg_     = RGB(32, 32, 32);
        colFg_     = RGB(230, 230, 230);
        colMuted_  = RGB(160, 160, 160);
        colBarBg_  = RGB(58, 58, 58);
        colBarFg_  = RGB(86, 156, 214);
        colSaved_  = RGB(98, 184, 110);
        colLine_   = RGB(90, 90, 90);
        colAlt_    = RGB(40, 40, 40);
    } else {
        colBg_     = RGB(255, 255, 255);
        colFg_     = RGB(30, 30, 30);
        colMuted_  = RGB(110, 110, 110);
        colBarBg_  = RGB(225, 225, 225);
        colBarFg_  = RGB(0, 120, 215);
        colSaved_  = RGB(16, 145, 60);
        colLine_   = RGB(180, 180, 180);
        colAlt_    = RGB(246, 246, 246);
    }

    // Fonts.
    font_ = CreateFontW(Scale(14, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    fontBold_ = CreateFontW(Scale(14, dpi_), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    // A monospace font aligns the tree columns and box-drawing connectors.
    fontTree_ = CreateFontW(Scale(14, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    // Register the window class once.
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &View::WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kWndClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    DWORD style = WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN;
    hwnd_ = CreateWindowExW(0, kWndClass, L"ArchDiView", style,
                            0, 0, 0, 0, parent, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
#ifdef ARCHDIVIEW_DEBUG
        wchar_t eb[160];
        int n = swprintf(eb, 160, L"ArchDiView: CreateWindowExW failed gle=%lu class=%s",
                         (unsigned long)GetLastError(), kWndClass);
        wchar_t tp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tp)) {
            std::wstring lp = std::wstring(tp) + L"archdiview_debug.log";
            HANDLE h = CreateFileW(lp.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                   nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(h, eb, (DWORD)(n * 2), &w, nullptr);
                WriteFile(h, L"\n", 2, &w, nullptr);
                CloseHandle(h);
            }
        }
#endif
    }
    if (hwnd_)
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
}

View::~View() {
    if (font_) DeleteObject(font_);
    if (fontBold_) DeleteObject(fontBold_);
    if (fontTree_) DeleteObject(fontTree_);
    if (hwnd_) DestroyWindow(hwnd_);
}

bool View::Load() {
    if (!ParseArchive(path_, info_))
        return false;
    tree_ = BuildTree(info_);
    tree_->Aggregate();
    return true;
}

LRESULT CALLBACK View::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    View* self = (View*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        self = (View*)cs->lpCreateParams;
        // Record the HWND before any message handling: hwnd_ is not yet set by
        // the constructor (CreateWindowExW is still running) and HandleMessage
        // must not pass a null hwnd to DefWindowProcW during WM_NCCREATE.
        if (self) self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->HandleMessage(hwnd, msg, wp, lp);
}

LRESULT View::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE: {
            int w = LOWORD(lp), h = HIWORD(lp);
            RecalcLayout(w, h);
            UpdateScroll();
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_VSCROLL: {
            int maxScroll = treeContentHeight_ -
                            (L_.treeRowH ? 1 : 0); // refined below
            RECT rc; GetClientRect(hwnd, &rc);
            int viewH = rc.bottom - L_.treeTop;
            maxScroll = std::max(0, treeContentHeight_ - viewH);
            int old = scrollPos_;
            switch (LOWORD(wp)) {
                case SB_TOP: scrollPos_ = 0; break;
                case SB_BOTTOM: scrollPos_ = maxScroll; break;
                case SB_LINEUP: scrollPos_ -= L_.treeRowH; break;
                case SB_LINEDOWN: scrollPos_ += L_.treeRowH; break;
                case SB_PAGEUP: scrollPos_ -= viewH; break;
                case SB_PAGEDOWN: scrollPos_ += viewH; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION:
                    scrollPos_ = HIWORD(wp); break;
            }
            scrollPos_ = std::max(0, std::min(scrollPos_, maxScroll));
            if (scrollPos_ != old) {
                SCROLLINFO si = {sizeof(si)};
                si.fMask = SIF_POS; si.nPos = scrollPos_;
                SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            RECT rc; GetClientRect(hwnd, &rc);
            int viewH = rc.bottom - L_.treeTop;
            int maxScroll = std::max(0, treeContentHeight_ - viewH);
            int delta = -(int)GET_WHEEL_DELTA_WPARAM(wp);
            int old = scrollPos_;
            scrollPos_ += (delta / WHEEL_DELTA) * L_.treeRowH * 3;
            scrollPos_ = std::max(0, std::min(scrollPos_, maxScroll));
            if (scrollPos_ != old) {
                SCROLLINFO si = {sizeof(si)};
                si.fMask = SIF_POS; si.nPos = scrollPos_;
                SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // we fully paint in WM_PAINT
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc; GetClientRect(hwnd, &rc);
            // Double-buffer to avoid flicker.
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right - rc.left,
                                                 rc.bottom - rc.top);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            OnPaint(mem, rc);
            BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            return 0;
        case WM_RBUTTONUP: {
            // Right-click context menu (About for now).
            HMENU menu = CreatePopupMenu();
            if (!menu) return 0;
            AppendMenuW(menu, MF_STRING, kMenuAbout, L"About ArchDiView...");
            POINT pt;
            GetCursorPos(&pt);
            // TrackPopupMenu sends WM_COMMAND to hwnd when an item is chosen.
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                           pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            return 0;
        }
        case WM_COMMAND: {
            int id = (int)LOWORD(wp);
            if (id == kMenuAbout) {
                ShowAbout(hwnd);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Build and show the About box with plugin identity, version and a short
// capability summary.
void View::ShowAbout(HWND hwnd) {
    // Format/algorithm summary from the currently-loaded archive (if any).
    std::wstring fmt = info_.format;
    std::wstring line1 = std::wstring(kPluginName) + L" version " +
                         kPluginVersion;
    std::wstring msg;
    msg += line1 + L"\n\n";
    msg += std::wstring(kPluginDesc) + L"\n\n";
    if (!fmt.empty()) {
        msg += L"Currently viewing: " + fmt;
        if (!info_.algorithm.empty()) msg += L" (" + info_.algorithm + L")";
        wchar_t counts[96];
        swprintf(counts, 96, L" — %u files, %u folders",
                 (unsigned)info_.fileCount, (unsigned)info_.dirCount);
        msg += counts;
        msg += L"\n";
    } else {
        msg += L"No archive loaded.\n";
    }
    msg += L"\nSupports ZIP, TAR, GZip, TarGz, BZip2, Z, PPM, 7z, RAR, CAB, "
           L"CPIO, ARJ, LHA/LZH, ARC/PAK, SQZ, ZOO, HA, BH, DWC, ACE.\n";
    msg += L"7z and RAR use Total Commander's own archiver DLLs when "
           L"available.\n\n";
    msg += std::wstring(L"© ") + kPluginAuthor;

    std::wstring title = std::wstring(L"About ") + kPluginName;
    MessageBoxW(hwnd, msg.c_str(), title.c_str(),
                MB_OK | MB_ICONINFORMATION);
}

void View::RecalcLayout(int clientWidth, int clientHeight) {
    (void)clientHeight;
    L_.width = clientWidth;
    L_.barHeight = Scale(72, dpi_);
    L_.rowH = Scale(18, dpi_);
    L_.treeRowH = Scale(19, dpi_);
    L_.indent = Scale(18, dpi_);

    // Detail table rows: format/algorithm + 4 size/count rows.
    L_.tableRows = 6;
    L_.tableHeight = L_.tableRows * L_.rowH;
    L_.headerHeight = L_.barHeight + L_.tableHeight + Scale(8, dpi_);
    L_.treeTop = L_.headerHeight;
}

void View::UpdateScroll() {
    RECT rc; GetClientRect(hwnd_, &rc);
    int viewH = rc.bottom - L_.treeTop;
    int maxScroll = std::max(0, treeContentHeight_ - viewH);
    scrollPos_ = std::max(0, std::min(scrollPos_, maxScroll));
    SCROLLINFO si = {sizeof(si)};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = treeContentHeight_;
    si.nPage = (UINT)std::max(1, viewH);
    si.nPos = scrollPos_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void View::LayoutTree() {
    rows_.clear();
    std::vector<bool> ancestorLast;
    // Skip drawing the artificial empty root; render its children at depth 0.
    if (tree_) {
        size_t n = tree_->children.size();
        for (size_t i = 0; i < n; ++i) {
            TreeNode* c = tree_->children[i].get();
            ancestorLast.clear();
            BuildFlattened(c, 0, ancestorLast, i + 1 == n);
        }
    }
    treeContentHeight_ = (int)rows_.size() * L_.treeRowH;
}

void View::BuildFlattened(TreeNode* node, int depth,
                          std::vector<bool>& ancestorLast, bool lastChild) {
    TreeRow row;
    row.node = node;
    row.depth = depth;
    row.lastChild = lastChild;
    row.ancestorLast = ancestorLast;
    rows_.push_back(row);

    if (node->isDir) {
        ancestorLast.push_back(lastChild);
        size_t n = node->children.size();
        for (size_t i = 0; i < n; ++i) {
            BuildFlattened(node->children[i].get(), depth + 1, ancestorLast,
                           i + 1 == n);
        }
        ancestorLast.pop_back();
    }
}

void View::OnPaint(HDC hdc, const RECT& client) {
    LayoutTree();
    int w = client.right - client.left;

    // Background.
    HBRUSH bg = CreateSolidBrush(colBg_);
    FillRect(hdc, &client, bg);
    DeleteObject(bg);

    DrawRatioBar(hdc, Scale(12, dpi_), Scale(10, dpi_),
                 w - Scale(24, dpi_), L_);
    DrawTable(hdc, Scale(12, dpi_), L_.barHeight, w - Scale(24, dpi_), L_);

    int treeH = (client.bottom - client.top) - L_.treeTop;
    if (treeH > 0)
        DrawTree(hdc, Scale(12, dpi_), L_.treeTop, w - Scale(24, dpi_), treeH, L_);
}

void View::DrawRatioBar(HDC hdc, int x, int y, int w, const Layout& L) {
    (void)L;
    SetBkMode(hdc, TRANSPARENT);

    // Title line.
    HFONT of = (HFONT)SelectObject(hdc, fontBold_);
    SetTextColor(hdc, colFg_);
    std::wstring title = L"Compression ratio";
    TextOutW(hdc, x, y, title.c_str(), (int)title.size());

    // Ratio value, right-aligned. When the unpacked size is unknown (0), the
    // ratio is meaningless — show a clear "size unknown" label instead of a
    // misleading "saved 100%".
    double ratio = info_.ratio();
    wchar_t pct[64];
    if (info_.totalUnpacked == 0) {
        wcscpy_s(pct, L"size unknown");
    } else {
        swprintf(pct, 64, L"%.1f%%  (saved %.1f%%)", ratio * 100.0, info_.saved() * 100.0);
    }
    SIZE sz; GetTextExtentPoint32W(hdc, pct, (int)wcslen(pct), &sz);
    TextOutW(hdc, x + w - sz.cx, y, pct, (int)wcslen(pct));
    SelectObject(hdc, of);

    int barY = y + Scale(24, dpi_);
    int barH = Scale(22, dpi_);

    // Track.
    RECT track = {x, barY, x + w, barY + barH};
    HBRUSH tb = CreateSolidBrush(colBarBg_);
    FillRect(hdc, &track, tb);
    DeleteObject(tb);

    // Filled portion = compression ratio (packed / unpacked).
    double frac = std::max(0.0, std::min(1.0, ratio));
    int fillW = (int)(w * frac);
    if (fillW > 0) {
        RECT fill = {x, barY, x + fillW, barY + barH};
        // Gradient from the theme bar color to the saved color for a nicer look.
        int steps = std::max(1, fillW);
        for (int i = 0; i < steps; ++i) {
            double t = (double)i / steps;
            COLORREF c = LerpColor(colSaved_, colBarFg_, t);
            RECT col = {x + i, barY, x + i + 1, barY + barH};
            HBRUSH b = CreateSolidBrush(c);
            FillRect(hdc, &col, b);
            DeleteObject(b);
        }
    }

    // Border.
    HBRUSH oldB = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    HPEN pen = CreatePen(PS_SOLID, 1, colLine_);
    HPEN oldP = (HPEN)SelectObject(hdc, pen);
    Rectangle(hdc, x, barY, x + w + 1, barY + barH + 1);
    SelectObject(hdc, oldP);
    DeleteObject(pen);
    SelectObject(hdc, oldB);

    // Size labels under the bar.
    SelectObject(hdc, font_);
    SetTextColor(hdc, colMuted_);
    std::wstring packedS = L"Packed: " + FormatBytes(info_.totalPacked);
    std::wstring unpackS = L"Unpacked: " + FormatBytes(info_.totalUnpacked);
    TextOutW(hdc, x, barY + barH + Scale(4, dpi_), packedS.c_str(),
             (int)packedS.size());
    SIZE sz2; GetTextExtentPoint32W(hdc, unpackS.c_str(), (int)unpackS.size(), &sz2);
    TextOutW(hdc, x + w - sz2.cx, barY + barH + Scale(4, dpi_), unpackS.c_str(),
             (int)unpackS.size());
}

void View::DrawTable(HDC hdc, int x, int y, int w, const Layout& L) {
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, font_);

    struct Row { std::wstring label, value; bool bold; };
    std::vector<Row> rows;
    rows.push_back({L"Format", info_.format, false});
    rows.push_back({L"Algorithm", info_.algorithm, false});
    rows.push_back({L"Files", std::to_wstring(info_.fileCount), false});
    rows.push_back({L"Folders", std::to_wstring(info_.dirCount), false});
    rows.push_back({L"Unpacked size", FormatBytes(info_.totalUnpacked), false});
    rows.push_back({L"Packed size", FormatBytes(info_.totalPacked), false});

    int labelW = Scale(130, dpi_);
    int yy = y + Scale(4, dpi_);
    int i = 0;
    for (const auto& r : rows) {
        // Alternate row background.
        if (i % 2 == 1) {
            RECT rb = {x, yy, x + w, yy + L_.rowH};
            HBRUSH b = CreateSolidBrush(colAlt_);
            FillRect(hdc, &rb, b);
            DeleteObject(b);
        }
        SetTextColor(hdc, colMuted_);
        SelectObject(hdc, font_);
        TextOutW(hdc, x + Scale(4, dpi_), yy + 1, r.label.c_str(),
                 (int)r.label.size());
        SetTextColor(hdc, colFg_);
        SelectObject(hdc, r.bold ? fontBold_ : font_);
        TextOutW(hdc, x + labelW, yy + 1, r.value.c_str(), (int)r.value.size());
        yy += L_.rowH;
        ++i;
    }
    (void)y;
}

void View::DrawTree(HDC hdc, int x, int y, int w, int h, const Layout& L) {
    SetBkMode(hdc, TRANSPARENT);
    SelectObject(hdc, fontTree_);

    // Header separator + caption.
    HPEN pen = CreatePen(PS_SOLID, 1, colLine_);
    HPEN oldP = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, x, y + Scale(2, dpi_), nullptr);
    LineTo(hdc, x + w, y + Scale(2, dpi_));
    SelectObject(hdc, oldP);
    DeleteObject(pen);

    SetTextColor(hdc, colMuted_);
    SelectObject(hdc, fontBold_);
    std::wstring cap = L"Contents (" + std::to_wstring(info_.fileCount) +
                       L" files, " + std::to_wstring(info_.dirCount) + L" folders)";
    TextOutW(hdc, x, y + Scale(6, dpi_), cap.c_str(), (int)cap.size());

    int top = y + Scale(26, dpi_);
    int availH = (y + h) - top;

    // Clip to the tree region so scrolled rows don't overwrite the header.
    RECT clip = {x, top, x + w, top + availH};
    HRGN rgn = CreateRectRgnIndirect(&clip);
    SelectClipRgn(hdc, rgn);

    // Column layout for sizes.
    int sizeColW = Scale(90, dpi_);
    int methodColW = info_.perEntryCompression ? Scale(110, dpi_) : 0;
    int nameMaxW = w - (sizeColW * 2 + methodColW) - Scale(8, dpi_);

    int yy = top - scrollPos_;
    for (const auto& row : rows_) {
        int rowY = yy;
        int nextY = yy + L_.treeRowH;
        yy = nextY;
        if (nextY < top) continue;           // above the visible area
        if (rowY >= top + availH) break;     // below the visible area

        TreeNode* n = row.node;
        int indentX = x + row.depth * L_.indent;
        int midY = rowY + L_.treeRowH / 2;

        // Vector tree connectors (box-drawing style, drawn as lines).
        HPEN tpen = CreatePen(PS_SOLID, 1, colLine_);
        HPEN otp = (HPEN)SelectObject(hdc, tpen);
        // Vertical line from parent down through this row for each ancestor
        // that is not the last child.
        for (int d = 0; d < row.depth; ++d) {
            if (d < (int)row.ancestorLast.size() && !row.ancestorLast[d]) {
                int vx = x + d * L_.indent + L_.indent / 2;
                MoveToEx(hdc, vx, rowY, nullptr);
                LineTo(hdc, vx, nextY);
            }
        }
        // Horizontal connector into this node.
        if (row.depth > 0) {
            int px = x + (row.depth - 1) * L_.indent + L_.indent / 2;
            MoveToEx(hdc, px, midY, nullptr);
            LineTo(hdc, indentX + L_.indent / 2, midY);
            // Branch tick.
            MoveToEx(hdc, indentX + L_.indent / 2, midY, nullptr);
            LineTo(hdc, indentX + L_.indent / 2, nextY);
        }
        SelectObject(hdc, otp);
        DeleteObject(tpen);

        // Node marker: a small filled square for folders, hollow for files.
        int box = Scale(9, dpi_);
        int bx = indentX + L_.indent / 2 - box / 2;
        int by = midY - box / 2;
        HBRUSH mark = CreateSolidBrush(n->isDir ? colBarFg_ : colFg_);
        if (n->isDir) {
            RECT fr = {bx, by, bx + box, by + box};
            FillRect(hdc, &fr, mark);
        } else {
            HBRUSH oldB = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RECT fr = {bx, by, bx + box, by + box};
            FillRect(hdc, &fr, mark);
            SelectObject(hdc, oldB);
        }
        DeleteObject(mark);

        // Name.
        SetTextColor(hdc, n->isDir ? colFg_ : colFg_);
        SelectObject(hdc, n->isDir ? fontBold_ : fontTree_);
        std::wstring label = n->name;
        if (n->isDir) label += L"/";
        // Ellipsize the name to fit the available width.
        int maxNamePx = nameMaxW - row.depth * L_.indent;
        SIZE ls; GetTextExtentPoint32W(hdc, label.c_str(), (int)label.size(), &ls);
        std::wstring shown = label;
        if (ls.cx > maxNamePx && maxNamePx > Scale(20, dpi_)) {
            int keep = (int)label.size();
            while (keep > 1) {
                --keep;
                std::wstring cand = label.substr(0, keep) + L"…";
                GetTextExtentPoint32W(hdc, cand.c_str(), (int)cand.size(), &ls);
                if (ls.cx <= maxNamePx) { shown = cand; break; }
            }
        }
        TextOutW(hdc, indentX + L_.indent, rowY + 1, shown.c_str(),
                 (int)shown.size());

        // Sizes: "packed / unpacked". Right-align each in its column.
        SelectObject(hdc, fontTree_);
        SetTextColor(hdc, colMuted_);
        std::wstring packS = FormatBytes(n->packedSize);
        std::wstring unpS = FormatBytes(n->unpackedSize);
        int packX = x + w - methodColW - sizeColW * 2;
        int unpX  = x + w - methodColW - sizeColW;
        int methX = x + w - methodColW;

        // Packed (right-aligned in its column).
        SIZE ps; GetTextExtentPoint32W(hdc, packS.c_str(), (int)packS.size(), &ps);
        TextOutW(hdc, packX + sizeColW - ps.cx - Scale(6, dpi_), rowY + 1,
                 packS.c_str(), (int)packS.size());
        // Unpacked.
        SIZE us; GetTextExtentPoint32W(hdc, unpS.c_str(), (int)unpS.size(), &us);
        TextOutW(hdc, unpX + sizeColW - us.cx - Scale(6, dpi_), rowY + 1,
                 unpS.c_str(), (int)unpS.size());
        // Method (files only, and only when compression is per-entry, e.g. ZIP).
        if (!n->isDir && info_.perEntryCompression && !n->methodName.empty()) {
            SetTextColor(hdc, colMuted_);
            TextOutW(hdc, methX, rowY + 1, n->methodName.c_str(),
                     (int)n->methodName.size());
        }
    }

    SelectClipRgn(hdc, nullptr);
    DeleteObject(rgn);
    (void)availH;
}

} // namespace archdiview
