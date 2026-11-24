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

namespace {

// Run a parser; accept its result only if it produced at least one entry.
// This lets ambiguous formats (no clean offset-0 magic) be tried in order and
// the first one that yields a plausible listing wins.
using ParseFn = bool (*)(const std::wstring&, ArchiveInfo&);
bool TryAccept(const std::wstring& path, ArchiveInfo& info, ParseFn fn) {
    ArchiveInfo tmp;
    if (fn(path, tmp) && (tmp.fileCount || tmp.dirCount)) {
        info = std::move(tmp);
        return true;
    }
    return false;
}

} // namespace

bool ParseArchive(const std::wstring& path, ArchiveInfo& info) {
    info = ArchiveInfo{};

    // Probe by magic first (extension is only a hint).
    uint8_t head[32] = {0};
    size_t got = 0;
    ReadHead(path, head, sizeof(head), got);

    // ---- Strong offset-0 signatures -------------------------------------
    if (got >= 4 && head[0] == 'P' && head[1] == 'K') {
        // "PK\003\004" (local header) or "PK\005\006" (empty) or "PK\007\010".
        return TryParseZip(path, info);
    }
    if (got >= 2 && head[0] == 0x1F && head[1] == 0x8B) {
        return TryParseGz(path, info);
    }
    if (got >= 3 && head[0] == 'B' && head[1] == 'Z' && head[2] == 'h') {
        return TryParseBZip2(path, info);
    }
    if (got >= 2 && head[0] == 0x1F && (head[1] == 0x9D || head[1] == 0xA0)) {
        // compress (.Z, LZW) — 0x9D; also LZH-in-Z (0xA0) treated the same.
        return TryParseZ(path, info);
    }
    if (got >= 6 && head[0] == 0x37 && head[1] == 0x7A && head[2] == 0xBC &&
        head[3] == 0xAF && head[4] == 0x27 && head[5] == 0x1C) {
        // 7-Zip: "7z\xBC\xAF'\x1C". Prefer the 7z DLL (handles compressed
        // headers); fall back to the native plain-header parser.
        if (TryParseSevenZipDll(path, info)) return true;
        if (info.error != L"7-Zip DLL not available") return false; // DLL present but failed
        return TryParseSevenZip(path, info);
    }
    if (got >= 7 && head[0] == 'R' && head[1] == 'a' && head[2] == 'r' &&
        head[3] == '!' && head[4] == 0x1A && head[5] == 0x07) {
        // RAR4 (\x00) and RAR5 (\x01\x00) share the "Rar!\x1A\x07" prefix.
        // Prefer the unrar DLL (full RAR5 support); fall back to native RAR4.
        if (TryParseRarDll(path, info)) return true;
        if (info.error != L"unrar DLL not available") return false; // DLL present but failed
        return TryParseRar(path, info);
    }
    if (got >= 4 && head[0] == 'M' && head[1] == 'S' && head[2] == 'C' &&
        head[3] == 'F') {
        // Microsoft CAB: "MSCF".
        return TryParseCab(path, info);
    }
    if (got >= 6 && head[0] == '0' && head[1] == '7' && head[2] == '0' &&
        head[3] == '7' && head[4] == '0' && (head[5] == '1' || head[5] == '2')) {
        // CPIO "new ASCII" magic: "070701" / "070702".
        return TryParseCpio(path, info);
    }
    if (got >= 2 && head[0] == 0x60 && head[1] == 0xEA) {
        // ARJ main-header id: 0x60 0xEA.
        return TryParseArj(path, info);
    }
    if (got >= 3 && head[0] == 'S' && head[1] == 'Q' && head[2] == 'Z') {
        return TryParseSqz(path, info);
    }
    if (got >= 4 && head[0] == 'B' && head[1] == 'H' && head[2] == 0x05 &&
        head[3] == 0x07) {
        // Black Hole: "BH\x05\x07".
        return TryParseBh(path, info);
    }
    if (got >= 6 && head[0] == 0x07 && head[1] == '*' && head[2] == 'A' &&
        head[3] == 'C' && head[4] == 'E' && head[5] == '*') {
        // ACE main header marker: 0x07 "*ACE*".
        return TryParseAce(path, info);
    }

    // ---- Formats with magic not at offset 0, or no clean magic ----------
    // Probe by extension to order the ambiguous attempts cheaply, but fall
    // through and try parsers regardless so mis-named files still open.

    // LHA/LZH: the method string ("-lh5-", "-lz4-", "-pm0-", ...) sits at
    // offset 2..6.
    if (got >= 7 && head[2] == '-' && head[6] == '-') {
        if (TryParseLha(path, info)) return true;
    }

    // ZOO: a 20-byte text banner then the id 0xFDC4A7DC at offset 20.
    if (got >= 24) {
        uint32_t zooId = (uint32_t)head[20] | ((uint32_t)head[21] << 8) |
                         ((uint32_t)head[22] << 16) | ((uint32_t)head[23] << 24);
        if (zooId == 0xFDC4A7DCu) {
            if (TryParseZoo(path, info)) return true;
        }
    }

    // HA: "HA" magic (id 0x4841) then a WORD file count.
    if (got >= 4 && head[0] == 'H' && head[1] == 'A') {
        if (TryParseHa(path, info)) return true;
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

    // ARC / PAK: no fixed offset-0 magic; the first byte is an archive/block
    // marker. Try the parser and accept it if it yields entries.
    if (got >= 1 && head[0] == 0x1A) {
        if (TryParseArcPak(path, info)) return true;
    }

    // DWC stores its directory at the END of the file ("DWC" end record), so
    // it has no offset-0 magic. Try it as a last resort.
    if (TryParseDwc(path, info)) return true;

    info = ArchiveInfo{};
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
