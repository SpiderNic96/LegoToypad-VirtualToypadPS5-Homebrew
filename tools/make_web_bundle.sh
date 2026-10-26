#!/usr/bin/env bash
# Build the /data/ldmod/web folder (phone remote + console overlay art) from a LegoToypad checkout.
#   usage: tools/make_web_bundle.sh <path to harrysof/LegoToypad checkout> [output dir, default build/web]
# Needs: python3 with pillow, numpy, fonttools, brotli; a host C compiler (gcc or cc).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(dirname "$here")"
ltp="$(cd "$1" && pwd)"
out="${2:-$repo/build/web}"
work="$repo/build/bundle-work"
rm -rf "$work"; mkdir -p "$work"
python3 "$here/bundle/patch_gen.py" "$ltp/generate_assets.py" "$work/ltp_generate.py"
cp "$here/build_bundle.py" "$work/"
${CC:-cc} -O2 -I"$repo/src/core" -o "$work/figid" "$here/bundle/figid.c" "$repo/src/core/ldproto.c"
python3 "$work/build_bundle.py" "$ltp" "$out" "$work/figid"
python3 "$here/build_ui_pack.py" "$ltp" "$out"
echo "web folder ready: $out  (copy it to /data/ldmod/web on the PS5)"
