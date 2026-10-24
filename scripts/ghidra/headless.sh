#!/usr/bin/env bash
# Run Windows headless Ghidra on the BF3 project from WSL.
#   scripts/ghidra/headless.sh <log-name> [/<folder>] <analyzeHeadless args...>
# Example:
#   scripts/ghidra/headless.sh import /modules -import "$(wslpath -w dump/...)" -noanalysis
# Project: C:\Users\Dustin\BF3-RECOMP (the only Windows-side write). The batch file and the log stay in
# logs/ and run over \\wsl.localhost. Paths to repo files must be Windows paths (wslpath -w).
# Other projects (e.g. one open in the Ghidra GUI) are unaffected; this project must not be open.
set -euo pipefail

GHIDRA='C:\Users\Dustin\Downloads\ghidra_12.1.4_PUBLIC_20260921\ghidra_12.1.4_PUBLIC'
JDK='C:\Program Files\Microsoft\jdk-21.0.12.101-hotspot'
PROJECT_DIR='C:\Users\Dustin'
PROJECT=BF3-RECOMP
MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-8G}"

repo="$(cd "$(dirname "$0")/../.." && pwd)"
name="${1:?usage: $0 <log-name> [/<folder>] <analyzeHeadless args...>}"; shift
folder=""
if [[ "${1:-}" == /* ]]; then folder="$1"; shift; fi

if [[ -e "/mnt/c/Users/Dustin/$PROJECT.lock" ]]; then
    echo "Project $PROJECT is open in Ghidra: close it first." >&2
    exit 1
fi

bat="$repo/logs/ghidra-$name.bat"
log="$repo/logs/ghidra-$name.log"
args=""
for a in "$@"; do args+=" \"$a\""; done
printf '%s\r\n' '@echo off' \
    "set \"JAVA_HOME=${JDK}\"" \
    "set \"GHIDRA_HEADLESS_MAXMEM=${MAXMEM}\"" \
    "call \"${GHIDRA}\\support\\analyzeHeadless.bat\" \"${PROJECT_DIR}\" \"${PROJECT}${folder}\" -scriptPath \"$(wslpath -w "$repo/scripts/ghidra")\"${args}" \
    > "$bat"

cd /mnt/c/Users/Dustin
cmd.exe /c "$(wslpath -w "$bat")" < /dev/null > "$log" 2>&1 || true
tr -d '\r' < "$log" > "$log.tmp" && mv "$log.tmp" "$log"
grep -E 'ERROR|Exception|REPORT|IMPORTING|Import succeeded|SCRIPT|> |ANALYZING|Analysis' "$log" | tail -40 || true
grep -qE 'ERROR|Exception' "$log" && exit 1 || exit 0
