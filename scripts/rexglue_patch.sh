#!/usr/bin/env bash
# Write the changes in tools/rexglue-src that aren't in patches/rexglue/*.patch yet to a new patch file.
# Usage: scripts/rexglue_patch.sh <name>   ->  patches/rexglue/NNNN-<name>.patch
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/tools/rexglue-src"
WT="$(mktemp -d)"
git -C "$SRC" worktree add --detach "$WT" bd833a2a > /dev/null 2>&1
(
  cd "$WT"
  for p in "$ROOT"/patches/rexglue/*.patch; do git apply "$p"; done
  git add -A > /dev/null && git -c user.name=patch -c user.email=patch@localhost commit -qm applied
  for f in $(git -C "$SRC" diff --name-only; git -C "$SRC" ls-files --others --exclude-standard); do
    mkdir -p "$(dirname "$f")"; cp "$SRC/$f" "$f"
  done
  git add -A > /dev/null
  n=$(ls "$ROOT"/patches/rexglue/*.patch | wc -l)
  out="$ROOT/patches/rexglue/$(printf '%04d' $((n + 1)))-$1.patch"
  git diff --cached > "$out"
  echo "$out: $(grep -c '^+' "$out") added lines"
)
git -C "$SRC" worktree remove --force "$WT"
