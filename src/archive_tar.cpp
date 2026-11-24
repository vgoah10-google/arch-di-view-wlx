// archive_tar.cpp — Native TAR parser (no external DLL).
//
// Walks 512-byte header blocks, decoding the octal size, type flag and name
// (handling the GNU '././@LongLink' and POSIX 'K' prefix entry types).
// The core walker runs over a BlockSource so it can read from either a file
// handle (plain .tar) or an in-memory buffer (the payload inside a .tgz).
#include "archive.h"

#include <cstring>
#include <algorithm>

namespace archdiview {
namespace {

constexpr size_t kBlockSize = 512;

// Abstract byte source the TAR walker reads 512-byte blocks and seeks on.
struct BlockSource {
    virtual ~BlockSource() {}
    virtual bool readBlock(void* out) = 0;          // read one 512-byte block
    virtual bool skipBlocks(uint64_t n) = 0;        // advance n blocks
    virtual uint64_t offset() const = 0;
};

// Decode a TAR octal field (may be NUL/space terminated).
uint64_t ParseOctal(const char* field, size_t len) {
    uint64_t value = 0;
    size_t i = 0;
    while (i < len && (field[i] == ' ' || field[i] == '\0')) ++i;
    for (; i < len && field[i] >= '0' && field[i] <= '7'; ++i)
        value = value * 8 + (uint64_t)(field[i] - '0');
    return value;
}

// Decode a TAR binary (base-256) size field, used for files > 8 GB.
uint64_t ParseBinarySize(const char* field, size_t len) {
    uint64_t value = 0;
    for (size_t i = 1; i < len; ++i)
        value = (value << 8) | (uint8_t)field[i];
    return value;
}

uint64_t ParseSizeField(const char* field, size_t len) {
    if (len > 0 && (field[0] & 0x80))
        return ParseBinarySize(field, len);
    return ParseOctal(field, len);
}

// Decode a NUL-terminated / space-padded fixed field to a wide string.
std::wstring DecodeField(const char* field, size_t len) {
    size_t n = 0;
    while (n < len && field[n] != '\0') ++n;
    while (n > 0 && field[n - 1] == ' ') --n;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, field, (int)n, nullptr, 0);
    if (wlen <= 0) {
        wlen = MultiByteToWideChar(CP_ACP, 0, field, (int)n, nullptr, 0);
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, field, (int)n, &w[0], wlen);
        return w;
    }
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, field, (int)n, &w[0], wlen);
    return w;
}

FILETIME OctalTimeToFileTime(const char* field, size_t len) {
    uint64_t secs = ParseOctal(field, len);
    ULARGE_INTEGER li{};
    li.QuadPart = secs * 10000000ULL + 116444736000000000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = li.LowPart;
    ft.dwHighDateTime = li.HighPart;
    return ft;
}

// The shared TAR walker. Returns true when the stream parsed (even if empty).
bool WalkTar(BlockSource& src, uint64_t streamSize, ArchiveInfo& info) {
    std::vector<char> block(kBlockSize);
    std::wstring pendingLongName;

    while (src.offset() + kBlockSize <= streamSize) {
        if (!src.readBlock(block.data())) break;

        bool allZero = true;
        for (size_t i = 0; i < kBlockSize; ++i)
            if (block[i] != '\0') { allZero = false; break; }
        if (allZero) break; // end-of-archive marker

        char name[100];   memcpy(name,    &block[0],   100);
        char sizeF[12];   memcpy(sizeF,   &block[124],  12);
        char typeFlag = block[156];
        char prefix[155]; memcpy(prefix,  &block[345], 155);
        char mtimeF[12];  memcpy(mtimeF,  &block[136], 12);

        // Validate the header checksum to avoid desync on garbage.
        char chk[8]; memcpy(chk, &block[148], 8);
        unsigned stored = (unsigned)ParseOctal(chk, 8);
        unsigned sum = 0;
        for (size_t i = 0; i < kBlockSize; ++i)
            sum += (i >= 148 && i < 156) ? (unsigned)' ' : (unsigned)(uint8_t)block[i];
        if (stored != 0 && stored != sum)
            return false; // not a valid TAR header

        uint64_t dataSize = ParseSizeField(sizeF, 12);
        FILETIME mtime = OctalTimeToFileTime(mtimeF, 12);
        uint64_t dataBlocks = (dataSize + kBlockSize - 1) / kBlockSize;

        std::wstring entryName;
        if (!pendingLongName.empty()) {
            entryName = pendingLongName;
            pendingLongName.clear();
        } else {
            entryName = DecodeField(name, 100);
            std::wstring pfx = DecodeField(prefix, 155);
            if (!pfx.empty()) entryName = pfx + L"/" + entryName;
        }

        if (typeFlag == 'L') {
            // GNU long name: the entry's "data" is the next entry's real name.
            std::vector<char> nm((size_t)dataSize + 1, 0);
            uint64_t got = 0;
            while (got < dataSize) {
                if (!src.readBlock(block.data())) return false;
                uint64_t take = std::min<uint64_t>(kBlockSize, dataSize - got);
                memcpy(nm.data() + got, block.data(), (size_t)take);
                got += take;
            }
            pendingLongName = DecodeField(nm.data(), (size_t)dataSize);
            continue;
        }
        if (typeFlag == 'K') {
            // GNU long link target: skip its data.
            src.skipBlocks(dataBlocks);
            continue;
        }

        ArchiveEntry ent;
        ent.path = entryName;
        ent.isDir = (typeFlag == '5');
        ent.unpackedSize = ent.isDir ? 0 : dataSize;
        ent.packedSize = ent.unpackedSize; // uncompressed inside the tar stream
        ent.method = 0;
        ent.mtime = mtime;
        if (ent.isDir) info.dirCount++;
        else {
            info.fileCount++;
            info.totalPacked += ent.packedSize;
            info.totalUnpacked += ent.unpackedSize;
        }
        info.entries.push_back(std::move(ent));

        src.skipBlocks(dataBlocks);
    }
    return true;
}

// File-backed block source (plain .tar).
struct FileSource : BlockSource {
    HANDLE h;
    uint64_t off;
    FileSource(HANDLE fh, uint64_t start) : h(fh), off(start) {}
    bool readBlock(void* out) override {
        DWORD got = 0;
        if (!ReadFile(h, out, kBlockSize, &got, nullptr) || got != kBlockSize)
            return false;
        off += kBlockSize;
        return true;
    }
    bool skipBlocks(uint64_t n) override {
        LARGE_INTEGER sk{}; sk.QuadPart = (LONGLONG)(n * kBlockSize);
        if (SetFilePointerEx(h, sk, nullptr, FILE_CURRENT) == INVALID_SET_FILE_POINTER)
            return false;
        off += n * kBlockSize;
        return true;
    }
    uint64_t offset() const override { return off; }
};

// Memory-backed block source (inflated .tgz payload).
struct MemSource : BlockSource {
    const std::vector<uint8_t>& d;
    uint64_t off;
    MemSource(const std::vector<uint8_t>& data, uint64_t start) : d(data), off(start) {}
    bool readBlock(void* out) override {
        if (off + kBlockSize > d.size()) return false;
        memcpy(out, d.data() + off, kBlockSize);
        off += kBlockSize;
        return true;
    }
    bool skipBlocks(uint64_t n) override {
        off += n * kBlockSize;
        return true;
    }
    uint64_t offset() const override { return off; }
};

} // namespace

// Entry point: parse a plain .tar file.
bool TryParseTarFile(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    uint64_t size = (uint64_t)sz.QuadPart;
    LARGE_INTEGER zero{}; SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
    FileSource src(h, 0);
    bool ok = WalkTar(src, size, info);
    if (ok) {
        info.format = L"TAR";
        info.algorithm = L"Stored (uncompressed)";
        info.archiveSize = size;
    }
    CloseHandle(h);
    return ok;
}

// Entry point: parse a TAR stream already inflated into memory (from a .tgz).
bool TryParseTarMemory(const std::vector<uint8_t>& data, ArchiveInfo& info) {
    MemSource src(data, 0);
    return WalkTar(src, data.size(), info);
}

} // namespace archdiview
