#!/usr/bin/env bash
# Host tests: the console picker overlay has a way into ShellUI on every firmware from 7.00 to 13.60.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
c++ -std=c++20 -O1 -Wall -I../src/overlay/common test_fw.cpp -o "$out/test_fw"
"$out/test_fw"
echo "all host tests passed"
