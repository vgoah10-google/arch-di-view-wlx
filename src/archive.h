// archive.h — Archive metadata model + format parsers for ArchDiView.
//
// Each parser reads the archive's directory metadata only (no extraction) and
// fills an ArchiveInfo with per-entry sizes and the compression method. The UI
// layer renders this as a compression-ratio bar, a detail table and a tree.
#ifndef ARCHDIVIEW_ARCHIVE_H
#define ARCHDIVIEW_ARCHIVE_H

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace archdiview {

// One entry (file or folder) inside the archive.
struct ArchiveEntry {
    std::wstring path;            // full path inside archive, '/'-separated
    bool isDir = false;
    uint64_t packedSize = 0;      // compressed size on disk
    uint64_t unpackedSize = 0;    // original size
    uint32_t method = 0;          // format-specific raw method id (optional)
    std::wstring methodName;      // resolved method label, e.g. "Deflate"
    FILETIME mtime = {};
};

// Aggregate archive information.
struct ArchiveInfo {
    std::wstring format;          // "ZIP", "TAR", "GZip", "TarGz", ...
    std::wstring algorithm;       // dominant method, e.g. "Deflate"
    uint64_t archiveSize = 0;     // size of the .zip/.tar/... file itself
    uint64_t totalPacked = 0;     // sum of entry packed sizes
    uint64_t totalUnpacked = 0;   // sum of entry unpacked sizes
    uint32_t fileCount = 0;
    uint32_t dirCount = 0;
    // True when compression is per-entry (ZIP). False for archive-level
    // compression (gzip/tar), where a per-entry method label is meaningless.
    bool perEntryCompression = false;
    std::vector<ArchiveEntry> entries;
    std::wstring error;           // non-empty when parsing failed

    // Compression ratio = packed / unpacked (0 = no compression, 1 = none).
    double ratio() const {
        return totalUnpacked ? (double)totalPacked / (double)totalUnpacked : 0.0;
    }
    // Space saved = 1 - ratio.
    double saved() const { return 1.0 - ratio(); }
};

// Detect and parse the archive at `path`. Returns true on success.
// On failure returns false and sets info.error.
bool ParseArchive(const std::wstring& path, ArchiveInfo& info);

// ---- Internal parser entry points (used by the dispatcher) -----------------
// Each returns false and sets info.error on failure. Parsers fill `info`.
bool TryParseZip(const std::wstring& path, ArchiveInfo& info);
bool TryParseTarFile(const std::wstring& path, ArchiveInfo& info);
// Variant that parses a TAR stream starting at `baseOffset` within `h`
// (used for the TAR payload inside a .tgz). `size` is the stream length.
bool TryParseTar(HANDLE h, uint64_t size, uint64_t baseOffset, ArchiveInfo& info,
                 const wchar_t* formatLabel);
bool TryParseGz(const std::wstring& path, ArchiveInfo& info);
// Parse a TAR stream already inflated into memory (used for the .tgz payload).
bool TryParseTarMemory(const std::vector<uint8_t>& data, ArchiveInfo& info);

// Single-file/stream compression formats (one logical member, not a directory).
bool TryParseBZip2(const std::wstring& path, ArchiveInfo& info);
bool TryParseZ(const std::wstring& path, ArchiveInfo& info);
bool TryParsePPM(const std::wstring& path, ArchiveInfo& info);

// Classic per-file-header archives.
bool TryParseLha(const std::wstring& path, ArchiveInfo& info);
bool TryParseArcPak(const std::wstring& path, ArchiveInfo& info);
bool TryParseArj(const std::wstring& path, ArchiveInfo& info);
bool TryParseSqz(const std::wstring& path, ArchiveInfo& info);
bool TryParseZoo(const std::wstring& path, ArchiveInfo& info);
bool TryParseHa(const std::wstring& path, ArchiveInfo& info);
bool TryParseBh(const std::wstring& path, ArchiveInfo& info);
bool TryParseDwc(const std::wstring& path, ArchiveInfo& info);

// Container formats.
bool TryParseCab(const std::wstring& path, ArchiveInfo& info);
bool TryParseCpio(const std::wstring& path, ArchiveInfo& info);

// Modern formats (header/listing parsers, no external DLL needed).
bool TryParseRar(const std::wstring& path, ArchiveInfo& info);
bool TryParseSevenZip(const std::wstring& path, ArchiveInfo& info);
bool TryParseAce(const std::wstring& path, ArchiveInfo& info);

// DLL-backed parsers (delegate to the archiver's own DLL for full fidelity).
// These return false with info.error set when the DLL is unavailable, so the
// dispatcher can fall back to the native header parsers above.
bool TryParseSevenZipDll(const std::wstring& path, ArchiveInfo& info);
bool TryParseRarDll(const std::wstring& path, ArchiveInfo& info);

// Minimal raw-DEFLATE (RFC 1951) decompressor used to read .tgz / .gz.
// Returns false on malformed input. `out` is resized to the inflated size.
bool Inflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out);

// Map a ZIP compression method id to a human-readable name.
const wchar_t* ZipMethodName(uint32_t method);

// Format a byte count as a compact human string ("1.20 MB").
std::wstring FormatBytes(uint64_t bytes);

// Format a FILETIME as "YYYY-MM-DD HH:MM".
std::wstring FormatFileTime(const FILETIME& ft);

} // namespace archdiview

#endif // ARCHDIVIEW_ARCHIVE_H
