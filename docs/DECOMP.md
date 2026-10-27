# BF3 decompilation: v0.1 workload, foundations, plan

Goal and scope: CLAUDE.md. v0.1 = a fully decompiled, working Team Deathmatch on Operation Metro. This document
measures what that takes (the functions Metro TDM executes), records what the binary tells us about them, and lays
out the tooling and order of work. Numbers are from 2026-10-27.

Files:
- `docs/decomp/v01_functions.tsv`: every function the recorded sessions executed (module, address, size, blocks, name,
  subsystem group, confidence, deciding signal, runs/total runs it showed up in, first phase, number of phases).
- `docs/decomp/v01_summary.json`: totals per group.
- Raw data (not committed): `logs/coverage/<run>/NN-<phase>.tsv` hit sets per phase, `typeinfo.bin` (the runtime
  typeinfo section), `logs/decomp/xrefs_<module>.json`, `logs/decomp/reflection.json`, `logs/decomp/bands_64k.txt`.

## 1. Coverage builds

Patch 0008 adds the ReXGlue codegen option `coverage` (manifest, per module). Every generated function starts with
`REX_COVERAGE_HIT(<guest address>)`: one byte per 4-byte slot of the module's code range, read and set to 1 on the
first call (exact, every call; no sampling). It compiles to nothing unless the build defines `REX_COVERAGE`, which
CMake sets only with `BF3_DEV_TOOLS` and `BF3_COVERAGE` (default ON in development builds, never in `win-cross-ship`:
verified, no `REX_COVERAGE` in its build.ninja). Codegen also writes `generated/<module>/bf3_functions.tsv`
(address, size, blocks, name, kind) from its own analysis, which the inventory uses for function boundaries.

Cost (Metro, sunny_street spot, same view, 10 s of frames): 86 fps / 9.69 ms median without coverage; a plain
store per call 62 fps (cache-line ping-pong: hot functions run on all job threads and every store re-dirties the
line); check-before-store 74 fps / 10.47 ms median (+8% frame time). Turn it off (`-DBF3_COVERAGE=OFF`) for
performance work.

Use: debug API `coverage [stats] | dump [reset] | reset`, `bf3dbg.py coverage dump [file] [reset]`;
`scripts/decomp/coverage_session.py <run> [--teams ru,us] [--kits 0,1,2,3]` launches the game and walks a whole
session, dumping (and clearing) the hit set at each phase: boot (launch to main menu), load (Metro TDM to the spawn
screen), then per team and kit: deploy, move (walk, strafe, sprint, jump, crouch, prone, look), weapons (fire, aim and
fire, fire mode, light, reload, empty magazine and reload, pistol), gadgets (both gadget slots, grenade, knife,
spot, scoreboard), damage (a 14 m fall), death (kill) and respawn on the next kit; then round end and leaving to the
menu. A phase with under 1000 functions stops the session (the game stalled).

## 2. The v0.1 workload

Hit set: **41271 functions, 7957 KB of guest code** (44% of the 93787 functions, 48% of the 16465 KB in the three modules), union of 5 recorded sessions.

| Group | Functions | KB | high / medium / low confidence |
|---|---:|---:|---|
| frostbite-core | 16634 | 2322 | 2943 / 4296 / 9395 |
| render | 3128 | 1153 | 749 / 2031 / 348 |
| ui-scaleform | 4067 | 998 | 3256 / 517 / 294 |
| game | 4662 | 776 | 1822 / 1499 / 1341 |
| animation | 3565 | 754 | 196 / 3018 / 351 |
| havok | 2534 | 650 | 2230 / 105 / 199 |
| audio | 1397 | 325 | 697 / 634 / 66 |
| crt-libs | 1284 | 317 | 1027 / 71 / 186 |
| physics | 1283 | 244 | 446 / 653 / 184 |
| online | 1342 | 210 | 786 / 298 / 258 |
| network | 1020 | 155 | 218 / 530 / 272 |
| launcher | 139 | 29 | 139 / 0 / 0 |
| ai | 216 | 24 | 0 / 143 / 73 |
| **total** | **41271** | **7957** | |

Sessions: run4 is complete (boot, Metro TDM, RU then US, all four kits each, round-end attempt, back to the menu);
run1 (to the US assault gadgets), run2 (to the engineer gadgets, then a GPU hang), run3 (US first, kits in reverse,
to the support gadgets) and run4a (to the engineer weapons) ended early (dev tool crashes and the hang, see below)
and add their phases to the union. Excluded: Splash (skipped by the launcher hook) and code the hooks replace without
calling the original. `frostbite-core` is also the catch-all: of its 16634 functions 9395 are low confidence (only
the call graph places them), so read that part as "not yet attributed" rather than core.

Confidence: high = a class (Frostbite getType vtable, reflected create function or a constructor storing a named
vtable), referenced strings, or a library block; medium = a subsystem block or agreeing address neighbours; low =
call-graph majority or the nearest labeled neighbour. Treat the medium/low part of a group as an estimate: the
group totals are good to roughly +-20%, the library blocks (Havok, Scaleform, CRT/Lua, Blaze) are firm.

New functions per phase (first complete run, in session order):

`boot +27861`, `load +5677`, `deploy-assault +4254`, `move-assault +202`, `weapons-assault +347`, `gadgets-assault +644`, `damage-assault +20`, `death-assault +651`, `deploy-engineer +74`, `move-engineer +338`, `weapons-engineer +11`, `gadgets-engineer +8`, `damage-engineer +0`, `death-engineer +0`, `deploy-support +8`, `move-support +9`, `weapons-support +27`, `gadgets-support +56`, `damage-support +0`, `death-support +1`, `deploy-recon +8`, `move-recon +1`, `weapons-recon +0`, `gadgets-recon +40`, `damage-recon +0`, `death-recon +0`, `team-us +22`, `deploy-assault +10`, `move-assault +4`, `weapons-assault +0`, `gadgets-assault +0`, `damage-assault +0`, `death-assault +2`, `deploy-engineer +0`, `move-engineer +0`, `weapons-engineer +0`, `gadgets-engineer +0`, `damage-engineer +0`, `death-engineer +0`, `deploy-support +2`, `move-support +0`, `weapons-support +0`, `gadgets-support +0`, `damage-support +0`, `death-support +0`, `deploy-recon +0`, `move-recon +16`, `weapons-recon +0`, `gadgets-recon +0`, `damage-recon +0`, `death-recon +0`, `round-end +14`, `leave +506`
Boot alone (launch to the main menu: engine, front end, UI, profile) is two thirds of the set; the Metro load and the
first deploy add most of the rest. Later kits, the second team and repeated actions add little, so the set is close
to saturated for what the script exercises.

Rare paths: 618 functions (108 KB) showed up in only one of the 5 sessions; by first phase: leave 499, gadgets-recon 47, gadgets-engineer 22, move-recon 13, gadgets-support 13, gadgets-assault 8, move-engineer 4, damage-assault 3; by group: frostbite-core 191, render 103, havok 82, game 75, animation 62, physics 61.
499 of those are the leave phase, which only run4 reached, so they aren't rare; the rest (about 120) are gadget and
movement paths that vary with timing or the kit order. Not covered yet: the round end (TDM ends at the kill goal; a
solo player can't score a kill, and lowering the goal at ServerKillCounterEntity +0xA8 to 1 plus a suicide added
only 14 functions, so the end-of-round screens never ran), vehicles (none on Metro TDM), revive/defibrillator on
another player, squad and spotting feedback from other players, and the in-game menu's own "quit" path (the session
leaves through a level switch to the front end). These need a second player (a bot or a second client) and should be
recorded before v0.1 is declared complete.

### Address map of BF.Main code (from the strings each 64 KB band references)

| Range | Contents | Group |
|---|---|---|
| 0x82FC0000-0x83060000 | Frostbite core: VFS, JSON, job scheduler (EA::Jobs), memory, Lua context | frostbite-core |
| 0x83060000-0x830A0000 | ANT job glue, entity updaters, sequences | mixed |
| 0x830A0000-0x830F0000 | Frostbite Havok integration, character physics | physics |
| 0x830F0000-0x83130000 | Frostbite sound (reverb, mixers, voice over) | audio |
| 0x83130000-0x831B0000 | textures, shaders, mesh streaming, post-process; zlib inflate at 0x83180000 | render (+ zlib) |
| 0x831B0000-0x831C0000 | superbundle layout, REFPACK decompression | frostbite-core |
| 0x831C0000-0x83300000 | client game: level, players, soldiers, cameras, aiming; UISystem at 0x83280000 | game, ui |
| 0x83300000-0x83370000 | EA rw::audio (RenderWare Audio), VP6 video decoder | audio |
| 0x83370000-0x833A0000 | AI behaviours | ai |
| 0x833A0000-0x83460000 | server game: level, players, soldiers, spawning, ghosts | game |
| 0x83460000-0x83470000 | job manager threads | frostbite-core |
| 0x83470000-0x834C0000 | AI pathfinding | ai |
| 0x834C0000-0x83520000 | vegetation, debris, emitters, terrain streaming | render |
| 0x83520000-0x83540000 | AI bots | ai |
| 0x83540000-0x83680000 | EA ANT animation | animation |
| 0x83680000-0x83880000 | BF3 ("Venice") game code: weapons, gadgets, scoring, UI data, Battlelog | game, ui, online |
| 0x83880000-0x838B0000 | terrain rendering | render |
| 0x838B0000-0x838F0000 | MSVC CRT, XDK libraries, XML parser | crt-libs |
| 0x838F0000-0x83920000 | Lua 5.1 + tolua | crt-libs |
| 0x83920000-0x83930000 | EA BugSentry, libjpeg | crt-libs |
| 0x83930000-0x83A80000 | Scaleform GFx 3.3.90 | ui-scaleform |
| 0x83A80000-0x83C40000 | Havok 2010.2.0-r1: base, Physics, Destruction (hkd), Scene (hkx) | havok |
| 0x83C40000-0x83CE0000 | Frostbite network (ghosts, GhostManager), UI Scaleform renderer, spatial queries | network, ui |
| 0x83CE0000-0x83D40000 | world/entity rendering, sky, lens flares, shadows | render |
| 0x83D40000-0x83DF0000 | EA DirtySDK (HTTP, sockets), Blaze SDK, EASTL | online |
| 0x83E10000-0x83E70000 | Havok utilities (shape cutter, scene, geometry, debug display) | havok |
| 0x83ECEE00-0x83FE6A44 | section `ctr`: getType() stubs and constructors of reflected types (~10k hit) | per class |

Library versions are useful: Havok 2010.2 and Scaleform 3.3 class layouts are known from their SDKs, so those
blocks can be typed from headers instead of by reverse engineering, and EASTL is open source.

## 3. Reflection data

BF.Main carries Frostbite 2's type reflection (`scripts/decomp/reflection.py`, `logs/decomp/reflection.json`):
- Section `typeinfo` (0x84155000): static type records, each `{char* name, u16 flags, u16 size, module*, u8 align,
  u8 field count, ...}`, followed by zeroed space for the runtime TypeInfo objects that static init points at them
  (object +0 -> record; `scripts/decomp/coverage_session.py` saves the live section as `typeinfo.bin`). Basic type
  = `(flags >> 4) & 0x1F`: 2 ValueType, 3 Class, 4 Array, 7 CString, 8 Enum, 10 Boolean, 11-18 Int8..Uint64, 19
  Float32, 20 Float64, 21 Guid. Classes continue `{super TypeInfo*, create function, field array}`, value types
  `{default instance, field array}`, enums `{value array}`, arrays `{element TypeInfo*}`.
- Section `fieldinf` (0x84198A00): 12-byte field records `{char* name, u16 field flags | u16 offset, TypeInfo* type}`
  (enum values: `{name, 0, value}`).
- Contents: 6318 records: 2413 classes (1321 with fields: assets, entity data, settings; 1092 without, among them
  the runtime classes such as ServerSoldierEntity 0x4F0, ClientSoldierEntity 0x390, ClientSoldierWeapon 0x174, known by
  name, size and super class), 1018 value types (structs, messages; 480 with fields), 417 enums (3737 values), 2466
  array types; 9761 fields with name, offset and type. 3869 TypeInfo objects are linked at runtime (primitives
  included).
- Coverage of the v0.1 hit set: 3482 hit functions (8.4%) belong to a reflected class with a full field layout, 3612 (8.8%) to a runtime class known by name, size and super class only. The field layouts reach further than these counts:
entity code reads its *EntityData (asset) fields throughout, and those are reflected.
- Not reflected: the runtime members of game objects (entities, components, players: ServerPlayer and EntryInputState
  have no record), engine internals, and the libraries. Havok's own hkClass reflection was stripped from this build
  (only 5 hkClassEnum tables remain); Scaleform has none.
- Generator (prototype): `scripts/decomp/gen_types.py <typeinfo.bin>` writes `project/src/native/fb/reflected_types.h`:
  3848 types (structs with base classes, enums), laid out as they sit in guest memory with ReXGlue's `rex::be<T>` and
  `TypedGuestPointer<T>` plus `project/src/native/fb/types.h` (CString, Array<T>, Enum<E>, Guid), and a `static_assert`
  for every field offset and every type size (13192, all passing: the layouts are exact). Gaps: 419 fields whose type
  the runtime never linked (`/* unknown type */`), 648 arrays with an unknown element type.

## 4. Proposal: differential test harness

Purpose: show a native function behaves like the recompiled one on real inputs, so refactoring can go as far as
idiomatic C++ without breaking the game.

Capture, in a development build, without new codegen: every generated function takes `uint8_t* base` and reaches
memory only as `base + guest address` (REX_LOAD/STORE, indirect calls through the table at
`base + IMAGE_BASE + IMAGE_SIZE`). So a sampled call of sub_X can be run a second time against a shadow memory:
1. Reserve a 4 GB shadow region (once), PAGE_NOACCESS.
2. At a sampled entry (an override wrapper, or the debug API `trace` hook), copy the PPCContext and run
   `__imp__sub_X(ctx_copy, shadow_base)` on the same thread. A vectored handler registered ahead of the runtime's
   commits each shadow page on first touch, copies it from real guest memory and records it as an input page.
3. Afterwards: output registers (r3/r4, f1, v2, cr, the non-volatile registers to check the ABI) and the bytes that
   differ between each touched shadow page and its input copy = the memory writes.
4. Then let the real call run normally on real memory; the game is unaffected apart from the time.
The capture is exact for code that only touches memory. Kernel imports act on host objects (events, files, threads),
so in capture mode the import thunks log their arguments and results instead (one TLS check in the import thunk
template: a small codegen change) and replay serves them from the log; a native implementation must make the same
import calls in the same order. 65% of the hit set (26,079 functions) reaches no import through direct calls;
indirect calls are detected at run time the same way.

Replay: a test executable (one Catch2 test per recorded case, data-driven) links the module's recompiled objects
and the native overrides, maps the input pages into a fresh `rex::memory::Memory` (as ReXGlue's
`recompile-tests` already does for its instruction tests), and runs both `__imp__sub_X` and the native `sub_X`
on separate copies, comparing output registers and written bytes (floating point bit-exact by default, a tolerance
per function where the native code legitimately reorders math). The recorded result also checks the harness itself
(recompiled replay == recording). The case format follows `test_recompiler`'s spec (inputs/outputs: registers,
memory), with the page data in a side file (`tests/diff/<sub_X>/<n>.case`, zstd), so the existing JSON shape and
templates carry over. Sampling: first N calls plus calls with new argument shapes (e.g. distinct vtables in r3),
recorded during the coverage sessions; cases are kept in `logs/` (game data) and regenerated, not committed.

Limits: functions that block on other threads (waits, job completion) or read the clock are recorded with their
import results but may need hand-written cases; data races (another thread writing the same memory meanwhile) show
up as replay mismatches against the recording, which flags them rather than hiding them.

## 5. Proposal: native source layout and progress metric

- `project/src/native/<subsystem>/`: core, input, ui, game, ai, render, audio, anim, physics, havok, net, online,
  libs (CRT, Lua, zlib), fb (shared Frostbite types: generated `reflected_types.h`, hand-written `types.h`). Globbed
  into the BF.Main DLL by CMakeLists.txt. One file per class or cohesive group of functions.
- Each native function is an override of sub_X (the weak generated symbol; the original stays `__imp__sub_X`),
  preceded by `BF3_NATIVE(<guest address>, <stage>)` (`project/src/native/native.h`). Stages: `lifted` (the generated
  body copied by `scripts/decomp/lift.py`, identical by construction: compiles and boots at once), `typed` (typed
  parameters and structs at the ABI boundary), `idiomatic` (readable C++: classes, loops, no ctx juggling inside),
  `verified` (idiomatic and passing the differential tests). The override keeps `REX_COVERAGE_HIT`, so coverage
  still sees it.
- Metric: `scripts/decomp/progress.py`: share of the v0.1 hit set at each stage, by function count and by guest
  code bytes, overall and per group. The headline is % verified by bytes. Phase-1 hooks (patches in
  `project/src/bf_main/`) are reported separately; they are not decompiled code.
- After v0.1 functions are native, a second metric takes over: guest-free (no `base`/guest memory, native data
  layout). That step goes subsystem by subsystem at clean boundaries (e.g. the input pipeline's output struct), once
  everything that touches a structure is native.

## 6. Plan

First subsystem: the input pipeline. It is small (call-graph closure in the hit set from the pad poll sub_832479F8,
the concept getters sub_8324B728 / sub_8324C130, the entry mapper sub_8324E938 and the client input update
sub_832303C0: 162 functions, 39 KB, of which the input-specific core is a few dozen), already understood (docs:
input cache layout, InputConceptIdentifiers, EntryInputActionEnum, soldier map, EntryInputState), mostly pure data
transformation (one import: XamInputGetState), testable by the harness with plain inputs, and it is the code the PC
keyboard/mouse work already hooks (pc_concepts.cpp, mouse_aim.cpp), which native code replaces cleanly. It also
exercises every step once (lift, type with reflected InputConcept/EntryInput data, refactor, verify) on low-risk code.

Order after it:
1. Input (above), then the aiming/camera path (sub_83264488, sub_83267DF0, sub_8325C658: 59 functions), already
   understood from the mouse-look work.
2. Front-end flow and UI data (graph transitions sub_832917E0, node entry sub_832914F8, message handler sub_8374EE10:
   172 functions), which boot_flow.cpp and the PRACTICE page already reverse-engineered.
3. Frostbite core the rest builds on: job system (sub_8346D590 is already a native port), memory allocators, message
   dispatch (sub_83026CB8), entity/type system (reflection-typed), resource and superbundle loading (0x831B0000 block,
   92 functions; formats known from scripts/fb_bundles.py).
4. Game simulation for TDM: server/client soldier, weapons and firing, damage, spawning, round and scoring
   (reflected *EntityData layouts give the data side).
5. Network (ghosts, replication between the in-process server and client) and the Metro-relevant online stubs.
6. Animation (EA ANT), audio (Frostbite sound, rw::audio), render (Frostbite renderer, then the GPU interface).
7. Libraries last and largely from known sources: Havok 2010.2 and Scaleform 3.3 (layouts from their SDK headers),
   Lua 5.1, zlib, EASTL, CRT (replace with host equivalents where behaviour allows).

Open issues seen while recording (not caused by coverage):
- GPU hang during a gadget phase (run2, engineer, RPG): the GPU stopped completing fences (completed 0x3E3E5, issued
  0x3E3FB) and the main thread waited in sub_82FCC8D8 for good. Intermittent (same phase passed in the other runs).
- Debug API snapshot (DevFrame) read stale guest pointers and ended the game three times (fix log).
