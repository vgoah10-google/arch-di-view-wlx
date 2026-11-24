# ArchDiView — Total Commander Lister plugin for archive inspection

ArchDiView is a pure C++/Win32/GDI Lister (WLX) plugin for Total Commander that
shows detailed information about an archive and a visual tree of its contents.
**No WebView2, no external archiver DLLs** — everything is parsed natively.

![screenshot](https://img.shields.io/badge/formats-ZIP%20%7C%20TAR%20%7C%20GZ%2FTGZ-blue)

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

All parsing is done in-process by ArchDiView's own code (including a built-in
RFC 1951 DEFLATE decompressor for gzip). No `7z.dll`, `unrar.dll`, or other
external dependencies are required.

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
  inflate.cpp      Minimal RFC 1951 DEFLATE decompressor (for gzip)
  wlxplugin.h      WLX SDK interface definitions
```

## License

MIT.
