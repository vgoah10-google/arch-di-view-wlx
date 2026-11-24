// view.h — The GDI-rendered ArchDiView Lister child window.
//
// Draws, top to bottom:
//   1. A horizontal compression-ratio bar (packed vs unpacked).
//   2. A detail table (format, algorithm, sizes, file/dir counts, ratio).
//   3. A vector tree diagram of every file/folder (like `ouch -t`), with
//      box-drawing connectors and per-node sizes.
// Supports vertical scrolling for the tree and both light/dark Lister themes.
#ifndef ARCHDIVIEW_VIEW_H
#define ARCHDIVIEW_VIEW_H

#include "archive.h"
#include "tree.h"

#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace archdiview {

// HINSTANCE of this DLL, captured in DllMain. Window classes must be
// registered against the DLL's own module (not the host EXE's).
extern HINSTANCE g_hInst;

class View {
public:
    View(HWND parent, const std::wstring& path, bool dark);
    ~View();

    bool Load();                 // parse + build tree; false on failure
    HWND hwnd() const { return hwnd_; }
    const ArchiveInfo& info() const { return info_; }

private:
    // Layout metrics (computed on resize).
    struct Layout {
        int width = 0;
        int headerHeight = 0;    // bar + detail table height (fixed, no scroll)
        int barHeight = 64;
        int tableHeight = 0;     // rows * rowH
        int rowH = 18;
        int tableRows = 0;
        int treeTop = 0;         // y where the tree begins
        int treeRowH = 18;
        int indent = 18;         // px per tree depth level
    };

    // A visible row of the flattened tree.
    struct TreeRow {
        TreeNode* node = nullptr;
        int depth = 0;
        bool lastChild = false;          // is last among its siblings
        std::vector<bool> ancestorLast;  // per-depth: ancestor is last child
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void OnPaint(HDC hdc, const RECT& client);
    void DrawRatioBar(HDC hdc, int x, int y, int w, const Layout& L);
    void DrawTable(HDC hdc, int x, int y, int w, const Layout& L);
    void DrawTree(HDC hdc, int x, int y, int w, int h, const Layout& L);
    void LayoutTree();           // flatten visible rows + compute total height
    void UpdateScroll();
    void RecalcLayout(int clientWidth, int clientHeight);

    void BuildFlattened(TreeNode* node, int depth,
                        std::vector<bool>& ancestorLast, bool lastChild);

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    bool dark_ = false;
    std::wstring path_;
    ArchiveInfo info_;
    std::unique_ptr<TreeNode> tree_;
    std::vector<TreeRow> rows_;

    Layout L_;
    int scrollPos_ = 0;          // tree scroll offset in px
    int treeContentHeight_ = 0;  // total px height of all tree rows

    HFONT font_ = nullptr;
    HFONT fontBold_ = nullptr;
    HFONT fontTree_ = nullptr;
    int dpi_ = 96;

    // Theme colors.
    COLORREF colBg_, colFg_, colMuted_, colBarBg_, colBarFg_, colSaved_,
             colLine_, colAlt_;
};

} // namespace archdiview

#endif // ARCHDIVIEW_VIEW_H
