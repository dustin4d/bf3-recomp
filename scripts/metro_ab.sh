#!/usr/bin/env bash
# A/B a GPU option on Operation Metro: launches PRACTICE -> Metro TDM, waits at the deploy screen and copies the
# screenshot taken ~20 s after the spawn screen to logs/ab/<name>.png.
# Usage: scripts/metro_ab.sh <name> [extra exe args...]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
name=$1; shift
mkdir -p "$ROOT/logs/ab"
SHOTS=5000 \
  "$ROOT/scripts/practice_run.sh" "${AB_SECS:-130}" info "$@" > "$ROOT/logs/ab/$name.out" 2>&1
t=$(grep -a "push screen 'SpawnScreen'" "$ROOT/logs/win-run.log" | head -1 | cut -c13-20 | tr -d :)
if [[ -z $t ]]; then echo "$name: no spawn screen"; exit 1; fi
shot=$(ls "$ROOT/logs/shots" | awk -F_ -v t="$t" '{s=substr($3,1,6)} s>=t {print}' | sed -n 5p)
cp "$ROOT/logs/shots/$shot" "$ROOT/logs/ab/$name.png" && echo "$name: $shot"
