# Battlefield 3 (Xbox 360) static recomp

Goal: a native Windows x64 build of Battlefield 3 (Xbox 360), statically recompiled with ReXGlue.
Spec, milestone 1 (user, 2026-10-24): boot to the main menu, then working keyboard/mouse input in the menus.
Campaign (Disc 2) and gameplay come later. Test runs: Claude may launch, kill and screenshot bf3.exe from
C:\Games\BF3-recomp.

## Environment
- Arch on WSL2. Codegen, analysis and compile checks run in WSL; boots and tests run on the Windows host.
- Ghidra runs on Windows; reach it through the `ghidra` MCP server (mirrored networking).
- Never build under /mnt/c. Builds deploy to C:\Games\BF3-recomp (exe + DLLs only); the Ghidra project is
  C:\Users\Dustin\BF3-RECOMP. Nothing else is written on Windows.

## Layout
- `scripts/rex_extract/` C++ tool on ReXGlue's runtime: extracts disc images and STFS packages, and turns an XEX
  into its flat PE image. `scripts/xdvdfs.py` is an independent Python disc lister for cross-checks.
- `dump/` original game files (never committed). Full disc ISOs in `dump/Disc 1/` (MP/Co-Op) and `dump/Disc 2/`
  (Campaign); full extracted game in `dump/Disc N/game/` (launcher `Default.xex` + DLL modules `BF.Main.xex`,
  `BF.Splash.xex`, `Engine.BuildInfo.xex`), flat images `dump/Disc N/*.image.bin`, HD Content pack in
  `dump/Disc 1/hd_content/`. Code is identical on both discs; Disc 1 is canonical. Details in docs/CLAUDE_SKILL.md.
- `project/` ReXGlue project: manifest, `generated/` (never hand-edited), `src/` (our hooks).
- `tools/` SDKs and sysroots (not committed, except `tools/cross/`). `scripts/` helpers. `logs/` all logs.
- `docs/CLAUDE_SKILL.md` verified commands, manifest syntax, fix log, findings.
- `docs/reference/trialshd_CLAUDE_SKILL.md` notes from the Trials HD port (same toolchain); reference only,
  its addresses do not apply here.
- `ENVIRONMENT.md` setup steps and first-run checklist.

## Build
See `docs/CLAUDE_SKILL.md` once commands are verified for this project. Redirect long output into `logs/`.

## Rules
- Never commit, copy elsewhere, or distribute game files or the original executable.
- Never hand-edit `project/generated/`; change the manifest or override functions in `project/src/`.
- Cite the guest address for every fix and add it to the fix log.
- Query Ghidra for specific addresses or functions; never dump the whole binary.
