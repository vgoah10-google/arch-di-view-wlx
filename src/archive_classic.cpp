// archive_classic.cpp — Native parsers for the classic per-entry archives
// LHA/LZH, ARC/PAK, ARJ and SQZ (no external DLL).
//
// Each format stores a walkable directory of per-file headers, so every parser
// loops over the headers, decodes the packed/unpacked sizes, timestamp and
// compression method, records an entry and then skips the entry's packed data
// to reach the next header. Only directory metadata is read; nothing is
// extracted. The header layouts and walking logic follow ArchView's
// TLhaLzh.cpp, TArcPak.cpp, TArj.cpp and TSqz.cpp, with the multi-byte fields
// that ArchView decomposes into separate BYTE members recombined here using
// the little-endian rd16/rd32 helpers.
#include "archive.h"
#include "archive_internal.h"

#include <vector>
#include <cstring>
#include <algorithm>

namespace archdiview {
namespace {

using internal::FileReader;
using internal::rd16;
using internal::rd32;

// ---- Shared helpers --------------------------------------------------------

// Decode a length-prefixed raw name, trying UTF-8 first and falling back to
// the system ANSI code page (classic archives predate Unicode).
std::wstring DecodeNameBytes(const uint8_t* d, size_t len) {
    int wl = MultiByteToWideChar(CP_UTF8, 0, (const char*)d, (int)len, nullptr, 0);
    if (wl > 0) {
        std::wstring w(wl, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, (const char*)d, (int)len, &w[0], wl);
        return w;
    }
    wl = MultiByteToWideChar(CP_ACP, 0, (const char*)d, (int)len, nullptr, 0);
    std::wstring w(std::max(wl, 0), L'\0');
    if (wl > 0) MultiByteToWideChar(CP_ACP, 0, (const char*)d, (int)len, &w[0], wl);
    return w;
}

// Record a completed entry, splitting file/folder counts and totals.
void AddEntry(ArchiveInfo& info, ArchiveEntry& ent) {
    if (ent.isDir) {
        info.dirCount++;
        ent.packedSize = 0;
        ent.unpackedSize = 0;
    } else {
        info.fileCount++;
        info.totalPacked += ent.packedSize;
        info.totalUnpacked += ent.unpackedSize;
    }
    info.entries.push_back(std::move(ent));
}

// Open `path` for reading and build a FileReader over it. Returns false (and
// sets info.error) when the file cannot be opened.
bool OpenReader(const std::wstring& path, HANDLE& h, FileReader*& fr,
                ArchiveInfo& info) {
    h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    fr = new FileReader(h);
    return true;
}

// Walk the extended headers shared by LHA (level 1/2) and SQZ. Each extended
// header is a 2-byte little-endian size (including these 2 bytes), then a
// 1-byte type; type 0x01 carries the stored name/path as a length-prefixed
// string. Returns false when the walk would run past `limit`. `size` is the
// total extended-header region length (0 for none).
bool WalkExtName(const uint8_t* base, size_t off, size_t size, size_t limit,
                 std::wstring& name) {
    if (size == 0) return true;
    size_t pos = off;
    size_t end = off + size;
    if (end > limit) return false;
    while (pos + 3 <= end) {
        uint16_t esz = rd16(base + pos);
        if (esz < 3 || pos + esz > end) return false;
        uint8_t type = base[pos + 2];
        if (type == 0x01 && pos + 4 <= end) {
            uint8_t nlen = base[pos + 3];
            if (nlen && pos + 4 + nlen <= end)
                name = DecodeNameBytes(base + pos + 4, nlen);
        }
        pos += esz;
    }
    return true;
}

// ---- LHA / LZH -------------------------------------------------------------

// Fixed portion of an LHA/LZH file header (see TLhaLzh.h). The 5-byte method
// follows the HEAD_SIZE + HEAD_CHECKSUM bytes; a level-0 header stores the
// name inline after NAME_SIZE, while level 1/2 append extended headers.
#pragma pack(push, 1)
struct LhaFixed {
    uint8_t  headSize;
    uint8_t  checksum;
    char     method[5];
    uint8_t  packSize[4];
    uint8_t  unpSize[4];
    uint8_t  ftime[4];
    uint8_t  attr;
    uint8_t  headLevel;
    uint8_t  nameSize;
};
#pragma pack(pop)
static_assert(sizeof(LhaFixed) == 22, "unexpected LhaFixed size");

const wchar_t* LhaMethodName(char c3, char c4) {
    if (c3 == 'h') {
        switch (c4) {
            case '0': return L"-lh0- Stored";
            case '1': return L"-lh1- Superfast";
            case '2': return L"-lh2- Fastest";
            case '3': return L"-lh3- Fast";
            case '4': return L"-lh4- Normal";
            case '5': return L"-lh5- Good";
            case '6': return L"-lh6- Very good";
            case '7': return L"-lh7- Best";
            case 'd': return L"-lhd- Directory";
        }
    } else if (c3 == 'z') {
        switch (c4) {
            case 's': return L"-lzs- Normal";
            case '4': return L"-lz4- Stored";
            case '5': return L"-lz5- Good";
        }
    } else if (c3 == 'p') {
        return L"-pm?- Stored";
    }
    return L"Unknown";
}

// Locate the first LHA/LZH file header, tolerating a leading SFX (EXE) stub.
// A header starts with "-l", has "-" at +4 and a level byte (0..2) at +21.
// Returns true and sets `off` when found.
bool FindLhaStart(FileReader& fr, uint64_t& off) {
    const uint64_t maxScan = std::min<uint64_t>(0x60000, fr.size());
    if (maxScan < 22) return false;
    std::vector<uint8_t> buf((size_t)maxScan);
    if (!fr.readAt(0, buf.data(), buf.size())) return false;
    for (uint64_t i = 0; i + 22 <= maxScan; ++i) {
        if (buf[(size_t)i] == (uint8_t)'-' && buf[(size_t)i + 1] == (uint8_t)'l' &&
            buf[(size_t)i + 4] == (uint8_t)'-' && buf[(size_t)i + 21] <= 2) {
            off = i;
            return true;
        }
    }
    return false;
}

bool ParseLha(FileReader& fr, ArchiveInfo& info, std::wstring& error) {
    info.format = L"LHA";
    info.algorithm = L"LZH";
    info.perEntryCompression = true;

    // Tolerate an SFX stub by scanning for the first valid header offset.
    uint64_t start = 0;
    if (!FindLhaStart(fr, start)) {
        error = L"Not an LHA/LZH archive";
        return false;
    }
    fr.seek(start);

    // Map each distinct method id to its label so we can pick the dominant one.
    std::vector<std::pair<uint32_t, const wchar_t*>> methods;

    while (fr.pos() + sizeof(LhaFixed) <= fr.size()) {
        LhaFixed fx{};
        if (!fr.read(&fx, sizeof(fx))) break;

        // A zero HEAD_SIZE byte terminates the archive.
        if (fx.headSize == 0) break;

        // The method must look like "-l??-" / "-pm?-" (see AnalyzeFileHead).
        if (fx.method[0] != '-' || fx.method[1] != 'l' || fx.method[4] != '-') {
            if (!(fx.method[0] == '-' && fx.method[1] == 'p' && fx.method[4] == '-'))
                break; // implausible header -> stop walking, keep what we have
        }

        uint32_t pack = rd32(fx.packSize);
        uint32_t unp  = rd32(fx.unpSize);
        uint32_t tim  = rd32(fx.ftime);
        uint8_t  level = fx.headLevel;

        bool isDir = (fx.method[2] == 'h' && fx.method[3] == 'd');

        // HEAD_SIZE excludes HEAD_SIZE + HEAD_CHECKSUM for level 0/1 but
        // includes them for level 2 (per the reference offset math).
        uint32_t headTotal = (level == 2) ? fx.headSize : (uint32_t)fx.headSize + 2;
        if (headTotal < sizeof(LhaFixed) || fr.pos() + (headTotal - sizeof(LhaFixed)) > fr.size())
            break;

        std::wstring rel;
        if (level == 0) {
            // Level 0 stores the name inline; its length is the rest of the header.
            size_t namelen = headTotal - sizeof(LhaFixed);
            std::vector<uint8_t> nb(namelen);
            if (!fr.read(nb.data(), namelen)) break;
            rel = DecodeNameBytes(nb.data(), namelen);
        } else {
            // Level 1/2: read the remaining extended-header region and extract
            // the stored path from a type 0x01 extended header.
            size_t extLen = headTotal - sizeof(LhaFixed);
            std::vector<uint8_t> ext(extLen);
            if (!fr.read(ext.data(), extLen)) break;
            WalkExtName(ext.data(), 0, extLen, extLen, rel);
        }

        ArchiveEntry ent;
        ent.isDir = isDir;
        ent.packedSize = pack;
        ent.unpackedSize = unp;
        ent.method = (uint8_t)fx.method[3];
        ent.methodName = LhaMethodName(fx.method[2], fx.method[3]);
        ent.mtime = internal::dosTimeToFT((uint16_t)(tim & 0xFFFF), (uint16_t)(tim >> 16));
        if (!rel.empty() && rel.back() == L'/') rel.pop_back();
        ent.path = rel;
        AddEntry(info, ent);
        if (!isDir)
            methods.push_back({ ent.method, ent.methodName.c_str() });

        // Skip the entry's packed data to reach the next header.
        if (!fr.skip((int64_t)pack)) break;
    }

    if (!methods.empty()) {
        // Dominant method: most frequent, tie-broken by highest dictionary.
        const wchar_t* best = nullptr; uint32_t bestCount = 0;
        for (size_t i = 0; i < methods.size(); ++i) {
            uint32_t c = 1;
            for (size_t j = 0; j < methods.size(); ++j)
                if (methods[j].first == methods[i].first) ++c;
            if (c > bestCount) { bestCount = c; best = methods[i].second; }
        }
        if (best) info.algorithm = best;
    }
    info.archiveSize = fr.size();
    return true;
}

// ---- ARC / PAK -------------------------------------------------------------

#pragma pack(push, 1)
struct ArcFileHead {          // per-file header (see TArcPak.h)
    uint8_t  name[13];
    uint8_t  packSize[4];
    uint8_t  ftime[4];
    uint8_t  crc[2];
    uint8_t  origSize[4];
};
#pragma pack(pop)
static_assert(sizeof(ArcFileHead) == 27, "unexpected ArcFileHead size");

const wchar_t* ArcMethodName(uint8_t m) {
    switch (m) {
        case 0x01: return L"Unpacked";
        case 0x02: return L"Unpacked";
        case 0x03: return L"Packed (RLE)";
        case 0x04: return L"Squeezed";
        case 0x05: return L"Crunched";
        case 0x06: return L"Crunched";
        case 0x07: return L"Crunched";
        case 0x08: return L"Crunched";
        case 0x09: return L"Squashed";
        case 0x0A: return L"Crushed";
        case 0x0B: return L"Distilled";
        default:   return L"Unknown";
    }
}

bool ParseArcPak(FileReader& fr, ArchiveInfo& info, std::wstring& error) {
    info.algorithm = L"ARC";
    info.perEntryCompression = true;

    // The archive begins with a block header: ID byte (must be 0x1A) + METHOD.
    uint8_t id = 0, method = 0;
    if (!fr.read(&id, 1) || !fr.read(&method, 1) || id != 0x1A) {
        error = L"Not an ARC/PAK archive";
        return false;
    }

    bool isPak = false;
    std::vector<std::pair<uint8_t, uint32_t>> methods;

    while (fr.pos() < fr.size()) {
        if (!fr.read(&id, 1) || !fr.read(&method, 1)) break;

        if (method == 0x00) {
            // End of archive. Peek the next block header to tell PAK (0xFE)
            // from ARC-with-comment (see AnalyzeInfoOfArc).
            uint8_t nid = 0, nm = 0;
            uint64_t mark = fr.pos();
            if (fr.read(&nid, 1) && fr.read(&nm, 1) && nid == 0xFE)
                isPak = true;
            (void)nm;
            fr.seek(mark);
            break;
        }
        if (method == 0x1E) {
            // ARC "open directory" block: no packed data follows the header.
            ArcFileHead fh{};
            if (!fr.read(&fh, sizeof(fh))) break;
            std::wstring name = internal::decodeAscii((const char*)fh.name, 13);
            if (!name.empty() && name.back() == L'/') name.pop_back();
            ArchiveEntry ent;
            ent.isDir = true;
            ent.method = method;
            ent.methodName = L"Directory";
            ent.path = name;
            AddEntry(info, ent);
            continue;
        }
        if (method == 0x1F) {
            // ARC "end of directory" marker: header only, nothing to skip.
            continue;
        }

        ArcFileHead fh{};
        if (!fr.read(&fh, sizeof(fh))) break;
        uint32_t pack = rd32(fh.packSize);
        uint32_t unp  = rd32(fh.origSize);
        uint32_t tim  = rd32(fh.ftime);

        ArchiveEntry ent;
        ent.isDir = false;
        ent.packedSize = pack;
        ent.unpackedSize = unp;
        ent.method = method;
        ent.methodName = ArcMethodName(method);
        ent.mtime = internal::dosTimeToFT((uint16_t)(tim & 0xFFFF), (uint16_t)(tim >> 16));
        ent.path = internal::decodeAscii((const char*)fh.name, 13);
        AddEntry(info, ent);
        methods.push_back({ method, 1 });

        if (!fr.skip((int64_t)pack)) break;
    }

    // Pick the dominant (most frequent) file method.
    uint8_t best = 0; uint32_t bestCount = 0;
    for (size_t i = 0; i < methods.size(); ++i) {
        uint32_t c = 1;
        for (size_t j = 0; j < methods.size(); ++j)
            if (methods[j].first == methods[i].first) ++c;
        if (c > bestCount) { bestCount = c; best = methods[i].first; }
    }
    if (bestCount > 0) {
        info.algorithm = ArcMethodName(best);
    } else {
        info.algorithm = isPak ? L"PAK" : L"ARC";
    }
    info.format = isPak ? L"PAK" : L"ARC";
    info.archiveSize = fr.size();
    return true;
}

// ---- ARJ -------------------------------------------------------------------

#pragma pack(push, 1)
struct ArjBlockHead {         // block header body (see TArj.h), after the
                              // HEADER_ID(2) + HEAD_SIZE(2) bytes we read first
    uint8_t  hdrSize;
    uint8_t  verCreated;
    uint8_t  verExtract;
    uint8_t  hostOs;
    uint8_t  flags;
    uint8_t  method;
    uint8_t  fileType;
    uint8_t  reserved;
    uint8_t  dateTime[4];
    uint8_t  packSize[4];
    uint8_t  origSize[4];
    uint8_t  crc[4];
    uint8_t  filespecPos[2];
    uint8_t  access[2];
    uint8_t  firstChapter;
    uint8_t  lastChapter;
};
#pragma pack(pop)
static_assert(sizeof(ArjBlockHead) == 30, "unexpected ArjBlockHead size");

const wchar_t* ArjMethodName(uint8_t m) {
    switch (m) {
        case 0: return L"Stored";
        case 1: return L"Good";
        case 2: return L"Normal";
        case 3: return L"Fast";
        case 4: return L"Fastest";
        default: return L"Unknown";
    }
}

// Read an ARJ block: HEADER_ID + HEAD_SIZE have already been consumed. Reads
// the fixed body, then the name and comment strings, then the block-terminating
// extended-header size word. Returns false when the block is malformed.
bool ReadArjBlock(FileReader& fr, ArjBlockHead& bh, std::wstring& name) {
    if (!fr.read(&bh, sizeof(bh))) return false;

    // hdrSize is the size of the basic header excluding HEADER_ID + HEAD_SIZE
    // (the reference seeks hdrSize + 4 - sizeof(head)). Compute where the name
    // string begins; guard against an implausible size.
    const uint32_t consumed = 4 + sizeof(ArjBlockHead); // ID + SIZE + body
    if (bh.hdrSize + 4 < consumed || fr.pos() + bh.hdrSize + 4 - consumed > fr.size())
        return false;
    fr.skip((int64_t)bh.hdrSize + 4 - (int64_t)consumed);

    // The archive/file name is a NUL-terminated string.
    {
        std::vector<uint8_t> nb;
        nb.resize(4096);
        size_t cap = std::min<size_t>(nb.size(), (size_t)(fr.size() - fr.pos()));
        if (!fr.read(nb.data(), cap)) return false;
        size_t n = 0;
        while (n < cap && nb[n] != 0) ++n;
        name = DecodeNameBytes(nb.data(), n);
        fr.skip((int64_t)n + 1 - (int64_t)cap); // rewind the unread tail
    }
    // The file comment is another NUL-terminated string (up to ~2000 bytes).
    {
        std::vector<uint8_t> cb;
        cb.resize(2001);
        size_t cap = std::min<size_t>(cb.size(), (size_t)(fr.size() - fr.pos()));
        if (!fr.read(cb.data(), cap)) return false;
        size_t n = 0;
        while (n < cap && cb[n] != 0) ++n;
        fr.skip((int64_t)n + 1 - (int64_t)cap);
    }
    // Every block ends with a 2-byte extended-header size word.
    uint8_t ext[2];
    if (!fr.read(ext, 2)) return false;
    uint16_t extSize = rd16(ext);
    if (extSize > 0) {
        if (fr.pos() + extSize > fr.size()) return false;
        fr.skip(extSize);
    }
    return true;
}

bool ParseArj(FileReader& fr, ArchiveInfo& info, std::wstring& error) {
    info.format = L"ARJ";
    info.algorithm = L"ARJ";
    info.perEntryCompression = true;

    // Main header: HEADER_ID (0x60 0xEA -> rd16 == 0xEA60) then HEAD_SIZE.
    uint8_t sig[2];
    if (!fr.read(sig, 2) || rd16(sig) != 0xEA60) {
        error = L"Not an ARJ archive";
        return false;
    }
    uint8_t hs[2];
    if (!fr.read(hs, 2)) { error = L"Truncated ARJ header"; return false; }
    uint16_t headSize = rd16(hs);
    if (headSize == 0 || headSize > 2600) { error = L"Bad ARJ header size"; return false; }

    std::vector<std::pair<uint8_t, uint32_t>> methods;

    while (true) {
        // Read the next block's HEADER_ID + HEAD_SIZE.
        uint8_t id[2], sz[2];
        if (!fr.read(id, 2) || !fr.read(sz, 2)) break;
        // A zero HEAD_SIZE marks the end-of-archive block.
        if (rd16(sz) == 0x0000) break;

        ArjBlockHead bh{};
        std::wstring name;
        if (!ReadArjBlock(fr, bh, name)) break;

        uint32_t pack = rd32(bh.packSize);
        uint32_t tim  = rd32(bh.dateTime);
        uint8_t  ftype = bh.fileType;

        // FILE_TYPE 0x03 is a directory; 0x00/0x01 a file; 0x04 a volume label.
        if (ftype == 0x00 || ftype == 0x01 || ftype == 0x03 || ftype == 0x04) {
            ArchiveEntry ent;
            ent.isDir = (ftype == 0x03);
            ent.packedSize = pack;
            ent.unpackedSize = rd32(bh.origSize);
            ent.method = bh.method;
            ent.methodName = ArjMethodName(bh.method);
            ent.mtime = internal::dosTimeToFT((uint16_t)(tim & 0xFFFF), (uint16_t)(tim >> 16));
            if (!name.empty() && name.back() == L'/') name.pop_back();
            ent.path = name;
            AddEntry(info, ent);
            if (!ent.isDir) methods.push_back({ bh.method, 1 });
        }

        // Skip the entry's packed data to reach the next header.
        if (!fr.skip((int64_t)pack)) break;
    }

    uint8_t best = 0; uint32_t bestCount = 0;
    for (size_t i = 0; i < methods.size(); ++i) {
        uint32_t c = 1;
        for (size_t j = 0; j < methods.size(); ++j)
            if (methods[j].first == methods[i].first) ++c;
        if (c > bestCount) { bestCount = c; best = methods[i].first; }
    }
    if (bestCount > 0) info.algorithm = ArjMethodName(best);
    info.archiveSize = fr.size();
    return true;
}

// ---- SQZ -------------------------------------------------------------------

#pragma pack(push, 1)
struct SqzArcHead {           // 8-byte archive header (see TSqz.h)
    char    id[5];            // "HLSQZ"
    uint8_t unpVer;
    uint8_t hostOs;
    uint8_t flags;
};
#pragma pack(pop)
static_assert(sizeof(SqzArcHead) == 8, "unexpected SqzArcHead size");

#pragma pack(push, 1)
struct SqzFileHead {          // fixed portion of a file block (see TSqz.h)
    uint8_t  headSum;
    uint8_t  method;
    uint8_t  packSize[4];
    uint8_t  origSize[4];
    uint8_t  ftime[4];
    uint8_t  attr;
    uint8_t  crc[4];
};
#pragma pack(pop)
static_assert(sizeof(SqzFileHead) == 19, "unexpected SqzFileHead size");

const wchar_t* SqzMethodName(uint8_t m) {
    switch (m & 0x0F) {
        case 0: return L"Stored";
        case 1: return L"Fast";
        case 2: return L"Normal";
        case 3: return L"Good";
        case 4: return L"Best";
        default: return L"Unknown";
    }
}

bool ParseSqz(FileReader& fr, ArchiveInfo& info, std::wstring& error) {
    info.format = L"SQZ";
    info.algorithm = L"Sqz";
    info.perEntryCompression = true;

    SqzArcHead ah{};
    if (!fr.read(&ah, sizeof(ah)) ||
        ah.id[0] != 'H' || ah.id[1] != 'L' || ah.id[2] != 'S' ||
        ah.id[3] != 'Q' || ah.id[4] != 'Z') {
        error = L"Not a SQZ archive";
        return false;
    }
    bool dosTime = (ah.flags & 0x02) != 0;

    std::vector<std::pair<uint8_t, uint32_t>> methods;

    while (fr.pos() < fr.size()) {
        uint8_t id = 0;
        if (!fr.read(&id, 1)) break;

        if (id == 0x00) break; // END_OF_ARC

        if (id > 18) {
            // File block: the ID byte is the block's size field (HEAD_SIZE).
            SqzFileHead fh{};
            if (!fr.read(&fh, sizeof(fh))) break;
            uint32_t pack = rd32(fh.packSize);
            uint32_t unp  = rd32(fh.origSize);
            uint32_t tim  = rd32(fh.ftime);

            // The name lives in the extended headers at the tail of the block.
            size_t blockRemain = (size_t)id - sizeof(SqzFileHead);
            std::vector<uint8_t> ext(blockRemain);
            std::wstring name;
            if (blockRemain) {
                if (!fr.read(ext.data(), blockRemain)) break;
                WalkExtName(ext.data(), 0, blockRemain, blockRemain, name);
            }

            ArchiveEntry ent;
            bool isDir = false;
            // A stored name ending in '/' (or the directory attribute) is a folder.
            if (!name.empty() && name.back() == L'/') { isDir = true; name.pop_back(); }
            else if (fh.attr & 0x10) isDir = true;
            ent.isDir = isDir;
            ent.packedSize = pack;
            ent.unpackedSize = unp;
            ent.method = fh.method & 0x0F;
            ent.methodName = SqzMethodName(fh.method);
            ent.mtime = dosTime
                ? internal::dosTimeToFT((uint16_t)(tim & 0xFFFF), (uint16_t)(tim >> 16))
                : internal::unixTimeToFT(tim);
            ent.path = name;
            AddEntry(info, ent);
            if (!isDir) methods.push_back({ (uint8_t)(fh.method & 0x0F), 1 });

            // Skip the entry's packed data to reach the next block.
            if (!fr.skip((int64_t)pack)) break;
        } else {
            // Control/informational block: a 2-byte size follows, then payload.
            uint8_t sz[2];
            if (!fr.read(sz, 2)) break;
            uint16_t size = rd16(sz);
            if (id == 0x06) {
                // DIRECTORY block: payload holds the directory name.
                std::vector<uint8_t> p(size);
                std::wstring name;
                if (size) {
                    if (!fr.read(p.data(), size)) break;
                    size_t n = 0;
                    while (n < size && p[n] != 0) ++n;
                    name = DecodeNameBytes(p.data(), n);
                }
                if (!name.empty() && name.back() == L'/') name.pop_back();
                ArchiveEntry ent;
                ent.isDir = true;
                ent.method = 0;
                ent.methodName = L"Directory";
                ent.path = name;
                AddEntry(info, ent);
            } else {
                if (size && (fr.pos() + size > fr.size())) break;
                if (size) fr.skip(size);
            }
        }
    }

    uint8_t best = 0; uint32_t bestCount = 0;
    for (size_t i = 0; i < methods.size(); ++i) {
        uint32_t c = 1;
        for (size_t j = 0; j < methods.size(); ++j)
            if (methods[j].first == methods[i].first) ++c;
        if (c > bestCount) { bestCount = c; best = methods[i].first; }
    }
    if (bestCount > 0) info.algorithm = SqzMethodName(best);
    info.archiveSize = fr.size();
    return true;
}

} // namespace

// ---- Public entry points ---------------------------------------------------

bool TryParseLha(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = INVALID_HANDLE_VALUE; FileReader* fr = nullptr;
    if (!OpenReader(path, h, fr, info)) return false;
    std::wstring error;
    bool ok = ParseLha(*fr, info, error);
    delete fr;
    CloseHandle(h);
    if (!ok) info.error = error;
    return ok;
}

bool TryParseArcPak(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = INVALID_HANDLE_VALUE; FileReader* fr = nullptr;
    if (!OpenReader(path, h, fr, info)) return false;
    std::wstring error;
    bool ok = ParseArcPak(*fr, info, error);
    delete fr;
    CloseHandle(h);
    if (!ok) info.error = error;
    return ok;
}

bool TryParseArj(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = INVALID_HANDLE_VALUE; FileReader* fr = nullptr;
    if (!OpenReader(path, h, fr, info)) return false;
    std::wstring error;
    bool ok = ParseArj(*fr, info, error);
    delete fr;
    CloseHandle(h);
    if (!ok) info.error = error;
    return ok;
}

bool TryParseSqz(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = INVALID_HANDLE_VALUE; FileReader* fr = nullptr;
    if (!OpenReader(path, h, fr, info)) return false;
    std::wstring error;
    bool ok = ParseSqz(*fr, info, error);
    delete fr;
    CloseHandle(h);
    if (!ok) info.error = error;
    return ok;
}

} // namespace archdiview
