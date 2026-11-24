// tree.cpp — Builds the file/folder hierarchy and aggregates folder sizes.
#include "tree.h"

#include <algorithm>
#include <map>

namespace archdiview {
namespace {

// Compare children: folders first, then case-insensitive by name.
bool NodeLess(const std::unique_ptr<TreeNode>& a, const std::unique_ptr<TreeNode>& b) {
    if (a->isDir != b->isDir) return a->isDir; // dirs first
    return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0;
}

} // namespace

void TreeNode::Aggregate() {
    if (!isDir) return;
    uint64_t p = 0, u = 0;
    for (auto& c : children) {
        c->Aggregate();
        p += c->packedSize;
        u += c->unpackedSize;
    }
    packedSize = p;
    unpackedSize = u;
}

std::unique_ptr<TreeNode> BuildTree(const ArchiveInfo& info) {
    auto root = std::make_unique<TreeNode>();
    root->name = L"";
    root->isDir = true;

    // Insert each entry by walking/creating its path components.
    for (const auto& ent : info.entries) {
        TreeNode* cur = root.get();
        // Split the path on '/' (and tolerate '\\').
        std::wstring path = ent.path;
        size_t start = 0;
        std::vector<std::wstring> parts;
        for (size_t i = 0; i <= path.size(); ++i) {
            if (i == path.size() || path[i] == L'/' || path[i] == L'\\') {
                if (i > start) parts.push_back(path.substr(start, i - start));
                start = i + 1;
            }
        }
        if (parts.empty()) continue;

        // Walk/create intermediate folders.
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            TreeNode* next = nullptr;
            for (auto& c : cur->children)
                if (c->isDir && _wcsicmp(c->name.c_str(), parts[i].c_str()) == 0) {
                    next = c.get();
                    break;
                }
            if (!next) {
                auto node = std::make_unique<TreeNode>();
                node->name = parts[i];
                node->isDir = true;
                next = node.get();
                cur->children.push_back(std::move(node));
            }
            cur = next;
        }
        // The final component is the entry itself.
        const std::wstring& leaf = parts.back();
        TreeNode* existing = nullptr;
        for (auto& c : cur->children)
            if (c->isDir == ent.isDir && _wcsicmp(c->name.c_str(), leaf.c_str()) == 0) {
                existing = c.get();
                break;
            }
        if (existing) {
            // Merge into an existing node (e.g. an implicit folder that is also
            // an explicit archive entry, or duplicate entries).
            if (!existing->isDir) {
                existing->packedSize += ent.packedSize;
                existing->unpackedSize += ent.unpackedSize;
            }
        } else {
            auto node = std::make_unique<TreeNode>();
            node->name = leaf;
            node->isDir = ent.isDir;
            node->packedSize = ent.packedSize;
            node->unpackedSize = ent.unpackedSize;
            node->method = ent.method;
            node->mtime = ent.mtime;
            cur->children.push_back(std::move(node));
        }
    }

    // Sort children recursively and aggregate folder sizes.
    struct Finalize {
        static void Run(TreeNode* n) {
            std::sort(n->children.begin(), n->children.end(), NodeLess);
            n->Aggregate();
            for (auto& c : n->children) Run(c.get());
        }
    };
    Finalize::Run(root.get());
    return root;
}

} // namespace archdiview
