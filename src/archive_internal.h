// archive_internal.h — Shared helpers for the format parsers.
//
// Provides a buffered file reader and little-endian field decoders so each
// parser can be written against plain byte layouts (matching ArchView's
// decomposed BYTE/WORD/DWORD headers) without repeating boilerplate.
#ifndef ARCHDIVIEW_ARCHIVE_INTERNAL_H
#define ARCHDIVIEW_ARCHIVE_INTERNAL_H

#include "archive.h"

#include <algorithm>
#include <cstring>
#include <cstdint>

namespace archdiview {
namespace internal {

// Buffered file reader with position tracking and sized reads.
class FileReader {
public:
    explicit FileReader(HANDLE h) : h_(h) {
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(h_, &sz)) size_ = (uint64_t)sz.QuadPart;
    }
    uint64_t size() const { return size_; }
    uint64_t pos() const { return pos_; }

    bool seek(uint64_t p) {
        LARGE_INTEGER li{}; li.QuadPart = (LONGLONG)p;
        if (SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
            return false;
        pos_ = p;
        return true;
    }
    bool skip(int64_t delta) {
        if (delta >= 0) return seek(pos_ + (uint64_t)delta);
        return seek(pos_ - (uint64_t)(-delta));
    }
    bool read(void* buf, size_t len) {
        DWORD got = 0;
        uint8_t* p = (uint8_t*)buf;
        while (len > 0) {
            DWORD want = (DWORD)std::min<size_t>(len, 0x40000000u);
            if (!ReadFile(h_, p, want, &got, nullptr) || got != want)
                return false;
            p += got; len -= got; pos_ += got;
        }
        return true;
    }
    bool readAt(uint64_t p, void* buf, size_t len) { return seek(p) && read(buf, len); }

private:
    HANDLE h_;
    uint64_t size_ = 0, pos_ = 0;
};

// ---- Little-endian field decoders (ArchView decomposes these into BYTEs) ----
inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint64_t rd64(const uint8_t* p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

// Decode a fixed NUL/space-padded ASCII field to a wide string.
inline std::wstring decodeAscii(const char* f, size_t len) {
    size_t n = 0;
    while (n < len && f[n] != '\0') ++n;
    while (n > 0 && (f[n - 1] == ' ' || f[n - 1] == '\0')) --n;
    int wl = MultiByteToWideChar(CP_UTF8, 0, f, (int)n, nullptr, 0);
    if (wl <= 0) {
        wl = MultiByteToWideChar(CP_ACP, 0, f, (int)n, nullptr, 0);
        std::wstring w(wl, L'\0');
        MultiByteToWideChar(CP_ACP, 0, f, (int)n, &w[0], wl);
        return w;
    }
    std::wstring w(wl, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, f, (int)n, &w[0], wl);
    return w;
}

// Decode a length-prefixed raw byte name (UTF-8 first, then ACP fallback).
inline std::wstring decodeBytes(const uint8_t* d, size_t len, bool utf8) {
    UINT cp = utf8 ? CP_UTF8 : CP_ACP;
    int wl = MultiByteToWideChar(cp, 0, (const char*)d, (int)len, nullptr, 0);
    std::wstring w(std::max(wl, 0), L'\0');
    if (wl > 0) MultiByteToWideChar(cp, 0, (const char*)d, (int)len, &w[0], wl);
    return w;
}

// DOS date/time (word date, word time) -> FILETIME.
inline FILETIME dosTimeToFT(uint16_t d, uint16_t t) {
    SYSTEMTIME st{};
    st.wYear = 1980 + ((d >> 9) & 0x7F);
    st.wMonth = (d >> 5) & 0x0F;
    st.wDay = d & 0x1F;
    st.wHour = (t >> 11) & 0x1F;
    st.wMinute = (t >> 5) & 0x3F;
    st.wSecond = (t & 0x1F) * 2;
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    return ft;
}

// Unix seconds -> FILETIME.
inline FILETIME unixTimeToFT(uint64_t secs) {
    ULARGE_INTEGER li{};
    li.QuadPart = secs * 10000000ULL + 116444736000000000ULL;
    FILETIME ft{};
    ft.dwLowDateTime = li.LowPart;
    ft.dwHighDateTime = li.HighPart;
    return ft;
}

// Does the buffer begin with the given ASCII signature?
inline bool hasSig(const uint8_t* d, size_t n, const char* sig) {
    size_t l = std::strlen(sig);
    if (n < l) return false;
    return std::memcmp(d, sig, l) == 0;
}

} // namespace internal
} // namespace archdiview

#endif // ARCHDIVIEW_ARCHIVE_INTERNAL_H
