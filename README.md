# ArchDiView — Total Commander Lister plugin for archive inspection

ArchDiView is a pure C++/Win32/GDI Lister (WLX) plugin for Total Commander that
shows detailed information about an archive and a visual tree of its contents.
**No WebView2** — everything is rendered with native GDI. ZIP/TAR and ~17 other
formats are parsed natively; **7z and RAR delegate to the archiver's own DLLs**
(`TC7Z64.DLL`/`TC7Z.DLL` and `UNRAR64.DLL`/`UNRAR.DLL`, both shipped with Total
Commander) for full fidelity, falling back to built-in native parsers when those
DLLs are unavailable.

![screenshot](https://img.shields.io/badge/formats-ZIP%20%7C%20TAR%20%7C%207Z%20%7C%20RAR%20%7C%20CAB%20%7C%20LHA-blue)

## What it shows

When you open a supported archive in the Lister (F3 / Ctrl+Q), ArchDiView renders
three things top-to-bottom:

1. **A horizontal compression-ratio bar** — a gradient bar showing packed vs
   unpacked size, with the ratio and space saved as a percentage.
2. **A detail table** — format, compression algorithm, file count, folder count,
   unpacked size and packed size.
3. **A vector tree diagram of the contents** (like `ouch -t` / the `tree`
   command) — every file and folder drawn with box-drawing connectors, each with
   its packed size, unpacked size and (for ZIP) the per-entry compression method.
   The tree scrolls when it is taller than the window.

Both the Lister **light and dark** themes are supported.

## Supported formats

| Format | Extension(s) | Notes |
|--------|--------------|-------|
| ZIP    | `.zip`, `.jar` | Full central-directory parse, per-entry methods, ZIP64 |
| TAR    | `.tar` | GNU/POSIX long-name support |
| GZip   | `.gz` | Single-file gzip |
| TarGz  | `.tgz`, `.tar.gz` | Inflates the gzip payload and lists the inner TAR |
| BZip2  | `.bz2`, `.tbz2` | Single-file bzip2 |
| Z      | `.z` | Unix `compress` (LZW) |
| PPM    | `.ppm` | PPM (PPMd) single-file |
| 7z     | `.7z` | Parsed via **`TC7Z64.DLL`/`TC7Z.DLL`** (`IInArchive` COM) — full support including compressed/encoded headers and accurate per-file LZMA method labels. Falls back to a native plain-header parser if the DLL is missing. |
| RAR    | `.rar` | RAR4 + RAR5 parsed via **`UNRAR64.DLL`/`UNRAR.DLL`** (listing) — full fidelity incl. Unicode names and NTFS timestamps. Falls back to a native RAR4 header parser if the DLL is missing. |
| CAB    | `.cab` | Microsoft Cabinet |
| CPIO   | `.cpio` | "new ASCII" (`070701`/`070702`) format |
| ARJ    | `.arj` | |
| LHA/LZH| `.lzh`, `.lha` | Levels 0/1/2 headers |
| ARC/PAK| `.arc`, `.pak` | |
| SQZ    | `.sqz` | |
| ZOO    | `.zoo` | |
| HA     | `.ha` | |
| BH     | `.bh` | Black Hole |
| DWC    | `.dwc` | Directory-at-end layout |
| ACE    | `.ace` | Header parse (listing only) |

All parsing is done in-process by ArchDiView's own code (including a built-in
RFC 1951 DEFLATE decompressor for gzip) — except **7z and RAR**, which delegate
to Total Commander's own `TC7Z64.DLL`/`TC7Z.DLL` and `UNRAR64.DLL`/`UNRAR.DLL`
for full fidelity (compressed 7z headers, RAR5, Unicode names, NTFS times). No
extra downloads are needed: those DLLs ship with Total Commander. When they are
unavailable the plugin transparently falls back to its built-in native header
parsers (plain-header 7z, RAR4 listing). Formats marked "listing only" (RAR,
ACE) parse the directory headers to show names, sizes and counts but do not
extract. Detection is primarily by magic bytes, so mis-named files still open.

## Building

Requirements: CMake 3.20+, Visual Studio 2022 (MSVC). No third-party libraries.

```bash
./build.sh            # builds both 64-bit and 32-bit
```

Outputs:
- `build/release/Release/ArchDiView.wlx64`  (64-bit)
- `build/release32/Release/ArchDiView.wlx`  (32-bit)

There are two test harnesses (built by default):
- `test_parse.exe <archive> ...` — console dump of the parsed info + ASCII tree.
- `test_capture.exe <archive> <out.bmp> [w] [h] [dark]` — renders the plugin
  window and captures it to a BMP (useful for visual verification).

## Installing into Total Commander

Either:
- Copy `ArchDiView.wlx64` (and/or `ArchDiView.wlx`) into a `Plugins\Lister\`
  subfolder and add it under Configuration → Options → Plugins → Listers, or
- Use the auto-install ZIP (`make_package.sh`), which TC can install directly.

## Source layout

```
src/
  plugin.cpp       WLX entry points (ListLoad/ListCloseWindow/...)
  view.h/.cpp      GDI renderer: ratio bar, detail table, vector tree
  tree.h/.cpp      Builds the file/folder hierarchy from flat entries
  archive.h/.cpp   Format detection dispatcher + size/time helpers
  archive_zip.cpp  Native ZIP central-directory parser (ZIP64 aware)
  archive_tar.cpp  Native TAR parser (file-backed + memory-backed)
  archive_gz.cpp   Native GZip / TarGz parser
  archive_modern.cpp Native RAR/7z/ACE header parsers (fallback path)
  archive_dll.cpp   DLL-backed 7z (IInArchive COM) + RAR (unrar.dll) parsers
  sevenzip_com.h    Minimal self-contained 7z IInArchive COM declarations
  inflate.cpp      Minimal RFC 1951 DEFLATE decompressor (for gzip)
  wlxplugin.h      WLX SDK interface definitions
```

## License

MIT.
