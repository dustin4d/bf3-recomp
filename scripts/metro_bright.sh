#!/usr/bin/env bash
# Metro test run at a well-lit spot: PRACTICE -> Metro TDM as RUSSIAN ARMY, auto deploy, then walk forward 5 s from
# the spawn to the opening onto the daylight park (pale concrete floor: good for viewmodel/body checks). The default
# US spawn is in a dark room. Mouse look tests (BF3_MOUSE_FEED) should start with a ~4 s pause for the walk, e.g.
# BF3_MOUSE_FEED="0,0,4000;1500,0,8000".
# Usage: scripts/metro_bright.sh [seconds=110] [extra exe args...]
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export BF3_MOUSE_SCRIPT="${BF3_MOUSE_SCRIPT:-wait:MainMenuScreen;3000:move:0.13,0.65;800:click;4000:move:0.21,0.218;800:click;2500:move:0.47,0.258;800:click}"
export BF3_PAD_SCRIPT="${BF3_PAD_SCRIPT:-wait:CrosshairDefault#1;2500:LY=32767:5000}"
export BF3_AUTO_DEPLOY="${BF3_AUTO_DEPLOY:-1}"
exec "$ROOT/scripts/metro_play.sh" "${1:-110}" "${@:2}"
