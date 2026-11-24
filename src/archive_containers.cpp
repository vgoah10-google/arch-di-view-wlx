// archive_containers.cpp — Native CAB and CPIO parsers (no external DLL).
//
// Ports the proven directory-walking logic from ArchView's TCab/TCpio onto the
// internal::FileReader helper. Each parser reads only the container's directory
// metadata (no extraction) and fills an ArchiveInfo.
//
//   CAB  — Microsoft Cabinet. MSCF header gives the folder/file counts and the
//          offset of the first CFFILE record. Folder headers (one per folder,
//          each carrying a compression-method byte) precede the CFFILE entries;
//          each CFFILE is a fixed 16-byte record followed by a NUL-terminated
//          name. Compression is per-folder, so per-entry method labels are
//          resolved through the entry's iFolder index. Per-file *compressed*
//          size is not stored in the directory (it lives in the CFDATA blocks),
//          so packed==unpacked here, mirroring ArchView which only sums UNP_SIZE.
//   CPIO — Unix "new ASCII" (newc) format. A 110-byte ASCII header (6-byte
//          magic + 13 eight-char octal fields), then the NUL-terminated name
//          padded to 4 bytes, then the file data padded to 4 bytes. The
//          "TRAILER!!!" entry ends the archive. Always uncompressed.
#include "archive.h"
#include "archive_internal.h"

#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <algorithm>

namespace archdiview {
namespace {

// ---- CAB -------------------------------------------------------------------

constexpr uint32_t kCabSignature = 0x4643534Du; // "MSCF" little-endian
constexpr size_t   kCabHeaderSize = 36;         // CFHEADER
constexpr size_t   kCabFolderSize = 8;          // CFFOLDER
constexpr size_t   kCabFileRecSize = 16;        // CFFILE without the name

// Map a CFFOLDER compression-method byte to a human-readable label.
const wchar_t* CabMethodName(uint8_t method) {
    switch (method) {
        case 0: return L"Stored (uncompressed)"; // none
        case 1: return L"MSZIP";
        case 2: return L"Quantum";
        case 3: return L"LZX";
        default: return L"Unknown";
    }
}

// Dominant method label across the folder table (used for info.algorithm).
std::wstring CabDominantMethod(const std::vector<uint8_t>& methods) {
    if (methods.empty()) return L"Stored (uncompressed)";
    uint32_t counts[4] = {0, 0, 0, 0};
    for (uint8_t m : methods)
        if (m < 4) counts[m]++;
    uint8_t best = 0;
    for (uint8_t m = 1; m < 4; ++m)
        if (counts[m] > counts[best]) best = m;
    // If nothing compressed, everything is stored.
    if (counts[best] == 0) return L"Stored (uncompressed)";
    return CabMethodName(best);
}

} // namespace (internal helpers)

bool TryParseCab(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    internal::FileReader r(h);
    const uint64_t size = r.size();
    bool ok = false;

    do {
        if (size < kCabHeaderSize) { info.error = L"File too small for CAB"; break; }

        uint8_t hdr[kCabHeaderSize];
        if (!r.readAt(0, hdr, sizeof(hdr))) { info.error = L"Read error"; break; }
        if (internal::rd32(hdr) != kCabSignature) { info.error = L"Bad CAB signature"; break; }

        const uint32_t cbCFHeader = internal::rd32(hdr + 16); // offset of first CFFILE
        const uint16_t numFolders = internal::rd16(hdr + 26);
        const uint16_t numFiles   = internal::rd16(hdr + 28);

        // Sanity: the file-entry region must start inside the file and after
        // the header + folder table.
        const uint64_t minFileOff = kCabHeaderSize +
                                    (uint64_t)numFolders * kCabFolderSize;
        if (cbCFHeader < minFileOff || cbCFHeader >= size) {
            info.error = L"Implausible CAB layout";
            break;
        }

        // Read the folder table starting right after the CFHEADER. Each folder
        // is 8 bytes; the compression-method byte sits at offset +6.
        std::vector<uint8_t> folderMethods;
        folderMethods.reserve(numFolders);
        for (uint32_t i = 0; i < numFolders; ++i) {
            const uint64_t fo = kCabHeaderSize + (uint64_t)i * kCabFolderSize;
            if (fo + kCabFolderSize > size) break;
            uint8_t fh[kCabFolderSize];
            if (!r.readAt(fo, fh, sizeof(fh))) break;
            folderMethods.push_back(fh[6]);
        }

        // Walk the CFFILE entries. Each: unpacked-size(4), folder-offset(4),
        // iFolder(2), date(2), time(2), attribs(2), then a NUL-terminated name.
        // The count from the header bounds the loop; a running cursor tracks the
        // variable-length records and guards against no-progress / overrun.
        info.perEntryCompression = true;
        uint64_t cursor = cbCFHeader;
        for (uint32_t i = 0; i < numFiles; ++i) {
            if (cursor + kCabFileRecSize > size) break; // truncated / end

            uint8_t rec[kCabFileRecSize];
            if (!r.readAt(cursor, rec, sizeof(rec))) break;

            const uint64_t unpacked = internal::rd32(rec + 0);
            const uint16_t iFolder  = internal::rd16(rec + 8);
            const uint16_t fdate    = internal::rd16(rec + 10);
            const uint16_t ftime    = internal::rd16(rec + 12);
            const uint16_t attribs  = internal::rd16(rec + 14);

            // Read the NUL-terminated name byte-by-byte, bounded by the file
            // end and a generous maximum to avoid runaway reads.
            const uint64_t nameOff = cursor + kCabFileRecSize;
            const uint64_t nameMax = std::min<uint64_t>(size - nameOff, 1024);
            std::string name;
            bool foundNul = false;
            for (uint64_t k = 0; k < nameMax; ++k) {
                uint8_t ch = 0;
                if (!r.readAt(nameOff + k, &ch, 1)) break;
                if (ch == '\0') { foundNul = true; break; }
                name.push_back((char)ch);
            }
            if (!foundNul) break; // malformed name — stop walking

            const uint64_t nextCursor = nameOff + name.size() + 1;
            if (nextCursor <= cursor) break; // no progress — avoid infinite loop

            ArchiveEntry ent;
            ent.path = internal::decodeBytes(
                (const uint8_t*)name.data(), name.size(), true);
            ent.isDir = (attribs & 0x0010) != 0; // FILE_ATTRIBUTE_DIRECTORY
            ent.unpackedSize = unpacked;
            ent.packedSize = unpacked; // per-file compressed size not in the dir
            ent.mtime = internal::dosTimeToFT(fdate, ftime);

            const uint8_t method =
                (iFolder < folderMethods.size()) ? folderMethods[iFolder] : 0;
            ent.method = method;
            ent.methodName = CabMethodName(method);

            if (ent.isDir) info.dirCount++;
            else {
                info.fileCount++;
                info.totalPacked += ent.packedSize;
                info.totalUnpacked += ent.unpackedSize;
            }
            info.entries.push_back(std::move(ent));

            cursor = nextCursor;
        }

        info.format = L"CAB";
        info.algorithm = CabDominantMethod(folderMethods);
        info.archiveSize = size;
        ok = true;
    } while (false);

    CloseHandle(h);
    return ok;
}

// ---- CPIO ------------------------------------------------------------------

constexpr size_t kCpioMagicLen = 6;
constexpr size_t kCpioHeaderSize = 110; // 6 magic + 13 * 8
constexpr size_t kCpioFieldLen = 8;

// Decode an ASCII octal field (may be NUL/space terminated) to a number.
uint64_t CpioOctal(const char* field, size_t len) {
    uint64_t value = 0;
    size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) ++i;
    for (; i < len && field[i] >= '0' && field[i] <= '7'; ++i)
        value = value * 8 + (uint64_t)(field[i] - '0');
    return value;
}

inline uint64_t CpioAlign4(uint64_t n) { return (n + 3) & ~(uint64_t)3; }

bool TryParseCpio(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    internal::FileReader r(h);
    const uint64_t size = r.size();
    bool ok = false;

    do {
        uint64_t off = 0;
        bool any = false;

        while (off + kCpioMagicLen <= size) {
            // Magic: "070701" (newc) or "070702" (crc).
            char magic[kCpioMagicLen + 1] = {};
            if (!r.readAt(off, magic, kCpioMagicLen)) break;
            if (!internal::hasSig((const uint8_t*)magic, kCpioMagicLen, "070701") &&
                !internal::hasSig((const uint8_t*)magic, kCpioMagicLen, "070702")) {
                break; // not a header — end of archive (or trailing padding)
            }

            if (off + kCpioHeaderSize > size) break; // truncated header
            char hdr[kCpioHeaderSize];
            if (!r.readAt(off, hdr, kCpioHeaderSize)) break;

            const char* modeF   = hdr + 6 + 1 * kCpioFieldLen;  // mode
            const char* mtimeF  = hdr + 6 + 5 * kCpioFieldLen;  // mtime
            const char* sizeF   = hdr + 6 + 6 * kCpioFieldLen;  // filesize
            const char* nameSzF = hdr + 6 + 11 * kCpioFieldLen; // namesize

            const uint64_t mode     = CpioOctal(modeF, kCpioFieldLen);
            const uint64_t mtimeSec = CpioOctal(mtimeF, kCpioFieldLen);
            const uint64_t fileSize = CpioOctal(sizeF, kCpioFieldLen);
            const uint64_t nameSize = CpioOctal(nameSzF, kCpioFieldLen);

            // Guard the name length: must include at least the NUL and stay
            // within the file and a sane maximum.
            if (nameSize < 1 || nameSize > 4096) break;
            const uint64_t nameOff = off + kCpioHeaderSize;
            if (nameOff + nameSize > size) break;

            std::vector<char> nameBuf((size_t)nameSize + 1, '\0');
            if (!r.readAt(nameOff, nameBuf.data(), (size_t)nameSize)) break;
            size_t nameLen = 0;
            while (nameLen < (size_t)nameSize && nameBuf[nameLen] != '\0') ++nameLen;

            const std::wstring name = internal::decodeBytes(
                (const uint8_t*)nameBuf.data(), nameLen, true);

            // "TRAILER!!!" marks the end of the archive.
            if (nameLen == 10 && std::memcmp(nameBuf.data(), "TRAILER!!!", 10) == 0)
                break;

            const uint64_t dataOff = nameOff + CpioAlign4(nameSize);
            const uint64_t dataPadded = CpioAlign4(fileSize);
            const uint64_t nextOff = dataOff + dataPadded;
            if (nextOff > size) break; // data runs past EOF — stop

            ArchiveEntry ent;
            ent.path = name;
            ent.isDir = (mode & 0xF000) == 0x4000; // S_IFDIR
            ent.unpackedSize = ent.isDir ? 0 : fileSize;
            ent.packedSize = ent.unpackedSize; // CPIO stores uncompressed
            ent.method = 0;
            ent.methodName = L"Stored (uncompressed)";
            ent.mtime = internal::unixTimeToFT(mtimeSec);

            if (ent.isDir) info.dirCount++;
            else {
                info.fileCount++;
                info.totalPacked += ent.packedSize;
                info.totalUnpacked += ent.unpackedSize;
            }
            info.entries.push_back(std::move(ent));
            any = true;

            off = nextOff; // headers are not padded; next header follows data
        }

        if (!any && info.entries.empty()) {
            info.error = L"No CPIO entries found";
            break;
        }

        info.format = L"CPIO";
        info.algorithm = L"Stored (uncompressed)";
        info.archiveSize = size;
        info.perEntryCompression = false;
        ok = true;
    } while (false);

    CloseHandle(h);
    return ok;
}

} // namespace archdiview
