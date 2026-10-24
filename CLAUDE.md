# Battlefield 3 (Xbox 360) static recomp

Goal: a native Windows x64 build of Battlefield 3 (Xbox 360), statically recompiled with ReXGlue.
Spec ("done"): TBD, to be agreed with the user and recorded here.

## Environment
- Arch on WSL2. Codegen, analysis and compile checks run in WSL; boots and tests run on the Windows host.
- Ghidra runs on Windows; reach it through the `ghidra` MCP server (mirrored networking).
- Never build under /mnt/c. The only Windows-side location builds may be written to: TBD.

## Layout
- `dump/` original game files (never committed). Main executable: `dump/default.xex` (md5 TBD).
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
