#!/usr/bin/env bash
# Metro gameplay benchmark: PRACTICE -> Metro TDM, auto deploy (assault), walk/turn a little, then print the frame
# rate and audio lines logged while playing.
# Usage: scripts/metro_play.sh [seconds=170] [extra exe args...]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SECS=${1:-170}; shift $(( $# < 1 ? $# : 1 ))
export BF3_AUTO_DEPLOY="${BF3_AUTO_DEPLOY:-0}"
export SHOTS="${SHOTS:-15000}"
"$ROOT/scripts/practice_run.sh" "$SECS" info "$@"
grep -aE "frame rate: [0-9]|audio:|memexport readback:" "$ROOT/logs/win-run.log" | cut -c1-160 | tail -25
