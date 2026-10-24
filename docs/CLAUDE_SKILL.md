# Skill: Battlefield 3 ReXGlue Recomp

**Purpose:** Assist with static recompilation of Battlefield 3 (Xbox 360, title ID TBD) using ReXGlue, Ghidra,
and GhidraMCP.

## Environment
- Working dir: `/home/dustin/workspace/bf3-recomp`
- XEX: `dump/default.xex` (md5 TBD, same file loaded in Ghidra)
- Project: `project/` (manifest: `project/bf3_manifest.toml`)
- SDK: `tools/rexglue-sdk` (v0.10.0, linux-amd64), not installed yet
- GhidraMCP: reachable from WSL (mirrored networking)

## Commands
None verified yet for this project. `docs/reference/trialshd_CLAUDE_SKILL.md` has the forms that worked for
Trials HD on the same SDK version; re-verify each before recording it here.

## Rules
- Never distribute or request game assets.
- Prefer minimal, reversible changes (manifest entries, stubs) over rewrites. Never hand-edit `generated/`.
- Always cite the guest address when proposing a fix, and log it below.
- Query GhidraMCP for specific addresses or functions; never dump the whole binary.

## Fix log
| Address | Fix | Reason |
|---|---|---|

## Known notes
