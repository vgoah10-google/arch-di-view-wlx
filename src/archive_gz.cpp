// archive_gz.cpp — Native GZip / TarGz parser (no external DLL).
//
// Parses the RFC 1952 gzip header, inflates the DEFLATE payload with our
// built-in inflate(), then:
//   * if the payload is a TAR stream (typical for .tgz / .tar.gz), lists the
//     inner entries and uses the compressed payload size as the packed total;
//   * otherwise treats it as a single-file gzip.
#include "archive.h"

#include <cstring>
#include <vector>

namespace archdiview {
namespace {

#pragma pack(push, 1)
struct GzHeader {
    uint8_t magic0;   // 0x1F
    uint8_t magic1;   // 0x8B
    uint8_t method;   // 8 = deflate
    uint8_t flags;    // FTEXT(1) FHCRC(2) FEXTRA(4) FNAME(8) FCOMMENT(16)
    uint32_t mtime;   // unix seconds
    uint8_t xfl;
    uint8_t os;
};
#pragma pack(pop)

constexpr uint8_t kFExtra   = 0x04;
constexpr uint8_t kFName    = 0x08;
constexpr uint8_t kFComment = 0x10;
constexpr uint8_t kFHCRC    = 0x02;

class MemReader {
public:
    MemReader(const uint8_t* d, size_t n) : d_(d), n_(n), p_(0) {}
    bool read(void* out, size_t len) {
        if (p_ + len > n_) return false;
        memcpy(out, d_ + p_, len);
        p_ += len;
        return true;
    }
    bool skip(size_t n) { if (p_ + n > n_) return false; p_ += n; return true; }
    // Consume a NUL-terminated string; returns the bytes consumed (incl. NUL).
    bool skipCString(size_t& consumed) {
        size_t start = p_;
        while (p_ < n_ && d_[p_] != '\0') ++p_;
        if (p_ >= n_) return false;
        ++p_;
        consumed = p_ - start;
        return true;
    }
    size_t pos() const { return p_; }
    const uint8_t* data() const { return d_; }

private:
    const uint8_t* d_;
    size_t n_, p_;
};

std::wstring BytesToWide(const uint8_t* b, size_t n) {
    if (n == 0) return std::wstring();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, (const char*)b, (int)n, nullptr, 0);
    if (wlen <= 0) {
        wlen = MultiByteToWideChar(CP_ACP, 0, (const char*)b, (int)n, nullptr, 0);
        std::wstring w(wlen, L'\0');
        MultiByteToWideChar(CP_ACP, 0, (const char*)b, (int)n, &w[0], wlen);
        return w;
    }
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, (const char*)b, (int)n, &w[0], wlen);
    return w;
}

// Does the inflated payload look like a TAR? Check the ustar magic at offset
// 257, or fall back to a plausible first-block checksum.
bool LooksLikeTar(const std::vector<uint8_t>& d) {
    if (d.size() < 512) return false;
    if (d.size() >= 262 && memcmp(&d[257], "ustar", 5) == 0) return true;
    unsigned stored = 0;
    for (int i = 148; i < 156 && i < 512; ++i)
        stored = stored * 8 + (unsigned)(d[i] - '0');
    unsigned sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? (unsigned)' ' : (unsigned)d[i];
    return stored != 0 && stored == sum;
}

} // namespace

bool TryParseGz(const std::wstring& path, ArchiveInfo& info) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        info.error = L"Cannot open file";
        return false;
    }
    LARGE_INTEGER szli{};
    GetFileSizeEx(h, &szli);
    uint64_t fileSize = (uint64_t)szli.QuadPart;

    // Read the whole file (we inflate the payload into memory regardless).
    std::vector<uint8_t> buf((size_t)fileSize);
    if (fileSize > 0) {
        uint64_t off = 0;
        while (off < fileSize) {
            DWORD want = (DWORD)std::min<uint64_t>(fileSize - off, 0x40000000u);
            DWORD got = 0;
            if (!ReadFile(h, buf.data() + off, want, &got, nullptr) || got != want) {
                CloseHandle(h);
                info.error = L"Cannot read file";
                return false;
            }
            off += got;
        }
    }
    CloseHandle(h);

    MemReader mr(buf.data(), buf.size());
    GzHeader gh{};
    if (!mr.read(&gh, sizeof(gh)) || gh.magic0 != 0x1F || gh.magic1 != 0x8B) {
        info.error = L"Not a gzip file";
        return false;
    }
    if (gh.method != 8) {
        info.error = L"Unsupported gzip compression method";
        return false;
    }

    // Optional header fields.
    std::vector<uint8_t> nameBytes;
    if (gh.flags & kFExtra) {
        uint16_t xlen = 0;
        if (!mr.read(&xlen, 2) || !mr.skip(xlen)) {
            info.error = L"Truncated gzip header"; return false;
        }
    }
    if (gh.flags & kFName) {
        size_t before = mr.pos();
        size_t consumed = 0;
        if (!mr.skipCString(consumed)) {
            info.error = L"Truncated gzip header"; return false;
        }
        nameBytes.assign(buf.data() + before, buf.data() + before + consumed - 1);
    }
    if (gh.flags & kFComment) {
        size_t consumed = 0;
        if (!mr.skipCString(consumed)) {
            info.error = L"Truncated gzip header"; return false;
        }
    }
    if (gh.flags & kFHCRC) {
        if (!mr.skip(2)) { info.error = L"Truncated gzip header"; return false; }
    }

    size_t payloadOffset = mr.pos();
    if (payloadOffset + 8 > buf.size()) {
        info.error = L"Truncated gzip payload";
        return false;
    }
    size_t payloadLen = buf.size() - payloadOffset - 8; // trailing CRC32 + ISIZE

    std::vector<uint8_t> inflated;
    if (!Inflate(buf.data() + payloadOffset, payloadLen, inflated)) {
        info.error = L"Corrupt DEFLATE stream";
        return false;
    }

    // gzip header stored the original file's unix mtime.
    FILETIME mt{};
    {
        ULARGE_INTEGER li{};
        li.QuadPart = (uint64_t)gh.mtime * 10000000ULL + 116444736000000000ULL;
        mt.dwLowDateTime = li.LowPart;
        mt.dwHighDateTime = li.HighPart;
    }

    info.archiveSize = fileSize;
    info.totalPacked = payloadLen; // the compressed gzip payload is the input

    if (LooksLikeTar(inflated)) {
        info.format = L"TarGz";
        info.algorithm = L"Deflate (gzip)";
        if (!TryParseTarMemory(inflated, info)) {
            info.error = L"Corrupt inner TAR stream";
            return false;
        }
        // WalkTar sums packed==unpacked per entry; reset packed to the real
        // compressed payload size so the ratio reflects gzip, not the tar.
        info.totalPacked = payloadLen;
        return true;
    }

    // Single-file gzip.
    info.format = L"GZip";
    info.algorithm = L"Deflate (gzip)";
    info.totalUnpacked = inflated.size();
    info.fileCount = 1;
    ArchiveEntry ent;
    ent.path = nameBytes.empty() ? std::wstring(L"(decompressed)") : BytesToWide(nameBytes.data(), nameBytes.size());
    ent.isDir = false;
    ent.packedSize = payloadLen;
    ent.unpackedSize = inflated.size();
    ent.mtime = mt;
    info.entries.push_back(std::move(ent));
    return true;
}

} // namespace archdiview
