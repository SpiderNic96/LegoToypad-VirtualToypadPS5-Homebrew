#!/usr/bin/env bash
# Host test for the PS4 GoldHEN plugin glue (Linux, gcc with ASan/UBSan).
# Builds ps4/source/main.c + the hook module + the web remote against a synthetic eboot image
# and stubbed GoldHEN/system calls, then checks every patch, the GOT redirection and the
# watchdog. Writes to /data/ldmod like the console does: run it in a container or VM.
#   GOLDHEN_SDK=/path/to/GoldHEN_Plugins_SDK tools/ps4-host-test/run.sh [0=US|1=EU] [seconds to serve :8765]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"; repo="$here/../.."
out="$(mktemp -d)"
F="-g -O1 -DLD_PS4_PLUGIN -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-unused-parameter"
G="${GOLDHEN_SDK:?set GOLDHEN_SDK}/include"
gcc $F -I"$G" -include stdint.h -c "$here/harness.c" -o "$out/harness.o"
gcc $F -DLD_INJECTOR -DVTP_HOST -I"$G" -include stdint.h -I"$repo/src/core" -c "$repo/ps4/source/main.c" -o "$out/main.o"
gcc $F -DLD_INJECTOR -I"$repo/src/core" -c "$repo/src/injector/webremote.c" -o "$out/webremote.o"
for f in src/hook/hooks.c src/hook/menu.c src/core/ldproto.c; do
  gcc $F -I"$repo/src/core" -c "$repo/$f" -o "$out/$(basename "$f" .c).o"
done
gcc $F "$out"/*.o -o "$out/harness" -lpthread -lm
mkdir -p /data/ldmod
ASAN_OPTIONS=detect_leaks=0 "$out/harness" "${1:-0}" ${2:-}
