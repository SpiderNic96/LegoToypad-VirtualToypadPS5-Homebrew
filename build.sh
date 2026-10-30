#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
if ! python3 -c "import elftools" 2>/dev/null; then
  echo "Missing pyelftools: python3 -m pip install pyelftools" >&2
  exit 1
fi
make -C src/hook
make -C src/overlay
make -C src/injector
echo "Built build/ldinject.elf"
