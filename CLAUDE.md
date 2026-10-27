# Battlefield 3 (Xbox 360) static recomp

Goal (user, 2026-10-27): a functional decompilation of Battlefield 3 (Xbox 360) into native, typed C++ source,
rebuilt for Windows with no dependency on the emulated Xbox runtime; only the game discs required.
Behaviour-equivalent and verified by tests, not matching. Private proof of concept, no distribution: licensing is
not a concern (Havok and Scaleform are decompiled from the game's own statically linked copy).
- Phase 1 (done 2026-10-27): ReXGlue static recomp boots straight to the main menu, Metro TDM playable with native PC
  keyboard/mouse, HD Content pack loaded. It is now the scaffolding: the game must stay bootable at every step, native
  code replaces recompiled functions as overrides (define sub_X in project/src/, the original stays __imp__sub_X).
- v0.1 alpha: a fully decompiled, working Team Deathmatch on Operation Metro (MP_Subway). Nothing else (campaign,
  other maps, modes) until then. Scope = the functions Metro TDM executes (coverage hit set, docs/DECOMP.md).
- Later: possibly Rust or a performance rework of the C++ (network I/O, servers with hundreds of players), so
  decompiled code is idiomatic and refactorable, not a transcription of PowerPC.
- Leverage ReXGlue: refactor its generated C++, reuse its codegen analysis, extend it via patches/rexglue/.
Test runs: Claude may launch, kill and screenshot bf3.exe from C:\Games\BF3-recomp.

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
- `scripts/bf3dbg.py` client for the debug API in development builds (`project/src/dev/`): load Metro, deploy, read
  player/world state, inject input, screenshot. Tests go through it. Development builds also have an in-game debug
  menu (Insert: god mode, flight, teleport, weapons; F9 fps/network overlay; `project/src/dev/dev_menu.cpp`). None of
  it may ship: production builds use the `win-cross-ship` preset (BF3_DEV_TOOLS=OFF).
- Decompilation: `docs/DECOMP.md` (v0.1 workload, address map, reflection, harness and layout proposals, plan),
  `docs/decomp/v01_functions.tsv` (the Metro TDM hit set, classified), `scripts/decomp/` (coverage sessions, xrefs,
  inventory, reflection, typed header generator, lift, progress), `project/src/native/` (decompiled overrides).
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
- Use Ghidra for reverse engineering. If the Ghidra MCP connection is down, or the right program isn't open (project
  file C:\Users\Dustin\BF3-RECOMP.gpr, program /merged/BF.Main.xex; check with get_program_info), stop and tell the
  user right away; don't quietly work around it.
