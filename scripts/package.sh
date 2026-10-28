#!/usr/bin/env bash
# Packages a built plugin in the layout the PlotJuggler Marketplace installs:
#   <id>-<version>-<os>-<arch>.zip
#     <id>/
#       libsegger_rtt_streamer.so
#       manifest.json
# and writes the SHA256 the registry entry needs next to it.
#
# Usage: scripts/package.sh <build-dir> <output-dir> [os] [arch]
set -euo pipefail

BUILD=${1:?usage: package.sh <build-dir> <output-dir> [os] [arch]}
OUT=${2:?usage: package.sh <build-dir> <output-dir> [os] [arch]}
OS=${3:-linux}
ARCH=${4:-$(uname -m)}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

read_manifest() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" "$ROOT/manifest.json" "$1"; }
ID=$(read_manifest id)
VERSION=$(read_manifest version)

case "$OS" in
  linux) LIB=libsegger_rtt_streamer.so ;;
  macos) LIB=libsegger_rtt_streamer.dylib ;;
  windows) LIB=segger_rtt_streamer.dll ;;
  *) echo "unknown os '$OS'" >&2; exit 1 ;;
esac
BIN=$(find "$BUILD" -maxdepth 2 -name "$LIB" | head -1)
[ -n "$BIN" ] || { echo "$LIB not found in $BUILD" >&2; exit 1; }

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/$ID" "$OUT"
cp "$BIN" "$ROOT/manifest.json" "$STAGE/$ID/"

ZIP="$ID-$VERSION-$OS-$ARCH.zip"
OUT=$(cd "$OUT" && pwd)
rm -f "$OUT/$ZIP"
(cd "$STAGE" && zip -q -r "$OUT/$ZIP" "$ID")
(cd "$OUT" && sha256sum "$ZIP" > "$ZIP.sha256")
echo "$OUT/$ZIP"
cat "$OUT/$ZIP.sha256"
