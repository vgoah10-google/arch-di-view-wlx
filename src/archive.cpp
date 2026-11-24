// archive.cpp — Format detection dispatcher + shared helpers.
#include "archive.h"

#include <algorithm>
#include <cstring>

namespace archdiview {
namespace {

// Read the first `n` bytes of a file for magic probing.
bool ReadHead(const std::wstring& path, uint8_t* buf, size_t n, size_t& got) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD r = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)n, &r, nullptr);
    CloseHandle(h);
    got = ok ? (size_t)r : 0;
    return ok;
}

} // namespace

bool ParseArchive(const std::wstring& path, ArchiveInfo& info) {
    info = ArchiveInfo{};

    // Probe by magic first (extension is only a hint).
    uint8_t head[8] = {0};
    size_t got = 0;
    ReadHead(path, head, sizeof(head), got);

    if (got >= 4 && head[0] == 'P' && head[1] == 'K') {
        // "PK\003\004" (local header) or "PK\005\006" (empty) or "PK\007\010".
        return TryParseZip(path, info);
    }
    if (got >= 2 && head[0] == 0x1F && head[1] == 0x8B) {
        return TryParseGz(path, info);
    }

    // TAR has no magic at offset 0; its signature "ustar" lives at offset 257.
    // Try the TAR parser and let its checksum validation decide.
    {
        ArchiveInfo tarInfo;
        if (TryParseTarFile(path, tarInfo) && (tarInfo.fileCount || tarInfo.dirCount)) {
            info = std::move(tarInfo);
            return true;
        }
        info = ArchiveInfo{};
    }

    info.error = L"Unsupported or unrecognized archive format";
    return false;
}

std::wstring FormatBytes(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = (double)bytes;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; ++u; }
    wchar_t buf[64];
    if (u == 0)
        swprintf(buf, 64, L"%llu B", (unsigned long long)bytes);
    else if (v >= 100.0)
        swprintf(buf, 64, L"%.1f %s", v, units[u]);
    else
        swprintf(buf, 64, L"%.2f %s", v, units[u]);
    return buf;
}

std::wstring FormatFileTime(const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0)
        return std::wstring();
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u-%02u-%02u %02u:%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

} // namespace archdiview
