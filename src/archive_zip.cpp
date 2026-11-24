// archive_zip.cpp — Native ZIP central-directory parser (no external DLL).
//
// Reads the End Of Central Directory record (with ZIP64 support), then walks
// the central directory to collect per-entry packed/unpacked sizes, the
// compression method and timestamps. Only metadata is read; nothing is
// extracted. Approach follows ArchView's TZipJar.cpp.
#include "archive.h"

#include <vector>
#include <cstring>
#include <algorithm>

namespace archdiview {
namespace {

#pragma pack(push, 1)
struct EOCD {                 // End Of Central Directory (0x06054b50)
    uint32_t sig;
    uint16_t diskNum;
    uint16_t cdDisk;
    uint16_t cdEntriesThisDisk;
    uint16_t cdTotalEntries;
    uint32_t cdSize;
    uint32_t cdOffset;
    uint16_t commentLen;
};

struct Zip64EOCD {            // ZIP64 End Of Central Directory (0x06064b50)
    uint32_t sig;
    uint64_t eocdSize;
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint32_t diskNum;
    uint32_t cdDisk;
    uint64_t cdEntriesThisDisk;
    uint64_t cdTotalEntries;
    uint64_t cdSize;
    uint64_t cdOffset;
};

struct Zip64EOCDLocator {     // (0x07064b50)
    uint32_t sig;
    uint32_t cdDisk;
    uint64_t cdOffset;
    uint32_t totalDisks;
};

struct CentralHeader {        // Central directory file header (0x02014b50)
    uint32_t sig;
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint16_t flags;
    uint16_t method;
    uint16_t modTime;
    uint16_t modDate;
    uint32_t crc32;
    uint32_t packedSize;
    uint32_t unpackedSize;
    uint16_t nameLen;
    uint16_t extraLen;
    uint16_t commentLen;
    uint16_t diskStart;
    uint16_t internalAttr;
    uint32_t externalAttr;
    uint32_t localHeaderOffset;
};
#pragma pack(pop)

// A simple buffered reader over a file HANDLE with position tracking.
class FileReader {
public:
    explicit FileReader(HANDLE h) : h_(h) {
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(h_, &sz)) size_ = (uint64_t)sz.QuadPart;
    }
    uint64_t size() const { return size_; }

    bool seek(uint64_t pos) {
        LARGE_INTEGER li{};
        li.QuadPart = (LONGLONG)pos;
        return SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER;
    }
    bool read(void* buf, size_t len) {
        DWORD got = 0;
        // Read in chunks to stay within DWORD limits.
        uint8_t* p = (uint8_t*)buf;
        while (len > 0) {
            DWORD want = (DWORD)std::min<size_t>(len, 0x40000000u);
            if (!ReadFile(h_, p, want, &got, nullptr) || got != want)
                return false;
            p += got; len -= got;
        }
        return true;
    }
    bool readAt(uint64_t pos, void* buf, size_t len) {
        return seek(pos) && read(buf, len);
    }
private:
    HANDLE h_;
    uint64_t size_ = 0;
};

// DOS date/time (as stored in ZIP) -> FILETIME (local, converted to UTC-ish).
FILETIME DosDateTimeToFileTime(uint16_t dosDate, uint16_t dosTime) {
    SYSTEMTIME st{};
    st.wYear   = 1980 + ((dosDate >> 9) & 0x7F);
    st.wMonth  = (dosDate >> 5) & 0x0F;
    st.wDay    = dosDate & 0x1F;
    st.wHour   = (dosTime >> 11) & 0x1F;
    st.wMinute = (dosTime >> 5) & 0x3F;
    st.wSecond = (dosTime & 0x1F) * 2;
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    return ft;
}

// Convert a ZIP entry filename to a wide string. Respects the UTF-8 flag
// (general purpose bit 11); otherwise assumes CP437/ANSI fallback to ACP.
std::wstring DecodeName(const uint8_t* data, size_t len, bool utf8) {
    if (utf8) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)data, (int)len, nullptr, 0);
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, (const char*)data, (int)len, &w[0], wlen);
        return w;
    }
    int wlen = MultiByteToWideChar(CP_ACP, 0, (const char*)data, (int)len, nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, (const char*)data, (int)len, &w[0], wlen);
    return w;
}

} // namespace

const wchar_t* ZipMethodName(uint32_t method) {
    switch (method) {
        case 0:  return L"Stored";
        case 1:  return L"Shrunk";
        case 2:  return L"Reduce 1";
        case 3:  return L"Reduce 2";
        case 4:  return L"Reduce 3";
        case 5:  return L"Reduce 4";
        case 6:  return L"Implode";
        case 7:  return L"Tokenize";
        case 8:  return L"Deflate";
        case 9:  return L"Deflate64";
        case 10: return L"PKWARE DCL";
        case 11: return L"BZip2";
        case 12: return L"BZip2";
        case 14: return L"LZMA";
        case 19: return L"LZ77";
        case 33: return L"LZMA2";
        case 93: return L"Zstandard";
        case 94: return L"MP3";
        case 95: return L"XZ";
        case 96: return L"JPEG";
        case 97: return L"WavPack";
        case 98: return L"PPMd";
        case 99: return L"AES encrypted";
        default: return L"Unknown";
    }
}

namespace {

bool ParseZip(FileReader& fr, ArchiveInfo& info) {
    info.format = L"ZIP";
    info.perEntryCompression = true;

    // 1. Locate EOCD by scanning the last 64 KB + sizeof(EOCD) for the signature.
    const uint32_t kEOCDSig = 0x06054b50;
    uint64_t scanStart = 0;
    if (fr.size() > 65536 + sizeof(EOCD))
        scanStart = fr.size() - (65536 + sizeof(EOCD));
    std::vector<uint8_t> tail((size_t)(fr.size() - scanStart));
    if (!fr.readAt(scanStart, tail.data(), tail.size())) {
        info.error = L"Cannot read archive tail";
        return false;
    }
    int64_t eocdRel = -1;
    for (int64_t i = (int64_t)tail.size() - sizeof(EOCD); i >= 0; --i) {
        uint32_t sig;
        memcpy(&sig, &tail[i], 4);
        if (sig == kEOCDSig) { eocdRel = i; break; }
    }
    if (eocdRel < 0) {
        info.error = L"End Of Central Directory not found";
        return false;
    }
    EOCD eocd{};
    memcpy(&eocd, &tail[eocdRel], sizeof(eocd));

    uint64_t cdOffset = eocd.cdOffset;
    uint64_t cdSize   = eocd.cdSize;
    uint64_t entries  = eocd.cdTotalEntries;

    // 2. ZIP64: if any field is at its 32-bit limit, read the ZIP64 EOCD.
    if (entries == 0xFFFF || cdOffset == 0xFFFFFFFF || cdSize == 0xFFFFFFFF) {
        // Locator sits immediately before the EOCD.
        uint64_t locPos = scanStart + (uint64_t)eocdRel - sizeof(Zip64EOCDLocator);
        Zip64EOCDLocator loc{};
        if (fr.readAt(locPos, &loc, sizeof(loc)) && loc.sig == 0x07064b50) {
            Zip64EOCD z64{};
            if (fr.readAt(loc.cdOffset, &z64, sizeof(z64)) && z64.sig == 0x06064b50) {
                entries  = z64.cdTotalEntries;
                cdSize   = z64.cdSize;
                cdOffset = z64.cdOffset;
            }
        }
    }

    // 3. Read the whole central directory into memory and walk it.
    if (cdSize == 0 || cdSize > fr.size()) {
        info.error = L"Invalid central directory size";
        return false;
    }
    std::vector<uint8_t> cd((size_t)cdSize);
    if (!fr.readAt(cdOffset, cd.data(), cd.data() ? cd.size() : 0)) {
        info.error = L"Cannot read central directory";
        return false;
    }

    size_t pos = 0;
    uint64_t maxPacked = 0;
    std::vector<uint32_t> methodCounts; // indexed by method id (sparse, small)
    for (uint64_t e = 0; e < entries && pos + sizeof(CentralHeader) <= cd.size(); ++e) {
        CentralHeader ch{};
        memcpy(&ch, &cd.data()[pos], sizeof(ch));
        if (ch.sig != 0x02014b50) break;
        pos += sizeof(ch);

        if (pos + ch.nameLen > cd.size()) break;
        bool utf8 = (ch.flags & 0x0800) != 0;
        std::wstring name = DecodeName(&cd.data()[pos], ch.nameLen, utf8);
        pos += ch.nameLen;

        // Parse the extra field for ZIP64 sizes when the 32-bit fields saturate.
        uint64_t packed = ch.packedSize, unpacked = ch.unpackedSize;
        size_t extraEnd = pos + ch.extraLen;
        if (extraEnd > cd.size()) extraEnd = cd.size();
        size_t epos = pos;
        while (epos + 4 <= extraEnd) {
            uint16_t hid, hsize;
            memcpy(&hid,   &cd.data()[epos], 2);
            memcpy(&hsize, &cd.data()[epos + 2], 2);
            epos += 4;
            if (hid == 0x0001 && epos + hsize <= extraEnd) {
                // ZIP64 extended information: sizes appear in a fixed order but
                // only the ones that were 0xFFFFFFFF in the header are present.
                size_t need = 0;
                if (ch.unpackedSize == 0xFFFFFFFF) need += 8;
                if (ch.packedSize   == 0xFFFFFFFF) need += 8;
                if (need && epos + need <= extraEnd) {
                    if (ch.unpackedSize == 0xFFFFFFFF) {
                        memcpy(&unpacked, &cd.data()[epos], 8); epos += 8;
                    }
                    if (ch.packedSize == 0xFFFFFFFF) {
                        memcpy(&packed, &cd.data()[epos], 8); epos += 8;
                    }
                }
            }
            epos += hsize;
        }
        pos = extraEnd;
        // Skip the file comment.
        pos += ch.commentLen;

        ArchiveEntry ent;
        // Directories in ZIP end with '/' (or carry the directory attribute).
        bool trailingSlash = !name.empty() && name.back() == L'/';
        bool dirAttr = (ch.externalAttr & 0x10) != 0;
        ent.isDir = trailingSlash || dirAttr;
        if (trailingSlash) name.pop_back();
        ent.path = name;
        ent.packedSize   = packed;
        ent.unpackedSize = unpacked;
        ent.method       = ch.method;
        ent.mtime        = DosDateTimeToFileTime(ch.modDate, ch.modTime);

        if (ent.isDir) {
            info.dirCount++;
        } else {
            info.fileCount++;
            info.totalPacked   += packed;
            info.totalUnpacked += unpacked;
            if (packed > maxPacked) maxPacked = packed;
        }
        // Track the dominant method across files.
        if (!ent.isDir) {
            if (methodCounts.size() <= ch.method)
                methodCounts.resize(ch.method + 1, 0);
            methodCounts[ch.method]++;
        }
        info.entries.push_back(std::move(ent));
    }

    // Determine the dominant compression algorithm across all files.
    if (!methodCounts.empty()) {
        uint32_t best = 0, bestCount = 0;
        for (uint32_t m = 0; m < methodCounts.size(); ++m)
            if (methodCounts[m] > bestCount) { bestCount = methodCounts[m]; best = m; }
        info.algorithm = ZipMethodName(best);
    }
    info.archiveSize = fr.size();
    return true;
}

} // namespace

// Entry point used by the format dispatcher in archive.cpp.
bool TryParseZip(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    FileReader fr(h);
    bool ok = ParseZip(fr, info);
    CloseHandle(h);
    return ok;
}

} // namespace archdiview
