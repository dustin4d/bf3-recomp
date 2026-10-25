#!/usr/bin/env bash
# Tile screenshots into one image for quick review: scripts/contact_sheet.sh <first index> <count> <out.png> [cols]
# Reads logs/shots/shot_NNN_*.png in order; each tile is 384 px wide with its index burned in (top left).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
first=$1 count=$2 out=$3 cols=${4:-5}
tmp=$(mktemp -d)
i=0
for f in $(ls "$ROOT"/logs/shots/shot_*.png | sort | tail -n +$((first + 1)) | head -n "$count"); do
  n=$(basename "$f" | sed -E 's/shot_([0-9]+)_.*/\1/')
  ffmpeg -loglevel error -y -i "$f" -vf "scale=384:-1,drawbox=x=0:y=0:w=56:h=24:color=black@0.8:t=fill,drawtext=text='$n':x=6:y=4:fontsize=18:fontcolor=yellow" "$tmp/$(printf '%04d' $i).png" 2>/dev/null \
    || ffmpeg -loglevel error -y -i "$f" -vf "scale=384:-1" "$tmp/$(printf '%04d' $i).png"
  i=$((i + 1))
done
rows=$(( (i + cols - 1) / cols ))
ffmpeg -loglevel error -y -framerate 1 -i "$tmp/%04d.png" -vf "tile=${cols}x${rows}" -frames:v 1 "$out"
rm -rf "$tmp"
echo "$out: $i tiles"
