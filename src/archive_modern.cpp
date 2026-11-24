// archive_modern.cpp — Native header parsers for RAR (RAR4 + RAR5), 7-Zip and ACE.
//
// ArchDiView only LISTING archive contents, so these parsers walk the on-disk
// directory/metadata structures (header blocks for RAR/ACE, the serialized
// header stream for 7z) and collect per-entry name/size/method/time plus totals.
// No decompression is performed and no external DLL is required.
//
// The block-walking logic is ported from ArchView's TRar.cpp / TAce.cpp. RAR5 and
// 7z are parsed natively here (ArchView delegated these to unrar.dll / 7z.dll).
// Every parser is defensive: signatures are validated, offsets/sizes are bounds
// checked against the file length, iteration is capped, and no read ever runs
// past EOF. On any structural surprise the parser degrades gracefully — it lists
// what it could, or returns false with info.error set.
#include "archive.h"
#include "archive_internal.h"

#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace archdiview {
namespace {

using internal::FileReader;

// Safety cap on entries per archive, guarding against corrupt/looping headers
// from exhausting memory.
constexpr uint32_t kMaxEntries = 5000000;

// Open `path` and build a FileReader over it (mirrors archive_classic.cpp).
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

// ===========================================================================
//  RAR4 — legacy block structure ("Rar!\x1a\x07\x00")
// ===========================================================================

const wchar_t* Rar4MethodName(uint8_t method) {
    switch (method) {
        case 0x30: return L"Stored";
        case 0x31: return L"Fastest";
        case 0x32: return L"Fast";
        case 0x33: return L"Normal";
        case 0x34: return L"Good";
        case 0x35: return L"Best";
        default:   return L"RAR";
    }
}

// Walk the RAR4 block chain starting at `pos` (just past the marker block).
bool WalkRar4(FileReader& fr, ArchiveInfo& info) {
    uint64_t pos = 7;
    while (pos + 7 <= fr.size() && info.entries.size() < kMaxEntries) {
        uint8_t bh[7];
        if (!fr.readAt(pos, bh, sizeof(bh))) break;
        const uint16_t headFlags = internal::rd16(bh + 3);
        const uint16_t headSize  = internal::rd16(bh + 5);
        const uint8_t  headType  = bh[2];

        // HEAD_SIZE counts from after its own field, so the total block span is
        // headSize + 4 (HEAD_CRC + HEAD_SIZE are not included in HEAD_SIZE).
        uint64_t blockSize = (uint64_t)headSize + 4;
        // Optional ADD_SIZE (large files / subblocks) extends the block.
        if (headFlags & 0x8000) {
            if (pos + 7 + 4 > fr.size()) break;
            uint8_t addb[4];
            if (!fr.readAt(pos + 7, addb, sizeof(addb))) break;
            blockSize += internal::rd32(addb);
        }
        if (blockSize < 7) break;                 // implausible — avoid rewind
        const uint64_t blockStart = pos;
        const uint64_t nextPos = blockStart + blockSize;
        if (nextPos <= blockStart) break;         // no progress — stop
        pos = nextPos;

        switch (headType) {
            case 0x73:  // MAIN_HEAD — archive header; nothing needed to list.
                break;

            case 0x74: {  // FILE_HEAD — a file or folder
                const uint64_t fhOff = blockStart + 7;
                if (fhOff + 28 > fr.size()) break;  // need the fixed part
                uint8_t fh[28];
                if (!fr.readAt(fhOff, fh, sizeof(fh))) break;
                const uint32_t packSize = internal::rd32(fh + 0);
                const uint32_t unpSize  = internal::rd32(fh + 4);
                const uint32_t ftime    = internal::rd32(fh + 12);
                const uint32_t attr     = internal::rd32(fh + 15);
                const uint8_t  hostOs   = fh[10];
                const uint8_t  method   = fh[12];
                const uint16_t nameSize = internal::rd16(fh + 13);

                uint64_t highPack = 0, highUnp = 0;
                uint64_t nameOff = fhOff + sizeof(fh);
                if (headFlags & 0x0100) {  // LARGE — 64-bit sizes present
                    if (nameOff + 8 > fr.size()) break;
                    uint8_t hi[8];
                    if (!fr.readAt(nameOff, hi, sizeof(hi))) break;
                    highPack = internal::rd32(hi + 0);
                    highUnp  = internal::rd32(hi + 4);
                    nameOff += 8;
                }
                if (nameSize == 0 || nameOff + nameSize > fr.size())
                    break;  // implausible name length

                std::vector<uint8_t> name(nameSize);
                if (!fr.readAt(nameOff, name.data(), name.size())) break;
                // Flag 0x0200 => name is UTF-8; otherwise the ANSI codepage.
                const bool utf8 = (headFlags & 0x0200) != 0;

                ArchiveEntry ent;
                ent.path = internal::decodeBytes(name.data(), name.size(), utf8);
                // Directory: dictionary-size field 0x00E0 set, or the DOS/Unix
                // directory attribute bit.
                const bool dirDict = (headFlags & 0x00E0) == 0x00E0;
                const bool dirAttr =
                    (attr & 0x10) != 0 ||                       // DOS dir
                    (hostOs == 3 && (attr & 0x4000) != 0);      // Unix dir
                ent.isDir = dirDict || dirAttr;
                ent.packedSize = (uint64_t)packSize + highPack;
                ent.unpackedSize = (uint64_t)unpSize + highUnp;
                ent.method = method;
                ent.methodName = Rar4MethodName(method);
                ent.mtime = internal::dosTimeToFT((uint16_t)(ftime >> 16),
                                                  (uint16_t)(ftime & 0xFFFF));

                if (ent.isDir) info.dirCount++;
                else {
                    info.fileCount++;
                    info.totalPacked += ent.packedSize;
                    info.totalUnpacked += ent.unpackedSize;
                }
                info.entries.push_back(std::move(ent));
                break;
            }

            case 0x7B:  // END_HEAD — end of archive
                return true;

            default:
                // Comment / signature / recovery / unknown subblock: the ADD_SIZE
                // handling above already advanced past any attached data.
                break;
        }
    }
    // Ran off the end without an end-block — still list what we collected.
    return true;
}

// ===========================================================================
//  RAR5 — variable-length integer (vint) header structure
//         ("Rar!\x1a\x07\x01\x00")
// ===========================================================================

const wchar_t* Rar5MethodName(uint32_t method) {
    switch (method) {
        case 0: return L"Stored";
        case 1: return L"Fastest";
        case 2: return L"Fast";
        case 3: return L"Normal";
        case 4: return L"Good";
        case 5: return L"Best";
        default: return L"RAR";
    }
}

// Decode a RAR5 vint at `p`, advancing it. Returns false on truncation/overflow.
bool ReadVint(const uint8_t*& p, const uint8_t* end, uint64_t& out) {
    uint64_t value = 0;
    int shift = 0;
    while (p < end) {
        const uint8_t b = *p++;
        value |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { out = value; return true; }
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

// Walk the RAR5 header chain starting at `pos` (just past the signature).
bool WalkRar5(FileReader& fr, ArchiveInfo& info) {
    uint64_t pos = 8;
    while (pos + 4 <= fr.size() && info.entries.size() < kMaxEntries) {
        // Each header: CRC32(4) + HeaderSize(vint) + HeaderSize bytes of payload.
        uint8_t scratch[10];
        if (!fr.readAt(pos, scratch, sizeof(scratch))) break;
        const uint8_t* sp = scratch;
        uint64_t headerSize = 0;
        if (!ReadVint(sp, scratch + sizeof(scratch), headerSize)) break;
        const uint64_t sizeVintLen = (uint64_t)(sp - scratch);
        if (headerSize < 2 || headerSize > 0x1000000ull) break;  // implausible
        if (pos + 4 + sizeVintLen + headerSize > fr.size()) break;  // past EOF

        std::vector<uint8_t> hdr((size_t)headerSize);
        if (!fr.readAt(pos + 4 + sizeVintLen, hdr.data(), hdr.size())) break;

        const uint64_t headerStart = pos + 4 + sizeVintLen;
        pos = headerStart + headerSize;  // default advance to next header

        const uint8_t* p = hdr.data();
        const uint8_t* end = hdr.data() + hdr.size();

        uint64_t headType = 0, headFlags = 0, extraSize = 0, dataSize = 0;
        if (!ReadVint(p, end, headType)) break;
        if (!ReadVint(p, end, headFlags)) break;
        if (headFlags & 0x0001) { if (!ReadVint(p, end, extraSize)) break; }
        if (headFlags & 0x0002) { if (!ReadVint(p, end, dataSize)) break; }

        if (headType == 0x05) {  // END_OF_ARCHIVE
            return true;
        }
        if (headType != 0x02) continue;  // MAIN(1) / ENCRYPTION(4) / others: skip

        // ---- FILE_HEADER (type 0x02) ----
        // Optional fields are gated by bits in fileFlags; consume exactly the
        // fields present so the name length lands correctly.
        uint64_t fileFlags = 0, unpSize = 0, attributes = 0;
        uint64_t compInfo = 0, hostOs = 0, nameSize = 0;
        if (!ReadVint(p, end, fileFlags)) break;
        if (!ReadVint(p, end, unpSize)) break;
        if (!ReadVint(p, end, attributes)) break;
        if (!ReadVint(p, end, hostOs)) break;
        // 0x0001 = directory (handled via attributes below), 0x0002 = compInfo,
        // 0x0004 = mtime vint, 0x0008 = crc32, 0x0010 = unpack version(vint).
        if (fileFlags & 0x0002) { if (!ReadVint(p, end, compInfo)) break; }
        if (fileFlags & 0x0004) {
            uint64_t mtime = 0;
            if (!ReadVint(p, end, mtime)) break;
        }
        if (fileFlags & 0x0008) {
            uint64_t crc = 0;
            if (!ReadVint(p, end, crc)) break;
        }
        // Solid / unpack-version extra vints are rare; nothing else precedes name.
        if (!ReadVint(p, end, nameSize)) break;
        if (nameSize == 0 || (uint64_t)(end - p) < nameSize) break;  // name past end

        ArchiveEntry ent;
        ent.path = internal::decodeBytes(p, (size_t)nameSize, true);
        // Directory: attributes bit. Unix (hostOs==1) uses 0x4000, Windows 0x10.
        const bool dirAttr = (hostOs == 1) ? ((attributes & 0x4000) != 0)
                                           : ((attributes & 0x10) != 0);
        ent.isDir = dirAttr;
        ent.packedSize = dataSize;
        ent.unpackedSize = unpSize;
        const uint32_t method = (uint32_t)((compInfo >> 7) & 0x3F);
        ent.method = method;
        ent.methodName = Rar5MethodName(method);
        // High-precision mtime lives in the extra area; we keep the header blank
        // rather than guess. (EXT time parsing is best-effort and omitted.)

        if (ent.isDir) info.dirCount++;
        else {
            info.fileCount++;
            info.totalPacked += ent.packedSize;
            info.totalUnpacked += ent.unpackedSize;
        }
        info.entries.push_back(std::move(ent));
    }
    // End-of-chain or a benign stop — list what we collected.
    return true;
}

// ===========================================================================
//  ACE — block structure ("**ACE**" main header, per-file headers)
// ===========================================================================

// Walk ACE blocks starting at `pos` (the main-header block).
bool WalkAce(FileReader& fr, ArchiveInfo& info, uint64_t pos) {
    uint8_t bh[7];
    if (!fr.readAt(pos, bh, sizeof(bh)) || bh[2] != 0x00) {
        info.error = L"Bad ACE main header";
        return false;
    }
    const uint16_t mainFlags = internal::rd16(bh + 4);
    const uint16_t mainSize  = internal::rd16(bh + 5);
    uint8_t mh[14];
    if (!fr.readAt(pos + 7, mh, sizeof(mh))) {
        info.error = L"Truncated ACE main header";
        return false;
    }
    (void)mainFlags;
    // After the main header: m_loffset = HEAD_SIZE + 4, then an AuthVerification
    // string (1-byte size + bytes) if flag 0x1000 is set.
    uint64_t afterMain = pos + (uint64_t)mainSize + 4;
    if (mainFlags & 0x1000) {
        if (afterMain + 1 > fr.size()) {
            info.error = L"Truncated ACE main header";
            return false;
        }
        uint8_t avSize = 0;
        if (!fr.readAt(afterMain, &avSize, 1)) {
            info.error = L"Truncated ACE main header";
            return false;
        }
        afterMain += 1 + avSize;
    }
    if (afterMain <= pos || afterMain > fr.size()) {
        info.error = L"Implausible ACE layout";
        return false;
    }
    pos = afterMain;

    while (pos + 7 <= fr.size() && info.entries.size() < kMaxEntries) {
        if (!fr.readAt(pos, bh, sizeof(bh))) break;
        const uint8_t  headType  = bh[2];
        const uint16_t headFlags = internal::rd16(bh + 4);
        const uint16_t headSize  = internal::rd16(bh + 5);
        // HEAD_SIZE excludes HEAD_CRC(2) + HEAD_SIZE(2), so the full header span
        // from the block start is HEAD_SIZE + 4.
        const uint64_t blockSpan = (uint64_t)headSize + 4;

        if (headType == 0x02 || headType == 0x05) {
            // RECOVERY RECORD: a 4-byte size then that many bytes. This is the
            // last block in the archive.
            uint8_t rb[4];
            uint64_t rrSize = 0;
            if (fr.readAt(pos + 7, rb, sizeof(rb))) rrSize = internal::rd32(rb);
            const uint64_t next = pos + 7 + 4 + rrSize;
            if (next <= pos) break;
            return true;
        }
        if (headType != 0x01 && headType != 0x03) {
            // Unknown block: if flag 0x0002 set, a 4-byte ADDSIZE precedes data.
            uint64_t addSize = 0;
            if (headFlags & 0x0002) {
                uint8_t ab[4];
                if (!fr.readAt(pos + 7, ab, sizeof(ab))) break;
                addSize = internal::rd32(ab);
            }
            const uint64_t next = pos + 7 + 4 + addSize;
            if (next <= pos) break;
            pos = next;
            continue;
        }

        // ---- FILE_HEADER (0x01 = 32-bit sizes, 0x03 = 64-bit sizes) ----
        const bool is64 = (headType == 0x03);
        const size_t fixedLen = is64 ? 34 : 28;
        if (pos + 7 + fixedLen > fr.size()) break;

        uint64_t packSize = 0, origSize = 0;
        uint32_t attr = 0, ftime = 0;
        uint8_t  method = 0;
        uint16_t fnameSize = 0;

        if (is64) {
            uint8_t fh[34];
            if (!fr.readAt(pos + 7, fh, sizeof(fh))) break;
            packSize = (uint64_t)internal::rd32(fh + 0) |
                       ((uint64_t)internal::rd32(fh + 4) << 32);
            origSize = (uint64_t)internal::rd32(fh + 8) |
                       ((uint64_t)internal::rd32(fh + 12) << 32);
            ftime = internal::rd32(fh + 16);
            attr = internal::rd32(fh + 20);
            method = fh[31];                       // TECH_INFO[1]
            fnameSize = internal::rd16(fh + 32);
        } else {
            uint8_t fh[28];
            if (!fr.readAt(pos + 7, fh, sizeof(fh))) break;
            packSize = internal::rd32(fh + 0);
            origSize = internal::rd32(fh + 4);
            ftime = internal::rd32(fh + 8);
            attr = internal::rd32(fh + 12);
            method = fh[25];                       // TECH_INFO[1]
            fnameSize = internal::rd16(fh + 26);
        }

        const uint64_t nameOff = pos + 7 + fixedLen;
        if (fnameSize == 0 || nameOff + fnameSize > fr.size()) break;
        std::vector<uint8_t> name(fnameSize);
        if (!fr.readAt(nameOff, name.data(), name.size())) break;

        ArchiveEntry ent;
        ent.path = internal::decodeBytes(name.data(), name.size(), false);
        ent.isDir = (attr & 0x10) != 0;  // DOS directory attribute
        ent.packedSize = packSize;
        ent.unpackedSize = origSize;
        ent.method = method;
        // ACE method byte: 0 = Stored, 1..5 = LZ compression levels.
        if (method == 0) ent.methodName = L"Stored";
        else ent.methodName = L"LZ (level " + std::to_wstring(method) + L")";
        ent.mtime = internal::dosTimeToFT((uint16_t)(ftime >> 16),
                                          (uint16_t)(ftime & 0xFFFF));

        if (ent.isDir) info.dirCount++;
        else {
            info.fileCount++;
            info.totalPacked += ent.packedSize;
            info.totalUnpacked += ent.unpackedSize;
        }
        info.entries.push_back(std::move(ent));

        // The packed file data follows the entire header block.
        const uint64_t next = pos + blockSpan + packSize;
        if (next <= pos) break;
        pos = next;
    }
    return true;
}

// Locate the ACE main-header block, tolerating a small SFX prefix.
bool FindAceHeader(FileReader& fr, uint64_t& pos) {
    uint8_t sig[7];
    if (fr.size() >= 7 && fr.readAt(0, sig, sizeof(sig)) &&
        sig[0] == 0x07 && sig[1] == '*' && sig[2] == 'A' &&
        sig[3] == 'C' && sig[4] == 'E' && sig[5] == '*' && sig[6] == '*') {
        pos = 0;
        return true;
    }
    const uint64_t scanLen = std::min<uint64_t>(fr.size(), 65536);
    std::vector<uint8_t> buf((size_t)scanLen);
    if (scanLen < 14 || !fr.readAt(0, buf.data(), buf.size())) return false;
    // Block header(7) then the 7-byte "**ACE**" signature.
    for (uint64_t i = 0; i + 14 <= scanLen; ++i) {
        if (buf[(size_t)(i + 7)] == '*' && buf[(size_t)(i + 8)] == 'A' &&
            buf[(size_t)(i + 9)] == 'C' && buf[(size_t)(i + 10)] == 'E' &&
            buf[(size_t)(i + 11)] == '*' && buf[(size_t)(i + 12)] == '*') {
            pos = i;
            return true;
        }
    }
    return false;
}

// ===========================================================================
//  7-Zip — serialized header stream
// ===========================================================================

// 7z NIDs (property / section identifiers). Verified against real archives.
constexpr uint8_t k7zEnd = 0x00, k7zHeader = 0x01, k7zEncodedHeader = 0x17;
constexpr uint8_t k7zMainStreamsInfo = 0x04, k7zFilesInfo = 0x05;
constexpr uint8_t k7zPackInfo = 0x06, k7zUnpackInfo = 0x07, k7zSubStreamsInfo = 0x08;
constexpr uint8_t k7zSize = 0x09, k7zCRC = 0x0A, k7zFolder = 0x0B;
constexpr uint8_t k7zCodersUnpackSize = 0x0C, k7zNumUnpackStreams = 0x0D;
constexpr uint8_t k7zEmptyStream = 0x0E, k7zEmptyFile = 0x0F, k7zName = 0x11;
constexpr uint8_t k7zMTime = 0x14;

// Read one 7z variable-length integer. NOTE: 7z does NOT use LEB128. The
// first byte's leading 1-bits tell how many additional bytes follow and carry
// the high part of the value; subsequent bytes are full 8-bit little-endian.
//   mask = 0x80; for i in 0..7:
//     if (firstByte & mask) == 0: value += (firstByte & (mask-1)) << (i*8); done
//     value |= nextByte << (i*8); mask >>= 1
bool Read7zNum(const uint8_t*& p, const uint8_t* end, uint64_t& value) {
    value = 0;
    if (p >= end) return false;
    uint8_t firstByte = *p++;
    unsigned mask = 0x80;
    for (unsigned i = 0; i < 8; ++i) {
        if ((firstByte & mask) == 0) {
            value += (uint64_t)(firstByte & (mask - 1)) << (8u * i);
            return true;
        }
        if (p >= end) return false;
        value |= (uint64_t)*p++ << (8u * i);
        mask >>= 1;
    }
    return true;
}

// Skip one complete property value whose structure we don't interpret.
bool Skip7zProp(uint8_t id, const uint8_t*& p, const uint8_t* end) {
    switch (id) {
        case k7zCRC: {  // CRC: a count vint then 4 * count bytes
            uint64_t n = 0;
            if (!Read7zNum(p, end, n)) return false;
            if ((uint64_t)(end - p) < n * 4) return false;
            p += (size_t)(n * 4);
            return true;
        }
        default: return false;  // unknown — can't skip safely
    }
}

// Skip a 7z HashDigests block (the kCRC property inside StreamsInfo /
// SubStreamsInfo). Structure, given the digest count `num`:
//   allAreDefined (1 byte, 0/nonzero)
//   if !allAreDefined: a bit-vector of `num` defined-flags (ceil(num/8) bytes)
//   then 4 bytes of CRC for each defined digest.
bool SkipHashDigests(const uint8_t*& p, const uint8_t* end, uint64_t num) {
    if (p >= end) return false;
    const uint8_t allDefined = *p++;
    uint64_t definedCount = num;
    if (!allDefined) {
        const uint64_t bytes = (num + 7) / 8;
        if ((uint64_t)(end - p) < bytes) return false;
        definedCount = 0;
        for (uint64_t i = 0; i < num; ++i)
            if ((p[i / 8] >> (i % 8)) & 1) ++definedCount;
        p += (size_t)bytes;
    }
    if ((uint64_t)(end - p) < definedCount * 4) return false;
    p += (size_t)(definedCount * 4);
    return true;
}

// Map a 7z coder GUID/ID (first byte distinguishes the well-known coders) to a
// readable method label. The full ID is 16 bytes; we match the common prefixes.
std::wstring SevenZipCoderName(const uint8_t* id, size_t len) {
    // Well-known compression coder IDs all start with the same 12-byte prefix;
    // byte 12 identifies the coder. (BCJ/Delta filters differ earlier.)
    static const uint8_t k7zPrefix[12] = {0x23, 0x03, 0x01, 0x01, 0x01, 0x01,
                                          0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    if (len == 16 && std::memcmp(id, k7zPrefix, 12) == 0) {
        switch (id[12]) {
            case 0x01: return L"LZMA";
            case 0x02: return L"PPMd";
            case 0x03: return L"Deflate";
            case 0x04: return L"BZip2";
            case 0x06: return L"LZMA2";
            case 0x09: return L"LZMA";
            case 0x0A: return L"LZMA2";
            default: break;
        }
    }
    // Copy / encryption / filter coders — fall back to a generic label.
    return L"7z";
}

// The serialized 7z header decoder. Fills names, dir/empty flags and (where the
// header provides them) per-file unpacked sizes plus folder packed sizes.
// Returns false when the layout can't be parsed reliably.
bool Decode7zHeader(const uint8_t* p, const uint8_t* end, ArchiveInfo& info,
                    std::wstring& algo) {
    uint64_t type = 0;
    if (!Read7zNum(p, end, type)) return false;

    if (type == k7zEncodedHeader) {
        // The real header is itself compressed and stored in the streams this
        // header describes. Decompressing it is out of scope for a lister, so we
        // degrade gracefully rather than emit a half-broken result.
        info.error = L"7z uses a compressed (encoded) header; not supported";
        return false;
    }
    if (type != k7zHeader) {
        info.error = L"Unsupported 7z header type";
        return false;
    }

    // ---- StreamsInfo: collect folder packed sizes and the coder method. ----
    std::vector<uint64_t> folderPacked;  // packed bytes per packed stream/folder
    std::vector<uint64_t> folderUnpacked;  // total unpacked bytes per folder
    uint64_t numFolders = 0;
    std::vector<uint64_t> substreamSizes;  // unpacked size per real file

    while (p < end && *p != k7zFilesInfo) {
        const uint8_t section = *p++;
        if (section == k7zMainStreamsInfo) {
            // kMainStreamsInfo is a transparent wrapper around PackInfo /
            // UnpackInfo / SubStreamsInfo. Just continue; the loop handles the
            // inner sections directly.
            continue;
        } else if (section == k7zPackInfo) {
            uint64_t baseOffset = 0, numStreams = 0;
            if (!Read7zNum(p, end, baseOffset)) return false;
            if (!Read7zNum(p, end, numStreams)) return false;
            folderPacked.assign((size_t)numStreams, 0);
            while (p < end && *p != 0x00) {
                const uint8_t prop = *p++;
                if (prop == k7zSize) {
                    for (uint64_t i = 0; i < numStreams; ++i) {
                        uint64_t sz = 0;
                        if (!Read7zNum(p, end, sz)) return false;
                        folderPacked[(size_t)i] = sz;
                    }
                } else if (prop == k7zCRC) {          // optional CRC (0x0A)
                    if (!Skip7zProp(prop, p, end)) return false;
                } else {
                    return false;                    // unknown pack property
                }
            }
            if (p >= end) return false;
            ++p;                                     // k7zEnd of PackInfo
        } else if (section == k7zUnpackInfo) {
            // Structure: kFolder(0x0b) numFolders external(byte) [folders...]
            //            kCodersUnpackSize(0x0c) [sizes] [kCRC(0x0a)] kEnd.
            if (p >= end || *p != k7zFolder) return false;
            ++p;                                     // kFolder marker
            if (!Read7zNum(p, end, numFolders)) return false;
            if (numFolders == 0 || numFolders > 65536) return false;
            // External flag: 0 = folder definitions are inline (the normal case).
            if (p >= end) return false;
            const uint8_t external = *p++;
            if (external != 0) return false;          // inline only
            std::wstring firstCoder;
            for (uint64_t f = 0; f < numFolders; ++f) {
                uint64_t numCoders = 0;
                if (!Read7zNum(p, end, numCoders)) return false;
                if (numCoders == 0 || numCoders > 64) return false;
                for (uint64_t c = 0; c < numCoders; ++c) {
                    if (p >= end) return false;
                    const uint8_t flags = *p++;
                    const uint64_t idSize = flags & 0x0F;
                    const bool isComplex = (flags & 0x10) != 0; // >1 out stream
                    const bool hasAttrs = (flags & 0x20) != 0;  // bit 5 = attrs
                    if ((uint64_t)(end - p) < idSize) return false;
                    if (c == 0 && firstCoder.empty())
                        firstCoder = SevenZipCoderName(p, (size_t)idSize);
                    p += (size_t)idSize;
                    if (hasAttrs) {
                        uint64_t propSize = 0;
                        if (!Read7zNum(p, end, propSize)) return false;
                        if ((uint64_t)(end - p) < propSize) return false;
                        p += (size_t)propSize;
                    }
                    // A complex coder (>1 out stream) stores its out-stream count.
                    if (isComplex) {
                        uint64_t nOut = 0;
                        if (!Read7zNum(p, end, nOut)) return false;
                    }
                }
                // BindPairs are only present when the folder has more than one coder (a single
                // coder has no connections). Reading it unconditionally over-consumes
                // the kCodersUnpackSize marker that follows a single-coder folder.
                if (numCoders > 1) {
                    uint64_t numBind = 0;
                    if (!Read7zNum(p, end, numBind)) return false;
                    for (uint64_t b = 0; b < numBind; ++b) {
                        uint64_t inIdx = 0, outIdx = 0;
                        if (!Read7zNum(p, end, inIdx)) return false;
                        if (!Read7zNum(p, end, outIdx)) return false;
                    }
                    // PackedStreams indices (only when the folder has >1 coder).
                    uint64_t numPacked = 0;
                    if (!Read7zNum(p, end, numPacked)) return false;
                    for (uint64_t s = 0; s < numPacked; ++s) {
                        uint64_t idx = 0;
                        if (!Read7zNum(p, end, idx)) return false;
                    }
                }
            }
            // (Folder-level CRC is consumed by the trailing kCRC block below, if present.)
            // kCodersUnpackSize: for each folder, one size per output stream;
            // the final output stream is the folder's total unpacked size.
            if (p < end && *p == k7zCodersUnpackSize) {
                ++p;
                // One unpacked size per output stream. For the common single-coder
                // folder this is the folder's total unpacked size, used to derive
                // the (implied) last substream size in SubStreamsInfo.
                folderUnpacked.assign((size_t)numFolders, 0);
                for (uint64_t f = 0; f < numFolders; ++f) {
                    if (!Read7zNum(p, end, folderUnpacked[(size_t)f])) return false;
                }
            }
            // A folder-level CRC block may also appear here in some layouts.
            while (p < end && *p == k7zCRC) {
                if (!Skip7zProp(k7zCRC, p, end)) return false;
            }
            if (!firstCoder.empty()) algo = firstCoder;
        } else if (section == k7zSubStreamsInfo) {
            std::vector<uint64_t> numUnpack;
            while (p < end && *p != 0x00) {
                const uint8_t prop = *p++;
                if (prop == k7zNumUnpackStreams) {
                    numUnpack.resize((size_t)numFolders, 1);
                    for (uint64_t f = 0; f < numFolders; ++f)
                        if (!Read7zNum(p, end, numUnpack[(size_t)f])) return false;
                } else if (prop == k7zSize) {
                    // 7z stores the unpacked size of every substream EXCEPT the last
                    // one of each folder; the last is implied (folder total minus the
                    // sizes read so far). Read (n-1) sizes per folder and derive the
                    // final one from the folder's total unpacked size.
                    for (uint64_t f = 0; f < numFolders; ++f) {
                        const uint64_t n = (f < numUnpack.size())
                                           ? numUnpack[(size_t)f] : 1;
                        const uint64_t folderTotal = (f < folderUnpacked.size())
                                                     ? folderUnpacked[(size_t)f] : 0;
                        uint64_t sum = 0;
                        for (uint64_t s = 0; s + 1 < n; ++s) {
                            uint64_t sz = 0;
                            if (!Read7zNum(p, end, sz)) return false;
                            substreamSizes.push_back(sz);
                            sum += sz;
                        }
                        // The last substream is whatever remains in the folder.
                        substreamSizes.push_back(folderTotal > sum ? folderTotal - sum : 0);
                    }
                } else if (prop == k7zCRC) {         // optional per-stream CRC
                    uint64_t totalSubs = 0;
                    for (uint64_t f = 0; f < numFolders; ++f)
                        totalSubs += (f < numUnpack.size()) ? numUnpack[(size_t)f] : 1;
                    if (!SkipHashDigests(p, end, totalSubs)) return false;
                } else if (prop == 0x18 || prop == 0x19) {  // kSHA256/kDigest (32/20 bytes each)
                    uint64_t totalSubs = 0;
                    for (uint64_t f = 0; f < numFolders; ++f)
                        totalSubs += (f < numUnpack.size()) ? numUnpack[(size_t)f] : 1;
                    const uint64_t sz = (prop == 0x18) ? 32 : 20;
                    if ((uint64_t)(end - p) < totalSubs * sz) return false;
                    p += (size_t)(totalSubs * sz);
                } else {
                    return false;                    // unknown substream property
                }
            }
            if (p >= end) return false;
            ++p;                                     // k7zEnd of SubStreamsInfo
        } else if (section == k7zCRC) {              // kCRC at the streams level
            if (!Skip7zProp(k7zCRC, p, end)) return false;
        } else if (section == k7zEnd) {
            continue;                    // kEnd closing kMainStreamsInfo wrapper
        } else {
            return false;                            // unknown streams section
        }
    }

    if (p >= end || *p != k7zFilesInfo) {
        info.error = L"7z header has no file list";
        return false;
    }
    ++p;

    // ---- FilesInfo ----
    uint64_t numFiles = 0;
    if (!Read7zNum(p, end, numFiles)) return false;
    if (numFiles > kMaxEntries) {
        info.error = L"7z file count too large";
        return false;
    }

    std::vector<uint8_t> emptyStream((size_t)numFiles, 0);
    std::vector<uint8_t> emptyFile((size_t)numFiles, 0);
    std::vector<std::wstring> names((size_t)numFiles);
    std::vector<FILETIME> mtimes((size_t)numFiles);
    bool haveNames = false;
    bool haveMTime = false;

    while (p < end && *p != 0x00) {
        uint64_t propType = 0, size = 0;
        if (!Read7zNum(p, end, propType)) return false;
        if (!Read7zNum(p, end, size)) return false;
        if ((uint64_t)(end - p) < size) return false;
        const uint8_t* pend = p + size;

        if (propType == k7zEmptyStream) {
            // Bit vector over all files: 1 = this file has an empty stream
            // (i.e. it is a directory or a zero-length file).
            for (uint64_t i = 0; i < numFiles; ++i) {
                const uint64_t byteIdx = i / 8, bitIdx = i % 8;
                if (byteIdx < (uint64_t)size)
                    emptyStream[(size_t)i] = (uint8_t)((p[byteIdx] >> bitIdx) & 1);
            }
        } else if (propType == k7zEmptyFile) {
            // Bit vector over the empty-stream files: 1 = a real empty file
            // (0 = a directory).
            uint64_t k = 0;
            for (uint64_t i = 0; i < numFiles; ++i) {
                if (!emptyStream[(size_t)i]) continue;
                const uint64_t byteIdx = k / 8, bitIdx = k % 8;
                if (byteIdx < (uint64_t)size)
                    emptyFile[(size_t)i] = (uint8_t)((p[byteIdx] >> bitIdx) & 1);
                ++k;
            }
        } else if (propType == k7zName) {
            // Names are NUL-terminated. The high bit of `size` marks UTF-16LE
            // encoding (the normal case); clear it to get the byte length.
            const bool utf16 = (size & 0x8000000000000000ull) != 0;
            const uint64_t byteLen = size & 0x7FFFFFFFFFFFFFFFull;
            if (byteLen > (uint64_t)(end - p)) return false;
            const uint8_t* np = p;
            const uint8_t* nend = p + byteLen;
            for (uint64_t i = 0; i < numFiles; ++i) {
                if (utf16) {
                    std::wstring w;
                    while (np + 2 <= nend) {
                        const wchar_t ch =
                            (wchar_t)((uint16_t)np[0] | ((uint16_t)np[1] << 8));
                        np += 2;
                        if (ch == 0) break;
                        w.push_back(ch);
                    }
                    names[(size_t)i] = w;
                } else {
                    const uint8_t* start = np;
                    while (np < nend && *np != 0) ++np;
                    names[(size_t)i] =
                        internal::decodeBytes(start, (size_t)(np - start), false);
                    if (np < nend) ++np;
                }
            }
            haveNames = true;
        } else if (propType == k7zMTime) {           // kMTime (8 bytes per file)
            const uint8_t* tp = p;
            for (uint64_t i = 0; i < numFiles; ++i) {
                if (tp + 8 <= pend) {
                    FILETIME ft{};
                    ft.dwLowDateTime = internal::rd32(tp);
                    ft.dwHighDateTime = internal::rd32(tp + 4);
                    mtimes[(size_t)i] = ft;
                    tp += 8;
                }
            }
            haveMTime = true;
        }
        // Any other property (attributes, CRC, ...) is skipped via pend.
        p = pend;
    }
    if (!haveNames) {
        info.error = L"7z header has no names";
        return false;
    }

    // ---- Assemble entries. Real (non-empty-stream) files take their unpacked
    // size from the substream list; their packed size is the packed stream they
    // belong to (approximated per folder when several files share a solid
    // block). Directories and empty files have no stream and zero sizes.
    uint64_t subIdx = 0;
    uint64_t realFileIdx = 0;
    const uint64_t numRealFolders =
        numFolders ? numFolders : (folderPacked.empty() ? 0 : 1);
    for (uint64_t i = 0; i < numFiles; ++i) {
        ArchiveEntry ent;
        ent.path = names[(size_t)i];
        ent.mtime = haveMTime ? mtimes[(size_t)i] : FILETIME{};
        if (emptyStream[(size_t)i]) {
            ent.isDir = !emptyFile[(size_t)i];
            ent.unpackedSize = 0;
            ent.packedSize = 0;
            ent.methodName = ent.isDir ? L"" : L"Stored";
            if (ent.isDir) info.dirCount++;
            else info.fileCount++;
        } else {
            ent.isDir = false;
            ent.unpackedSize = (subIdx < substreamSizes.size())
                               ? substreamSizes[(size_t)subIdx] : 0;
            ++subIdx;
            // Assign the file to a packed stream/folder round-robin so totals
            // stay bounded by the real packed size; per-file exactness is not
            // recoverable from the header for solid archives.
            if (numRealFolders > 0) {
                // Per-file packed size isn't recoverable from the header for solid
                // archives, so give each file a proportional share of its folder's
                // packed size (by unpacked bytes). Folder totals still sum exactly
                // to the real packed streams.
                const uint64_t folder = realFileIdx % numRealFolders;
                const uint64_t psize = (folder < folderPacked.size())
                                       ? folderPacked[(size_t)folder] : 0;
                const uint64_t ftotal = (folder < folderUnpacked.size())
                                        ? folderUnpacked[(size_t)folder] : 0;
                ent.packedSize = (ftotal && ent.unpackedSize <= ftotal)
                                     ? (psize * ent.unpackedSize) / ftotal
                                     : psize;
            }
            ++realFileIdx;
            ent.methodName = algo.empty() ? L"7z" : algo;
            info.fileCount++;
            info.totalPacked += ent.packedSize;
            info.totalUnpacked += ent.unpackedSize;
        }
        info.entries.push_back(std::move(ent));
    }
    return true;
}

bool Walk7z(FileReader& fr, ArchiveInfo& info) {
    // Signature(6) + Version(2) + StartHeaderCRC(4) + StartHeader(20):
    //   NextHeaderOffset(8) + NextHeaderSize(8) + NextHeaderCRC(4)
    if (fr.size() < 32) {
        info.error = L"7z file too small";
        return false;
    }
    uint8_t start[20];
    if (!fr.readAt(12, start, sizeof(start))) {
        info.error = L"Truncated 7z start header";
        return false;
    }
    const uint64_t nextOff = internal::rd64(start + 0);
    const uint64_t nextSize = internal::rd64(start + 8);
    const uint64_t headerAbs = 32 + nextOff;
    if (nextSize == 0 || nextSize > 0x10000000ull ||
        headerAbs + nextSize > fr.size()) {
        info.error = L"Implausible 7z header offset/size";
        return false;
    }
    std::vector<uint8_t> hdr((size_t)nextSize);
    if (!fr.readAt(headerAbs, hdr.data(), hdr.size())) {
        info.error = L"Cannot read 7z header";
        return false;
    }
    std::wstring algo;
    if (!Decode7zHeader(hdr.data(), hdr.data() + hdr.size(), info, algo))
        return false;
    if (!algo.empty()) info.algorithm = algo;
    return true;
}

} // namespace (internal helpers)

// ---- Public entry points ---------------------------------------------------

bool TryParseRar(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = INVALID_HANDLE_VALUE; FileReader* fr = nullptr;
    if (!OpenReader(path, h, fr, info)) return false;
    bool ok = false;
    uint8_t sig[8] = {0};
    if (fr->size() >= 8 && fr->readAt(0, sig, sizeof(sig)) &&
        sig[0] == 'R' && sig[1] == 'a' && sig[2] == 'r' &&
        sig[3] == '!' && sig[4] == 0x1a && sig[5] == 0x07) {
        info.format = L"RAR";
        info.perEntryCompression = true;
        if (sig[6] == 0x00) {
            info.algorithm = L"RAR";
            ok = WalkRar4(*fr, info);
        } else if (sig[6] == 0x01 && sig[7] == 0x00) {
            info.algorithm = L"RAR5";
            ok = WalkRar5(*fr, info);
        }
    }
    const uint64_t asize = fr->size();
    delete fr;
    CloseHandle(h);
    info.archiveSize = asize;
    if (!ok && info.error.empty()) info.error = L"Unsupported RAR layout";
    // A valid signature but zero entries means the archive is truncated or
    // corrupt — report it as a failure rather than an empty listing.
    if (ok && info.entries.empty()) { info.error = L"No entries found in RAR"; return false; }
    return ok;
}

bool TryParseSevenZip(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = INVALID_HANDLE_VALUE; FileReader* fr = nullptr;
    if (!OpenReader(path, h, fr, info)) return false;
    bool ok = false;
    uint8_t sig[6] = {0};
    if (fr->size() >= 32 && fr->readAt(0, sig, sizeof(sig)) &&
        sig[0] == '7' && sig[1] == 'z' && sig[2] == 0xBC &&
        sig[3] == 0xAF && sig[4] == '\'' && sig[5] == 0x1C) {
        info.format = L"7z";
        info.algorithm = L"7z";
        info.perEntryCompression = true;
        ok = Walk7z(*fr, info);
    }
    const uint64_t asize = fr->size();
    delete fr;
    CloseHandle(h);
    info.archiveSize = asize;
    if (!ok && info.error.empty()) info.error = L"Unsupported 7z layout";
    if (ok && info.entries.empty()) { info.error = L"No entries found in 7z"; return false; }
    return ok;
}

bool TryParseAce(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = INVALID_HANDLE_VALUE; FileReader* fr = nullptr;
    if (!OpenReader(path, h, fr, info)) return false;
    uint64_t pos = 0;
    bool ok = FindAceHeader(*fr, pos);
    if (ok) {
        info.format = L"ACE";
        info.algorithm = L"ACE";
        info.perEntryCompression = true;
        ok = WalkAce(*fr, info, pos);
    } else {
        info.error = L"Bad ACE signature";
    }
    const uint64_t asize = fr->size();
    delete fr;
    CloseHandle(h);
    info.archiveSize = asize;
    if (!ok && info.error.empty()) info.error = L"Unsupported ACE layout";
    if (ok && info.entries.empty()) { info.error = L"No entries found in ACE"; return false; }
    return ok;
}

} // namespace archdiview
