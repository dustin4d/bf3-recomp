#!/usr/bin/env bash
# Print the guest PowerPC disassembly of recompiled functions, taken from the instruction comments codegen
# writes into the generated C++ (exact, unlike the decompiler around ldarx/stdcx.).
# Usage: scripts/guest_asm.sh <addr> [<addr>...]     e.g. scripts/guest_asm.sh 8346BAE0
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for a in "$@"; do
  a=$(tr '[:lower:]' '[:upper:]' <<<"${a#0x}")
  f=$(grep -l "DEFINE_REX_FUNC(sub_$a)" "$ROOT"/project/generated/*/*.cpp | head -1 || true)
  if [[ -z $f ]]; then echo "===== sub_$a: not found"; continue; fi
  echo "===== sub_$a ($(basename "$(dirname "$f")")/$(basename "$f"))"
  awk -v F="DEFINE_REX_FUNC(sub_$a)" 'index($0, F){p=1} p&&(/^\t\/\/ /||/^loc_/){print} p&&/^}/{exit}' "$f" \
    | sed 's/^\t\/\/ /  /'
done
