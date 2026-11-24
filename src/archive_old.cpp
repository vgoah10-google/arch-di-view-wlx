// archive_old.cpp — Native parsers for four classic archive formats.
//
// ZOO, HA, BH (Black Hole) and DWC. Each parser reads only the archive's
// directory metadata (no extraction) and fills an ArchiveInfo with per-entry
// sizes and the compression method, following the proven directory-walking
// logic of ArchView's TZoo / THa / TBh / TDwc classes.
//
// Layout notes (all multi-byte fields are little-endian):
//  - ZOO: a 20-byte text header, an archive header whose ID is 0xFDC4A7DC and
//    whose first field is the offset of the first directory entry, then a
//    singly-linked list of 52-byte directory entries chained by a "next entry
//    offset" field (0 terminates the list). Each entry's name lives at the
//    entry's data offset; an optional long name / comment block follows the
//    packed data (pointed to by the comment offset).
//  - HA: a 4-byte archive header (ID 0x4841 + file count), then per-file
//    headers: a fixed part, a NUL-terminated dir path, a NUL-terminated name,
//    a 3-byte addendum (name len, host os, attr), optional salt / ext-time,
//    then the packed data.
//  - BH: a stream of 32-byte (minimum) file headers whose ID is 0x07054842
//    ("BH\x05\x07"), each carrying a head size, name size and comment size,
//    followed by the name, the comment and the packed data.
//  - DWC: the directory is stored at the END of the file. A 27-byte end record
//    (ID "DWC") gives the entry count; fixed 34-byte file entries precede it.
#include "archive.h"
#include "archive_internal.h"

#include <cstring>
#include <algorithm>

namespace archdiview {
namespace {

using internal::FileReader;
using internal::rd16;
using internal::rd32;
using internal::decodeAscii;
using internal::decodeBytes;
using internal::dosTimeToFT;

// Safety cap so a corrupt "next offset" chain can never loop forever.
constexpr uint32_t kMaxEntries = 100000;
// Safety cap for a single variable-length name/comment field.
constexpr uint32_t kMaxField = 65536;

// RAII wrapper so the file handle is closed on every exit path.
struct FileHandle {
    HANDLE h;
    explicit FileHandle(HANDLE handle) : h(handle) {}
    ~FileHandle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

// Open `path` for reading and report its size. Returns false (and sets
// info.error) when the file cannot be opened.
bool OpenArchive(const std::wstring& path, HANDLE& h, uint64_t& size,
                 ArchiveInfo& info) {
    h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        info.error = L"Cannot get file size";
        return false;
    }
    size = (uint64_t)sz.QuadPart;
    return true;
}

// Read a NUL-terminated string of up to `maxLen` bytes at the current
// position. Returns the number of bytes consumed (string + terminator).
// Returns 0 on failure (never reads past EOF).
size_t ReadCString(FileReader& r, uint64_t fileSize, std::wstring& out) {
    uint64_t remaining = fileSize - r.pos();
    size_t cap = (size_t)std::min<uint64_t>(remaining, kMaxField);
    if (cap == 0) return 0;
    std::vector<char> buf(cap);
    if (!r.read(buf.data(), cap)) return 0;
    size_t len = strnlen(buf.data(), cap);
    // A missing terminator would mean we consumed the whole window; reject.
    if (len == cap) return 0;
    out = decodeAscii(buf.data(), len);
    return len + 1;
}

// Decode a fixed N-byte name field that may not be NUL-terminated (trim at
// the first NUL, falling back to the full width).
std::wstring DecodeFixedName(const uint8_t* d, size_t len, bool utf8) {
    size_t n = 0;
    while (n < len && d[n] != '\0') ++n;
    if (n == 0) n = len; // fully-packed name with no terminator
    return decodeBytes(d, n, utf8);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (name.empty()) return dir;
    return dir + L"\\" + name;
}

void AddEntry(ArchiveInfo& info, const std::wstring& path, bool isDir,
              uint64_t packed, uint64_t unpacked, uint32_t method,
              const wchar_t* methodName, const FILETIME& mtime) {
    ArchiveEntry ent;
    ent.path = path;
    ent.isDir = isDir;
    ent.packedSize = packed;
    ent.unpackedSize = unpacked;
    ent.method = method;
    ent.methodName = methodName;
    ent.mtime = mtime;
    if (isDir) {
        info.dirCount++;
    } else {
        info.fileCount++;
        info.totalPacked += packed;
        info.totalUnpacked += unpacked;
    }
    info.entries.push_back(std::move(ent));
}

// --------------------------------------------------------------------------
// ZOO
// --------------------------------------------------------------------------

constexpr uint32_t kZooId = 0xFDC4A7DC;
constexpr size_t kZooArcHeadSize = 42;  // 20 text + 8 id + 4 + 4 + 2 + 4 + 2
constexpr size_t kZooFileHeadSize = 52;

// ZOO method -> label. 0x00 stored, 0x01 crunched (normal), 0x02 crunched
// (good / maximum). Higher codes are newer/lzhd-style variants.
const wchar_t* ZooMethodName(uint8_t m) {
    switch (m) {
        case 0x00: return L"Stored";
        case 0x01: return L"Crunched (Normal)";
        case 0x02: return L"Crunched (Good)";
        default:   return L"Unknown";
    }
}

} // namespace (internal helpers)

bool TryParseZoo(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = nullptr;
    uint64_t fileSize = 0;
    if (!OpenArchive(path, h, fileSize, info)) return false;
    FileHandle guard(h);
    if (fileSize < kZooArcHeadSize) { info.error = L"File too small"; return false; }

    FileReader r(h);
    uint8_t head[kZooArcHeadSize];
    if (!r.read(head, sizeof(head))) { info.error = L"Read error"; return false; }

    if (rd32(head + 20) != kZooId) { info.error = L"Not a ZOO archive"; return false; }

    // Offset of the first directory entry (from the archive header).
    uint64_t offset = rd32(head + 28);
    // Archive-level comment (present but not surfaced as an entry).
    // uint32_t arcCommentOffset = rd32(head + 36);
    // uint16_t arcCommentSize   = rd16(head + 40);

    uint32_t maxMethod = 0;
    uint32_t iterations = 0;

    while (offset != 0) {
        if (++iterations > kMaxEntries) { info.error = L"Too many entries"; return false; }
        if (offset < kZooArcHeadSize || offset + kZooFileHeadSize > fileSize) {
            info.error = L"Corrupt entry offset";
            return false;
        }
        uint8_t fh[kZooFileHeadSize];
        if (!r.readAt(offset, fh, sizeof(fh))) { info.error = L"Read error"; return false; }

        if (rd32(fh + 0) != kZooId) { info.error = L"Corrupt entry"; return false; }

        uint8_t type = fh[4];
        uint8_t method = fh[5];
        uint32_t nextOffset = rd32(fh + 6);   // offset of next directory entry
        uint32_t dataOffset = rd32(fh + 10);  // offset of packed data / name
        uint32_t ftime = rd32(fh + 14);
        uint32_t unpSize = rd32(fh + 18);
        uint32_t packSize = rd32(fh + 22);
        // fh[26] ver_compress, fh[27] ver_extract
        // uint16_t flags    = rd16(fh + 28);
        uint32_t commentOffset = rd32(fh + 30);
        uint16_t commentSize = rd16(fh + 34);

        bool isDir = (type == 2);
        if (!isDir && type != 1) { info.error = L"Unknown entry type"; return false; }

        // Resolve the name. Base name is a NUL-terminated string at dataOffset;
        // an extended (long) name block, when flagged, supersedes it.
        std::wstring name;
        bool longName = (fh[29] & 0x08) != 0;
        if (longName && commentOffset != 0 &&
            commentOffset + 5 <= fileSize) {
            uint8_t nb[5];
            if (r.readAt(commentOffset, nb, sizeof(nb)) && rd32(nb) == 0xFDC4A7DC) {
                size_t nlen = nb[4];
                if (nlen > 0 && commentOffset + 5 + nlen <= fileSize) {
                    std::vector<uint8_t> nbuf(nlen);
                    if (r.readAt(commentOffset + 5, nbuf.data(), nlen)) {
                        name = decodeBytes(nbuf.data(), nlen, false);
                    }
                }
            }
        }
        if (name.empty() && dataOffset != 0 && dataOffset < fileSize) {
            std::vector<uint8_t> nbuf(13);
            size_t take = (size_t)std::min<uint64_t>(13, fileSize - dataOffset);
            if (r.readAt(dataOffset, nbuf.data(), take)) {
                name = DecodeFixedName(nbuf.data(), take, false);
            }
        }

        // Skip the packed data so a following long-name/comment block could be
        // located (not needed for metadata, but keeps offsets meaningful).
        (void)commentSize;

        if (method > maxMethod) maxMethod = method;
        FILETIME mtime = dosTimeToFT((uint16_t)(ftime >> 16), (uint16_t)ftime);
        AddEntry(info, name, isDir,
                 isDir ? 0 : packSize, isDir ? 0 : unpSize,
                 method, ZooMethodName(method), mtime);

        offset = nextOffset;
    }

    info.format = L"ZOO";
    info.algorithm = ZooMethodName((uint8_t)maxMethod);
    info.archiveSize = fileSize;
    info.perEntryCompression = true;
    return true;
}

// --------------------------------------------------------------------------
// HA
// --------------------------------------------------------------------------

constexpr uint16_t kHaId = 0x4841;  // "AH" in memory (little-endian)
constexpr size_t kHaArcHeadSize = 4;
constexpr size_t kHaFileHeadSize = 22;  // fixed part before path/name/add

// HA method (low nibble) -> label.
const wchar_t* HaMethodName(uint8_t m) {
    switch (m) {
        case 0x00: return L"CPY (Stored)";
        case 0x01: return L"ASC (Normal)";
        case 0x02: return L"HSC (Good)";
        default:   return L"Unknown";
    }
}

bool TryParseHa(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = nullptr;
    uint64_t fileSize = 0;
    if (!OpenArchive(path, h, fileSize, info)) return false;
    FileHandle guard(h);
    if (fileSize < kHaArcHeadSize) { info.error = L"File too small"; return false; }

    FileReader r(h);
    uint8_t ahead[kHaArcHeadSize];
    if (!r.read(ahead, sizeof(ahead))) { info.error = L"Read error"; return false; }
    if (rd16(ahead) != kHaId) { info.error = L"Not a HA archive"; return false; }
    uint16_t numFiles = rd16(ahead + 2);
    if (numFiles > kMaxEntries) { info.error = L"Implausible file count"; return false; }

    uint8_t maxMethod = 0;
    for (uint16_t i = 0; i < numFiles; ++i) {
        if (r.pos() + kHaFileHeadSize > fileSize) { info.error = L"Unexpected end"; return false; }
        uint8_t fh[kHaFileHeadSize];
        if (!r.read(fh, sizeof(fh))) { info.error = L"Read error"; return false; }

        uint8_t method = fh[0];
        uint32_t packSize = rd32(fh + 1);
        uint32_t origSize = rd32(fh + 5);
        uint32_t ftime = rd32(fh + 13);

        std::wstring dirPath;
        size_t consumed = ReadCString(r, fileSize, dirPath);
        if (consumed == 0) { info.error = L"Corrupt path"; return false; }
        std::wstring name;
        consumed = ReadCString(r, fileSize, name);
        if (consumed == 0) { info.error = L"Corrupt name"; return false; }

        // 3-byte addendum: name len, host os, attr.
        uint8_t add[3];
        if (!r.read(add, sizeof(add))) { info.error = L"Read error"; return false; }
        (void)add[0];  // stored name length (informational)

        uint8_t low = (uint8_t)(method & 0x0F);
        bool isDir = (low == 0x0E);
        FILETIME mtime = dosTimeToFT((uint16_t)(ftime >> 16), (uint16_t)ftime);
        AddEntry(info, JoinPath(dirPath, name), isDir,
                 isDir ? 0 : packSize, isDir ? 0 : origSize,
                 low, isDir ? L"Directory" : HaMethodName(low), mtime);
        if (!isDir && low > maxMethod) maxMethod = low;

        // Skip the packed data (and any salt / extended time already counted
        // inside packSize by the reference).
        if (packSize > fileSize || r.pos() + packSize > fileSize) {
            info.error = L"Corrupt packed size";
            return false;
        }
        if (!r.skip((int64_t)packSize)) { info.error = L"Seek error"; return false; }
    }

    info.format = L"HA";
    info.algorithm = HaMethodName(maxMethod);
    info.archiveSize = fileSize;
    info.perEntryCompression = true;
    return true;
}

// --------------------------------------------------------------------------
// BH (Black Hole)
// --------------------------------------------------------------------------

constexpr uint32_t kBhId = 0x07054842;  // "BH\x05\x07"
constexpr size_t kBhFileHeadSize = 32;

// BH method -> label (stored / fast / fuse / normal / best).
const wchar_t* BhMethodName(uint8_t m) {
    switch (m) {
        case 0x00: return L"Stored";
        case 0x01: return L"Fast";
        case 0x02: return L"Fuse (Normal)";
        case 0x03: return L"Normal";
        case 0x04: return L"Best";
        default:   return L"Unknown";
    }
}

// Decode the BH compression method from the 16-bit flags field.
uint8_t BhMethodFromFlags(uint16_t flags) {
    if ((flags & 0x0800) != 0) return 0x03;                       // Normal
    if ((flags & 0x0802) == 0x0802) return 0x04;                  // Best
    if ((flags & 0x0804) == 0x0804) return 0x01;                  // Fast
    if ((flags & 0x0300) == 0x0300) return 0x02;                  // Fuse
    if ((flags & 0x0302) == 0x0302) return 0x02;
    if ((flags & 0x0304) == 0x0304) return 0x02;
    if ((flags & 0x0306) == 0x0306) return 0x02;
    return 0x00;                                                  // Stored
}

bool TryParseBh(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = nullptr;
    uint64_t fileSize = 0;
    if (!OpenArchive(path, h, fileSize, info)) return false;
    FileHandle guard(h);
    if (fileSize < kBhFileHeadSize) { info.error = L"File too small"; return false; }

    FileReader r(h);
    uint32_t iterations = 0;

    while (r.pos() + kBhFileHeadSize <= fileSize) {
        if (++iterations > kMaxEntries) { info.error = L"Too many entries"; return false; }
        uint8_t fh[kBhFileHeadSize];
        if (!r.read(fh, sizeof(fh))) break;

        if (rd32(fh + 0) != kBhId) { info.error = L"Corrupt entry"; return false; }

        uint16_t headSize = rd16(fh + 4);
        uint16_t flags = rd16(fh + 9);
        uint32_t ftime = rd32(fh + 11);
        uint32_t packSize = rd32(fh + 15);
        uint32_t origSize = rd32(fh + 19);
        uint16_t attr = rd16(fh + 27);
        uint16_t nameSize = rd16(fh + 29);
        uint16_t commentSize = rd16(fh + 31);

        if (headSize < kBhFileHeadSize || headSize > fileSize) {
            info.error = L"Corrupt header size";
            return false;
        }
        if ((uint32_t)nameSize > kMaxField || (uint32_t)commentSize > kMaxField) {
            info.error = L"Implausible name/comment size";
            return false;
        }
        // head size must cover the name + comment that follow the fixed part.
        if ((uint32_t)headSize > kBhFileHeadSize + nameSize + commentSize) {
            info.error = L"Corrupt header size";
            return false;
        }

        std::wstring name;
        if (nameSize > 0) {
            if (r.pos() + nameSize > fileSize) { info.error = L"Unexpected end"; return false; }
            std::vector<uint8_t> nbuf(nameSize);
            if (!r.read(nbuf.data(), nameSize)) { info.error = L"Read error"; return false; }
            name = decodeBytes(nbuf.data(), nameSize, false);
        }
        if (commentSize > 0) {
            if (r.pos() + commentSize > fileSize) { info.error = L"Unexpected end"; return false; }
            if (!r.skip(commentSize)) { info.error = L"Seek error"; return false; }
        }
        // Skip any remaining reserved header bytes, then the packed data.
        int64_t rest = (int64_t)headSize - (int64_t)(kBhFileHeadSize + nameSize + commentSize);
        if (rest > 0 && !r.skip(rest)) { info.error = L"Seek error"; return false; }
        if (packSize > fileSize || r.pos() + packSize > fileSize) {
            info.error = L"Corrupt packed size";
            return false;
        }
        if (!r.skip((int64_t)packSize)) { info.error = L"Seek error"; return false; }

        bool isDir = (attr & 0x0010) != 0;
        uint8_t method = BhMethodFromFlags(flags);
        FILETIME mtime = dosTimeToFT((uint16_t)(ftime >> 16), (uint16_t)ftime);
        AddEntry(info, name, isDir,
                 isDir ? 0 : packSize, isDir ? 0 : origSize,
                 method, BhMethodName(method), mtime);
    }

    if (info.entries.empty()) { info.error = L"No entries found"; return false; }

    // Dominant (highest) method across entries.
    uint8_t maxMethod = 0;
    for (const auto& e : info.entries) {
        if (!e.isDir) {
            uint8_t m = (uint8_t)std::min<uint32_t>(e.method, 0xFF);
            if (m > maxMethod) maxMethod = m;
        }
    }
    info.format = L"BH";
    info.algorithm = BhMethodName(maxMethod);
    info.archiveSize = fileSize;
    info.perEntryCompression = true;
    return true;
}

// --------------------------------------------------------------------------
// DWC
// --------------------------------------------------------------------------

constexpr size_t kDwcEndSize = 0x1B;     // 27-byte end record
constexpr size_t kDwcEntrySize = 0x22;   // 34-byte directory entry
constexpr size_t kDwcNameSize = 13;

bool TryParseDwc(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = nullptr;
    uint64_t fileSize = 0;
    if (!OpenArchive(path, h, fileSize, info)) return false;
    FileHandle guard(h);
    if (fileSize < kDwcEndSize) { info.error = L"File too small"; return false; }

    FileReader r(h);

    // Read the end record from the very end of the file.
    uint8_t end[kDwcEndSize];
    if (!r.readAt(fileSize - kDwcEndSize, end, sizeof(end))) {
        info.error = L"Read error";
        return false;
    }
    uint16_t headSize = rd16(end + 0);
    uint16_t entriesSize = rd16(end + 2);
    uint32_t entryCount = rd32(end + 20);
    // The three ID bytes are the final three bytes of the end record.
    if (end[kDwcEndSize - 3] != 'D' || end[kDwcEndSize - 2] != 'W' ||
        end[kDwcEndSize - 1] != 'C') {
        info.error = L"Not a DWC archive";
        return false;
    }
    if (headSize != kDwcEndSize || entriesSize != kDwcEntrySize) {
        info.error = L"Unexpected DWC block sizes";
        return false;
    }
    if (entryCount > kMaxEntries) { info.error = L"Implausible entry count"; return false; }

    // The directory of fixed-size entries sits immediately before the end record.
    uint64_t dirSize = (uint64_t)entryCount * kDwcEntrySize;
    if (dirSize > fileSize - kDwcEndSize) { info.error = L"Directory past EOF"; return false; }
    uint64_t dirOffset = fileSize - kDwcEndSize - dirSize;

    uint8_t minMethod = 0xFF;
    bool any = false;
    for (uint32_t i = 0; i < entryCount; ++i) {
        uint64_t eo = dirOffset + (uint64_t)i * kDwcEntrySize;
        uint8_t fe[kDwcEntrySize];
        if (!r.readAt(eo, fe, sizeof(fe))) { info.error = L"Read error"; return false; }

        std::wstring name = DecodeFixedName(fe, kDwcNameSize, false);
        uint32_t origSize = rd32(fe + 13);
        uint32_t ftime = rd32(fe + 17);
        uint32_t packSize = rd32(fe + 21);
        uint32_t dataOffset = rd32(fe + 25);
        uint8_t methodByte = fe[29];
        uint8_t commentSize = fe[30];

        uint8_t method = (uint8_t)(methodByte & 0x0F);
        bool isDir = false;  // DWC stores plain file entries; name ending in a
                             // separator is not used as a folder marker here.
        FILETIME mtime = dosTimeToFT((uint16_t)(ftime >> 16), (uint16_t)ftime);
        AddEntry(info, name, isDir, packSize, origSize,
                 method, method == 0x01 ? L"Crunched (Normal)"
                       : method == 0x02 ? L"Stored" : L"Unknown", mtime);
        if (method < minMethod) { minMethod = method; any = true; }

        (void)dataOffset;
        // Any per-entry comment bytes are skipped; they sit inside the entry's
        // reserved area which is already accounted for by the fixed size.
        (void)commentSize;
    }

    if (!any) { info.error = L"No entries found"; return false; }

    info.format = L"DWC";
    info.algorithm = (minMethod == 0x02) ? L"Stored"
                   : (minMethod == 0x01) ? L"Crunched (Normal)" : L"Unknown";
    info.archiveSize = fileSize;
    info.perEntryCompression = true;
    return true;
}

} // namespace archdiview
