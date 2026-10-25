#!/usr/bin/env bash
# Deploy the Windows build and run it once on the host. Game data: C:\Games\BF3-recomp\game (NTFS: the runtime
# lowercases guest paths, so it cannot read the case-sensitive WSL share). Logs and user data stay in WSL.
# Usage: scripts/win_run.sh [seconds=45] [log_level=debug] [extra exe args...]
# Env: SHOTS=<interval_ms> also captures the game window every interval to logs/shots/ (PNG).
# Writes on Windows: only bf3.exe + required DLLs in C:\Games\BF3-recomp (see CLAUDE.md).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST=/mnt/c/Games/BF3-recomp
UNC="\\\\wsl.localhost\\${WSL_DISTRO_NAME:-archlinux}$(tr / '\\' <<<"$ROOT")"
SECS=${1:-45}
LEVEL=${2:-debug}
shift $(( $# < 2 ? $# : 2 ))

"$ROOT/scripts/win_deploy.sh" || exit $?

mkdir -p "$ROOT/logs/win-userdata"
rm -f "$ROOT/logs/win-run.log"
if [[ -n "${SHOTS:-}" ]]; then
  rm -rf "$ROOT/logs/shots" && mkdir -p "$ROOT/logs/shots"
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$UNC\\scripts\\win_screenshot.ps1" \
    -OutDir "$UNC\\logs\\shots" -IntervalMs "$SHOTS" -Count $(( SECS * 1000 / SHOTS )) -StartDelayMs 3000 \
    > "$ROOT/logs/shots.stdout" 2>&1 &
fi
cd "$DEST" || exit 1
# Forward BF3_* environment variables (e.g. BF3_JOB_SKIP_NULL=0) to the Windows process.
for v in $(compgen -e | grep '^BF3_' || true); do WSLENV="${WSLENV:+$WSLENV:}$v"; done
export WSLENV
timeout "$SECS" ./bf3.exe "--game_data_root=C:\\Games\\BF3-recomp\\game" "--log_file=$UNC\\logs\\win-run.log" \
  "--user_data_root=$UNC\\logs\\win-userdata" "--log_level=$LEVEL" "--log_flush_interval=1" "$@" \
  > /dev/null 2>&1
echo "exit code: $?  (124 = still running at timeout)"
taskkill.exe /IM bf3.exe /F > /dev/null 2>&1
for _ in 1 2 3 4 5 6 7 8 9 10; do  # let it finish exiting before the next run's check
  tasklist.exe /FI "IMAGENAME eq bf3.exe" 2>/dev/null | grep -qi bf3.exe || break
  sleep 0.5
done
wait

cd "$ROOT" || exit 1
grep -aE '\[(critical|error)\]' logs/win-run.log | head -8
if grep -aq crash_trace logs/win-run.log; then
  python -I scripts/resolve_rva.py project/out/build/win-cross-release logs/win-run.log | cut -c1-220
fi
