#!/usr/bin/env bash
# make_package.sh — Build both architectures and produce a Total Commander
# auto-install ZIP (pluginst.inf at the archive root, pure ASCII / LF / no BOM,
# which is the encoding TC recognizes).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
VERSION="${1:-1.0}"

# 1. Build both architectures (skips configure if already configured).
./build.sh >/dev/null

# 2. Stage files.
STAGE="$ROOT/build/package"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp build/release/Release/ArchDiView.wlx64  "$STAGE/"
cp build/release32/Release/ArchDiView.wlx  "$STAGE/"
cp README.md "$STAGE/"

# 3. pluginst.inf — pure ASCII, LF line endings, NO BOM (what TC recognizes).
printf '[plugininstall]\ndescription=ArchDiView - archive info: compression ratio, details and contents tree\ntype=wlx\nfile=ArchDiView.wlx\ndefaultdir=ArchDiView\n' \
    > "$STAGE/pluginst.inf"

# Verify encoding (fail the build if it regresses).
node -e '
const fs=require("fs");
const b=fs.readFileSync(process.argv[1]);
const bad = b[0]===0xEF || b.includes(13) || !/^[ -~\n]*$/.test(b.toString("latin1"));
if(bad){ console.error("ERROR: pluginst.inf must be pure ASCII, LF-only, no BOM"); process.exit(1); }
console.log("pluginst.inf OK: ASCII/LF/no-BOM, "+b.length+" bytes");
' "$STAGE/pluginst.inf"

# 4. Zip (pluginst.inf at the ROOT for TC auto-install).
mkdir -p dist
OUT="dist/ArchDiView_TC_${VERSION}.zip"
rm -f "$OUT"
ABS_OUT="$ROOT/$OUT"
( cd "$STAGE" && zip -r "$ABS_OUT" . >/dev/null )
echo "==> Created $OUT"
unzip -l "$OUT" | sed -n '4,$p' | tr -d '\000' | head -8
