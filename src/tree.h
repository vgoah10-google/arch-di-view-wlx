// tree.h — Builds a file/folder hierarchy from the flat entry list and
// aggregates folder sizes, ready for the GDI tree renderer.
#ifndef ARCHDIVIEW_TREE_H
#define ARCHDIVIEW_TREE_H

#include "archive.h"

#include <memory>
#include <string>
#include <vector>

namespace archdiview {

// One node in the rendered tree.
struct TreeNode {
    std::wstring name;
    bool isDir = false;
    uint64_t packedSize = 0;      // self (files) or aggregated (folders)
    uint64_t unpackedSize = 0;    // self (files) or aggregated (folders)
    uint32_t method = 0;          // valid for files
    FILETIME mtime = {};
    std::vector<std::unique_ptr<TreeNode>> children;

    // Aggregation helpers.
    void Aggregate();
};

// Build a tree from the archive entries. Folders are created implicitly from
// path components; folder sizes are the sum of their descendants.
std::unique_ptr<TreeNode> BuildTree(const ArchiveInfo& info);

} // namespace archdiview

#endif // ARCHDIVIEW_TREE_H
