#!/usr/bin/env bash
# Host tests: the console picker overlay has a way into ShellUI on every firmware from 7.00 to 13.60,
# and the resolution safety net steps down after a start that never reached the toy pad.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
c++ -std=c++20 -O1 -Wall -I../src/overlay/common test_fw.cpp -o "$out/test_fw"
# the resolution safety net, taken straight from main.c
awk '/^#define RES_TRIAL/{f=1} f{print} f&&/^    return want;/{getline; print; exit}' ../src/injector/main.c > "$out/res.inc"
cc -O1 -Wall -DRES_INC="\"$out/res.inc\"" test_resolution.c -o "$out/test_res"
"$out/test_res"
"$out/test_fw"
echo "all host tests passed"
