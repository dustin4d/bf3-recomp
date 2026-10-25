#!/usr/bin/env bash
# Copy the Windows build (bf3.exe + the DLLs built next to it) to the deploy folder on the host.
# Usage: scripts/win_deploy.sh
# Writes on Windows: only C:\Games\BF3-recomp, and only files that changed (see CLAUDE.md).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=$ROOT/project/out/build/win-cross-release
DEST=/mnt/c/Games/BF3-recomp

[[ -f $BUILD/bf3.exe ]] || { echo "no $BUILD/bf3.exe: build win-cross-release first" >&2; exit 1; }

# Never replace the exe under a session the user is running.
if tasklist.exe /FI "IMAGENAME eq bf3.exe" 2>/dev/null | grep -qi bf3.exe; then
  echo "bf3.exe is running on the host (user session?) - not deploying." >&2
  exit 2
fi

mkdir -p "$DEST"
copied=0
# Our patched GPU plugin (gpu_plugin/, patches/rexglue/) replaces the SDK's stock one staged by the build.
PATCHED_GPU=$ROOT/gpu_plugin/build/rexgpu-xenos.dll
for f in "$BUILD"/bf3.exe "$BUILD"/*.dll; do
  name=$(basename "$f")
  [[ $name == rexgpu-xenos.dll && -f $PATCHED_GPU ]] && f=$PATCHED_GPU
  if ! cmp -s "$f" "$DEST/$name"; then
    cp "$f" "$DEST/"
    echo "copied  $name ($(du -h "$f" | cut -f1))"
    copied=$((copied + 1))
  fi
done
echo "$copied file(s) updated in C:\\Games\\BF3-recomp"
