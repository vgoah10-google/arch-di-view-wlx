// test_parse.cpp — Console harness that exercises the archive parsers directly
// (no window). Prints the compression-ratio bar data, the detail table and the
// tree for each archive passed on the command line. Used to verify parsing and
// the inflate() implementation against real archives created by zip/tar/gzip.
#include "archive.h"
#include "tree.h"

#include <cstdio>
#include <string>
#include <vector>
#include <io.h>
#include <fcntl.h>

using namespace archdiview;

static void PrintBar(const ArchiveInfo& info) {
    const int width = 40;
    if (info.totalUnpacked == 0) {
        wprintf(L"  [%*s] size unknown\n", width, L"");
    } else {
        double ratio = info.ratio();
        int fill = (int)(width * std::max(0.0, std::min(1.0, ratio)));
        wprintf(L"  [");
        for (int i = 0; i < width; ++i) wprintf(L"%c", i < fill ? L'\x2588' : L' ');
        wprintf(L"] %.1f%%  (saved %.1f%%)\n", ratio * 100.0, info.saved() * 100.0);
    }
    wprintf(L"  Packed: %s   Unpacked: %s\n",
            FormatBytes(info.totalPacked).c_str(),
            FormatBytes(info.totalUnpacked).c_str());
}

static void PrintTable(const ArchiveInfo& info) {
    struct Row { const wchar_t* k; std::wstring v; };
    Row rows[] = {
        {L"Format",         info.format},
        {L"Algorithm",      info.algorithm},
        {L"Files",          std::to_wstring(info.fileCount)},
        {L"Folders",        std::to_wstring(info.dirCount)},
        {L"Unpacked size",  FormatBytes(info.totalUnpacked)},
        {L"Packed size",    FormatBytes(info.totalPacked)},
    };
    for (const auto& r : rows)
        wprintf(L"  %-14s %s\n", r.k, r.v.c_str());
}

static void PrintTree(const TreeNode* node, int depth,
                      const std::vector<bool>& ancestorLast, bool lastChild) {
    if (depth >= 0) {
        // Draw connectors.
        for (int d = 0; d < depth; ++d) {
            bool cont = (d < (int)ancestorLast.size()) && !ancestorLast[d];
            wprintf(L"   %s", cont ? L"\x2502" : L" ");
        }
        wprintf(L"  %s\x2500%s ", lastChild ? L"\x2514" : L"\x251c",
                node->isDir ? L"\x25a0" : L"\x25a1");
        std::wstring label = node->name;
        if (node->isDir) label += L"/";
        wprintf(L"%-24s %10s %10s",
                label.c_str(),
                FormatBytes(node->packedSize).c_str(),
                FormatBytes(node->unpackedSize).c_str());
        if (!node->isDir && !node->methodName.empty())
            wprintf(L"  %s", node->methodName.c_str());
        wprintf(L"\n");
    }
    if (node->isDir) {
        std::vector<bool> al = ancestorLast;
        al.push_back(lastChild);
        size_t n = node->children.size();
        for (size_t i = 0; i < n; ++i)
            PrintTree(node->children[i].get(),
                      depth < 0 ? 0 : depth + 1, al, i + 1 == n);
    }
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    if (argc < 2) {
        wprintf(L"usage: test_parse <archive> [...]\n");
        return 2;
    }
    int rc = 0;
    for (int a = 1; a < argc; ++a) {
        std::wstring path = argv[a];
        wprintf(L"\n===== %s =====\n", path.c_str());
        ArchiveInfo info;
        if (!ParseArchive(path, info)) {
            wprintf(L"  PARSE FAILED: %s\n", info.error.c_str());
            rc = 1;
            continue;
        }
        PrintBar(info);
        PrintTable(info);
        auto tree = BuildTree(info);
        tree->Aggregate();
        wprintf(L"  Contents (%u files, %u folders):\n", info.fileCount, info.dirCount);
        // Print root's children at depth 0.
        std::vector<bool> al;
        size_t n = tree->children.size();
        for (size_t i = 0; i < n; ++i)
            PrintTree(tree->children[i].get(), 0, al, i + 1 == n);
    }
    return rc;
}
