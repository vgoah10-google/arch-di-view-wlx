// archive_simple.cpp — Native parsers for single-member / stream formats
// (no external DLL). Ports the proven header logic from ArchView's
// TBZip2Tbz2, TZ and TPpm into the ArchDiView parser interface:
//
//   * BZip2 / TBz2 (.bz2/.tbz2) — 'B','Z','h' + level digit; one logical
//     member. The bzip2 header carries no unpacked size, so the unpacked
//     size is unknown and reported as 0 (the ratio bar handles 0).
//   * Z / compress (.z) — magic 0x1F 0x9D (ID3 = mode/flags byte); one
//     logical member, unpacked size not stored reliably (reported as 0).
//   * PPM (.ppm) — PPMD_A header with method, memory size, original size,
//     time and a stored member name.
//
// Each format holds a single member, so each parser pushes exactly one
// ArchiveEntry and sets perEntryCompression = false.
#include "archive.h"

#include "archive_internal.h"

#include <cwchar>
#include <wchar.h>   // _wcsicmp

namespace archdiview {
namespace {

// RAII wrapper so every open path closes the handle exactly once.
class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h) : h_(h) {}
    ~ScopedHandle() { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    HANDLE get() const { return h_; }

private:
    HANDLE h_;
};

// Strip a leading directory component and return the bare filename.
std::wstring BaseName(const std::wstring& path) {
    size_t p = path.find_last_of(L"/\\");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

// If `name` ends (case-insensitively) with one of the `exts`, return the
// name with that extension removed; otherwise return `name` unchanged.
std::wstring StripExt(const std::wstring& name, const wchar_t* const* exts, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const wchar_t* e = exts[i];
        size_t el = std::wcslen(e);
        if (name.size() > el && _wcsicmp(name.c_str() + (name.size() - el), e) == 0)
            return name.substr(0, name.size() - el);
    }
    return name;
}

// Derive the stored member name for a stream format by stripping the
// compression extension from the on-disk filename. Falls back to a
// placeholder when nothing usable remains.
std::wstring DeriveMemberName(const std::wstring& path, const wchar_t* const* exts, size_t n) {
    std::wstring base = BaseName(path);
    std::wstring stripped = StripExt(base, exts, n);
    if (stripped.empty()) return std::wstring(L"(decompressed)");
    return stripped;
}

// Fill the aggregate fields for a single-member archive and append its entry.
void FinishSingle(ArchiveInfo& info, const wchar_t* format, const wchar_t* algorithm,
                  uint64_t archiveSize, uint64_t packed, uint64_t unpacked, uint32_t rawMethod,
                  const std::wstring& memberName, const FILETIME& mtime) {
    info.format = format;
    info.algorithm = algorithm;
    info.archiveSize = archiveSize;
    info.totalPacked = packed;
    info.totalUnpacked = unpacked;
    info.fileCount = 1;
    info.dirCount = 0;
    info.perEntryCompression = false;

    ArchiveEntry ent;
    ent.path = memberName;
    ent.isDir = false;
    ent.packedSize = packed;
    ent.unpackedSize = unpacked;
    ent.method = rawMethod;
    ent.methodName = algorithm;
    ent.mtime = mtime;
    info.entries.push_back(std::move(ent));
}

} // namespace

// ---- BZip2 / TBz2 -----------------------------------------------------------
//
// Header (10 bytes): 'B','Z','h', level digit '1'..'9', 6 reserved bytes,
// 4-byte block CRC. The unpacked size is not stored in the header (bzip2
// terminates the stream with a 6-byte magic and a combined CRC only), so the
// uncompressed size is unknown and left at 0. Packed = full file size.
bool TryParseBZip2(const std::wstring& path, ArchiveInfo& info) {
    ScopedHandle sh(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (sh.get() == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    internal::FileReader r(sh.get());

    uint8_t head[10] = {};
    if (!r.read(head, sizeof(head))) {
        info.error = L"Truncated BZip2 header";
        return false;
    }
    if (head[0] != 'B' || head[1] != 'Z' || head[2] != 'h') {
        info.error = L"Not a BZip2 file";
        return false;
    }
    uint32_t level = head[3]; // raw method id: ASCII '1'..'9'

    static const wchar_t* const kBzExts[] = { L".bz2", L".tbz2", L".tbz" };
    std::wstring member = DeriveMemberName(path, kBzExts, 3);

    FinishSingle(info, L"BZip2", L"BZip2", r.size(), r.size(), 0, level, member, FILETIME{});
    return true;
}

// ---- Z / compress -----------------------------------------------------------
//
// Header (3 bytes): ID1=0x1F, ID2=0x9D, ID3=mode/flags byte (high bit = block
// mode, low 7 bits = max code bits). One logical member. The unpacked size is
// not stored reliably, so it is reported as 0. Packed = full file size.
bool TryParseZ(const std::wstring& path, ArchiveInfo& info) {
    ScopedHandle sh(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (sh.get() == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    internal::FileReader r(sh.get());

    uint8_t head[3] = {};
    if (!r.read(head, sizeof(head))) {
        info.error = L"Truncated Z header";
        return false;
    }
    if (head[0] != 0x1F || head[1] != 0x9D) {
        info.error = L"Not a Z (compress) file";
        return false;
    }
    uint32_t flags = head[2]; // mode byte

    static const wchar_t* const kZExts[] = { L".z" };
    std::wstring member = DeriveMemberName(path, kZExts, 1);

    FinishSingle(info, L"Z", L"LZW (compress)", r.size(), r.size(), 0, flags, member, FILETIME{});
    return true;
}

// ---- PPM --------------------------------------------------------------------
//
// Header per TPpm.h: signature 'P','P','M','D','_','A', METHOD (1), MEMORY_SIZE
// (1), ORIG_SIZE (4), FTIME (4), NAME_SIZE (4), then NAME_SIZE bytes of the
// stored member name. unpackedSize comes from ORIG_SIZE. Packed = file size.
bool TryParsePPM(const std::wstring& path, ArchiveInfo& info) {
    ScopedHandle sh(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (sh.get() == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    internal::FileReader r(sh.get());

    // Fixed portion up to and including NAME_SIZE.
    uint8_t head[20] = {};
    if (!r.read(head, sizeof(head))) {
        info.error = L"Truncated PPM header";
        return false;
    }
    if (head[0] != 'P' || head[1] != 'P' || head[2] != 'M' ||
        head[3] != 'D' || head[4] != '_' || head[5] != 'A') {
        info.error = L"Not a PPM file";
        return false;
    }

    uint32_t method = head[6];
    // ORIG_SIZE is a little-endian DWORD at offset 8.
    uint64_t unpacked = internal::rd32(&head[8]);
    // FTIME is a 4-byte timestamp at offset 12 (see note below).
    uint32_t ftime = internal::rd32(&head[12]);
    uint32_t nameSize = internal::rd32(&head[16]);

    // Guard against a nonsensical name length.
    if (nameSize > 0x1000u || (uint64_t)nameSize > r.size()) {
        info.error = L"Corrupt PPM member name";
        return false;
    }

    std::wstring member;
    if (nameSize > 0) {
        std::vector<uint8_t> nameBuf(nameSize);
        if (!r.read(nameBuf.data(), nameSize)) {
            info.error = L"Truncated PPM member name";
            return false;
        }
        member = internal::decodeBytes(nameBuf.data(), nameBuf.size(), true);
    }
    if (member.empty()) member = L"(decompressed)";

    // FTIME is interpreted as a Unix time_t. When it is 0 the mtime is left
    // zeroed rather than reporting 1601.
    FILETIME mt{};
    if (ftime != 0) mt = internal::unixTimeToFT(ftime);

    FinishSingle(info, L"PPM", L"PPM", r.size(), r.size(), unpacked, method, member, mt);
    return true;
}

} // namespace archdiview
