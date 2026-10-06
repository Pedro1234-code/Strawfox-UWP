#!/bin/bash
# Fetch the windows crate (windows-rs 0.62.2) that the engine build takes from
# MOZ_WINDOWS_RS_DIR, check it against the crates.io index, and patch it for
# 32-bit ARM Windows with tools/patch-windows-rs-arm32.py. The result lands in
# engine/third_party/windows-0.62.2 (not tracked: it is generated).
#
# Run once after cloning. It does nothing if the crate is already there.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=0.62.2
SHA256=527fadee13e0c05939a6a05d5bd6eec6cd2e3dbd648b9f8e447c6518133d8580
DEST="$ROOT/engine/third_party"
CRATE="$DEST/windows-$VERSION.crate"

if [ -d "$DEST/windows-$VERSION/src" ]; then
  echo "windows-$VERSION is already in engine/third_party"
  exit 0
fi

mkdir -p "$DEST"
if [ ! -f "$CRATE" ]; then
  curl -sSfL -o "$CRATE" "https://static.crates.io/crates/windows/windows-$VERSION.crate"
fi
# Python rather than sha256sum and tar: MozillaBuild's tar forks gzip, and a
# fork between two msys runtimes (MozillaBuild's and Git Bash's) can fail.
python - "$CRATE" "$DEST" "$SHA256" <<'PY'
import hashlib, sys, tarfile
crate, dest, want = sys.argv[1:4]
got = hashlib.sha256(open(crate, "rb").read()).hexdigest()
if got != want:
    sys.exit(f"{crate}: sha256 {got}, expected {want}")
with tarfile.open(crate, "r:gz") as t:
    try:
        t.extractall(dest, filter="data")
    except TypeError:  # Python older than 3.12
        t.extractall(dest)
PY
if [ "${GECKO_W10M_ARCH:-x64}" = arm ]; then
  python "$ROOT/tools/patch-windows-rs-arm32.py" "$DEST/windows-$VERSION/src"
  echo "windows-$VERSION fetched and patched for ARM32 into engine/third_party"
else
  echo "windows-$VERSION fetched for x64 into engine/third_party"
fi
