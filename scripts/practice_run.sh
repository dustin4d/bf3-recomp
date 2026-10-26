#!/usr/bin/env bash
# PRACTICE -> OPERATION METRO test run: main menu (skip_title boots straight to it), PRACTICE, the Metro TDM row, US MARINES.
# Usage: scripts/practice_run.sh [seconds=150] [log_level=info] [extra exe args...]  (SHOTS defaults to 10000)
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# With --skip_title=false the title screen needs START: BF3_PAD_SCRIPT="wait:StartScreen#2;3000:START;3000:START".
export BF3_PAD_SCRIPT="${BF3_PAD_SCRIPT:-}"
export BF3_MOUSE_SCRIPT="${BF3_MOUSE_SCRIPT:-wait:MainMenuScreen;3000:move:0.13,0.65;800:click;4000:move:0.21,0.218;800:click;2500:move:0.47,0.218;800:click}"
export SHOTS="${SHOTS:-10000}"
SECS=${1:-150}; LEVEL=${2:-info}; shift $(( $# < 2 ? $# : 2 ))
exec "$ROOT/scripts/win_run.sh" "$SECS" "$LEVEL" "$@"
