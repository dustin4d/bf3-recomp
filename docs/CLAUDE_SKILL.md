# Skill: Battlefield 3 ReXGlue Recomp

**Purpose:** Assist with static recompilation of Battlefield 3 (Xbox 360, title ID 45410950) using ReXGlue, Ghidra,
and GhidraMCP.

## Environment
- Working dir: `/home/dustin/workspace/bf3-recomp`
- Disc images: `dump/Disc 1/*.iso` (Multiplayer-Co-Op), `dump/Disc 2/*.iso` (Single-Player Campaign). Redump-named,
  8738846720 bytes each, XGD3 (game partition at 0x2080000).
- Full game partition extracted to `dump/Disc N/game/` with `rex_extract disc` (ReXGlue's DiscImageDevice):
  Disc 1 104 files / 7700635673 bytes, Disc 2 97 files / 7058107060 bytes, 0 failures; paths and sizes match the
  independent `scripts/xdvdfs.py` listing. Logs: `logs/extract-disc{1,2}.log`.
- HD Content package unpacked to `dump/Disc 1/hd_content/` (`rex_extract stfs`): `Data/Xenon/HD.sb` (1566438432),
  `Data/Xenon/HD.toc` (identical to the disc's own HD.toc), `package.mft` (Name hd, Flag hdcontent, Location hdd).
- Flat PE images: `dump/Disc N/<Module>.image.bin` (`rex_extract image`), each mapped at its XEX base. All four
  are LZX-compressed (window 0x8000), retail-encrypted, PE machine 0x1F2, ImageBase = XEX base. **Decoded images are
  byte-identical across discs**, so code addresses hold for either disc. Image md5: Default 9ad74459dd3a465603f3dea9bc894244,
  BF.Splash ad9eba4bd935c74957c5ce9cf127cae1, BF.Main 350cc9c468680e27a212cb4b93d0db5c,
  Engine.BuildInfo 9d397627e2889d1e57bca26cd1b4670f.
- XEX files: Same build on both discs (version 5), only the header's media ID and
  disc number differ, so the file hashes differ too:

  | Module | Orig name | Base | Size | Entry | md5 Disc 1 | md5 Disc 2 |
  |---|---|---|---|---|---|---|
  | Default.xex | BF.Launcher_Xenon_Retail.exe (title exe) | 0x82000000 | 0xC0000 | 0x82017B98 | a41a06fd19c2cbebc16537b3e347d91d | 950dcf747ff368f2cf959240d6a69639 |
  | BF.Splash.xex | BF.Splash.dll | 0x83480000 | 0x990000 | 0x83CBFF00 | 4aaa21630ac6f646dd5b3826f73fe8a3 | 4071689fdf7100e9f8ac2dd166826da3 |
  | BF.Main.xex | BF.Main.dll | 0x82D80000 | 0x14E0000 | 0x838B6040 | 9350704350c4317784b6008d202dfeaa | 9d0c127369f04080707b9f8e62c1ff6a |
  | Engine.BuildInfo.xex | Engine.BuildInfo.dll | 0x89000000 | 0x100000 | 0x89010E90 | d2e6ad54276f028942278386de0be3f1 | 56cf28dd98ad9308ab6fc8a362583e4e |

  Media IDs: Disc 1 0x61C746D7, Disc 2 0x511536A8. Ghidra: load from `dump/Disc 1/game/` (canonical; Disc 2 decodes to the same images).
- Project: `project/` (manifest: `project/bf3_manifest.toml`)
- SDK: `tools/rexglue-sdk` + `tools/rexglue-sdk-win` = **nightly-20261002-bd833a2a (0.10.0.24-dev.gbd833a2)**,
  sha256 checked (linux `0a565652...a3befe`, win `1e19d7ec...7627e7`). Switched from v0.10.0 (kept as
  `tools/rexglue-sdk-0.10.0`, `tools/rexglue-sdk-win-0.10.0`) because v0.10.0 codegen emits NOTHING for
  `lwsync`/`sync`/`eieio` (BF.Main: 279/19/16 sites), and Frostbite's lock-free job system broke without the fences
  (null job callback in sub_8346D590 or a hang, varying run to run). The nightly's generated rexglue.cmake,
  CMakePresets.json and main.cpp are identical to v0.10.0's. After an SDK swap: `cmake --preset ... --fresh` and
  `rexglue codegen bf3_manifest.toml --ignore-stamp`.
- xwin 0.10.0 (`tools/xwin-bin/`, sha256 checked) splatted to `tools/xwin/` (CRT 14.44.35220, Windows SDK
  10.0.26100, same files as the Trials HD setup). Cache in `tools/xwin-cache/`, log `logs/xwin-splat.log`.
- `tools/cross/` (committed): `win-amd64-xwin.cmake` toolchain + `rexglue/` wrapper package (Windows SDK config,
  codegen pointed at the Linux binary), copied from Trials HD. Gotchas in the reference doc. Not yet exercised here.
- **Patched GPU plugin** (required): `tools/rexglue-src` = ReXGlue source at bf3-sdk commit bd833a2a (git clone, not
  committed) with `patches/rexglue/*.patch` applied; `gpu_plugin/CMakeLists.txt` builds only `rexgpu-xenos.dll`
  (D3D12, like the prebuilt Windows SDK) against the prebuilt Windows SDK runtime via `rex::runtime`:
  ```bash
  cd tools/rexglue-src && git checkout bd833a2a && git apply ../../patches/rexglue/*.patch && cd ../..   # 0001, 0002
  cmake -S gpu_plugin -B gpu_plugin/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=$PWD/tools/cross/win-amd64-xwin.cmake -Drexglue_DIR=$PWD/tools/cross/rexglue
  cmake --build gpu_plugin/build      # ~20 s; win_deploy.sh deploys it instead of the SDK's stock plugin
  ```
  Needs `-msse4.1` and `tools/cross/include-case/` (relative symlinks ObjBase.h, SDKDDKVer.h: xwin's headers are
  lowercase). New patches: scripts/rexglue_patch.sh <name> writes the changes not yet in patches/ to the next file.
  Patch 0001 = precise memexport readback (cvar `readback_memexport_precise`, default on). Patch 0002 =
  16-bit integer resolves, integer texture fetch, small-resolve readback, GPU frame tracer, slow WAIT_REG_MEM log.
- GhidraMCP: reachable from WSL (mirrored networking). Next phase: import `dump/Disc 1/game/Default.xex` and
  `BF.Main.xex` (then BF.Splash, Engine.BuildInfo) with XEXLoaderWV, confirm image md5s/bases above with
  `get_program_info`, then find how the launcher loads the DLL modules (XexLoadImage / XexLoadExecutable imports).
- Ghidra project: `C:\Users\Dustin\BF3-RECOMP` (user-created). `/modules/` = the four .xex imported with
  XEXLoaderWV, unanalyzed, untouched. `/merged/BF.Main.xex` = BF.Main + Default + Engine.BuildInfo at their real
  addresses + BF.Splash in overlay space `BF_Splash` (addresses `BF_Splash::83480000`): 38 blocks, 66770 functions
  (from .pdata), labels/imports copied. Open `/merged/BF.Main.xex` for GhidraMCP.
- Auto-analyzed 2026-10-24 (headless, 12G heap, ~9.5 min) with **Non-Returning Functions - Discovered disabled**:
  75695 functions, all 66770 .pdata entries kept (`python -I scripts/pdata_coverage.py logs/merged-functions.txt`).
  With default options the same analysis cleared the code of 7686 .pdata functions (6795 BF.Main, 891 Splash; left as
  undefined bytes, almost all preceded by padding, VMX128 not a factor). Keep that analyzer off for this project.
- Remaining analysis noise: ~357 "Bad Instruction: Unable to resolve constructor" = VMX128 forms (primary ops 4/6)
  that Ghidra's PowerPC:BE:64:A2ALT-32addr doesn't decode, in ~37 functions; only their decompiler output suffers
  (ReXGlue has its own decoder). Two harmless "Invalid PNG data" errors in BF_Splash .rdata.
- Verified with `BF3VerifyAgainstImage`: every block of all four modules matches `rex_extract image` byte for byte,
  except the tail of BF.Main `.reloc` (VA 0x841F0200, vsize 0x13DED0, runs 0xCE0D0 past the image end 0x84260000;
  the in-image part differs between the two decoders). XEXLoaderWV logs `ERROR out of input bytes
  (LzxDecompression)` on every module for the same reason. Harmless: titles load at a fixed base, relocs are unused.
- A single merged XEX isn't possible (one PE per XEX, signed headers, BF.Splash overlaps BF.Main) and XEXLoaderWV
  has no Add To Program, hence the merge script.

## Commands
```bash
tools/rexglue-sdk/bin/rexglue --version   # 0.10.0; subcommands: codegen, init, recompile-tests
# init flags (verified with --help): --project-name, --xex-path (required), --game-root, --project-root,
# --scan-dll, --template-dir. Use absolute paths.
tools/xwin-bin/xwin-0.10.0-x86_64-unknown-linux-musl/xwin --accept-license --arch x86_64 \
  --sdk-version 10.0.26100 --cache-dir tools/xwin-cache splat --output tools/xwin
```
# rex_extract: our tool on ReXGlue's runtime (DiscImageDevice, StfsContainerDevice, lzx_decompress, aes128).
# Needs C++23 (SDK headers use std::byteswap). Build:
cmake -S scripts/rex_extract -B scripts/rex_extract/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=$PWD/tools/rexglue-sdk
cmake --build scripts/rex_extract/build
X=scripts/rex_extract/build/rex_extract
$X disc  "dump/Disc 1/<name>.iso" "dump/Disc 1/game"          # ~45s per disc
$X stfs  "dump/Disc 1/game/Content/0000000000000000/45410950/00000002/4541095000000000" "dump/Disc 1/hd_content"
$X image "dump/Disc 1/game/BF.Main.xex" "dump/Disc 1/BF.Main.image.bin"
python -I scripts/xdvdfs.py list <iso>      # independent pure-Python XDVDFS reader, for cross-checks

# Headless Ghidra (Windows, project BF3-RECOMP must be closed in the GUI). Log: logs/ghidra-<name>.log
scripts/ghidra/headless.sh import /modules -import "$(wslpath -w "dump/Disc 1/game/BF.Main.xex")" ... -noanalysis
scripts/ghidra/headless.sh merge /merged -import "$(wslpath -w "dump/Disc 1/game/BF.Main.xex")" -overwrite \
  -noanalysis -postScript BF3MergeXexModules.java /modules/Default.xex /modules/Engine.BuildInfo.xex /modules/BF.Splash.xex
scripts/ghidra/headless.sh verify-merged /merged -process BF.Main.xex -readOnly -noanalysis \
  -postScript BF3VerifyAgainstImage.java <BF.Main.image.bin> <Default...> <Engine.BuildInfo...> BF_Splash=<BF.Splash...>
```
# Auto-analysis (also exports the function list for the .pdata check):
GHIDRA_HEADLESS_MAXMEM=12G scripts/ghidra/headless.sh analyze /merged -process BF.Main.xex \
  -preScript BF3SetAnalysisOptions.java "Non-Returning Functions - Discovered=false" \
  -postScript BF3ExportFunctions.java "$(wslpath -w "$PWD/logs/merged-functions.txt")"
python -I scripts/pdata_coverage.py logs/merged-functions.txt
```
Other scripts: `BF3ProbeAddresses.java <addrs.txt>` (what's at each address + Error/Warning bookmark counts).
Headless gotchas: `wslpath -w` on a relative path gives a relative Windows path, so pass absolute paths.
`-postScript` takes a script name only, and `C:\Users\Dustin\ghidra_scripts` wins over
`-scriptPath`, so repo scripts carry a `BF3` prefix. The wrapper exits 1 on any ERROR line, including the
harmless LZX one: read the log. Imports take about 1 min; the program image base stays 0 (XEX base is not applied).

### ReXGlue project (verified 2026-10-24)
```bash
# init (done once). It LOWERCASES every path it writes (game_root, file_path): fix the case in the manifest.
tools/rexglue-sdk/bin/rexglue init --project-name bf3 --xex-path "$PWD/dump/Disc 1/game/Default.xex" \
  --game-root "$PWD/dump/Disc 1/game" --project-root "$PWD/project"
# --scan-dll only finds *.dll; BF3's modules are .xex, so [[modules]] are written by hand.
# guest_path = lowercase path relative to game_root (what --scan-dll writes), e.g. "bf.main.xex".
# Per-module function fixes go in [modules.functions] right after that module's [[modules]] block.
cd project
cmake --preset local-debug | win-cross-release        # CMakeUserPresets.json (ours), -j6
cmake --build --preset win-cross-release --target bf3_codegen > ../logs/codegen.log 2>&1   # ~4 min
# Codegen for a subset (entrypoint always included). Target names: BF_Main, Engine_BuildInfo, BF_Splash.
../tools/rexglue-sdk/bin/rexglue codegen bf3_manifest.toml --target BF_Main --target Engine_BuildInfo
cmake --build --preset win-cross-release > ../logs/build-win.log 2>&1
```
Codegen log: ~600k "Analyze: function 0x... not in any code region" lines are noise (pointer-like data);
filter them out (`grep -v 'not in any code region'`). Real failures end with `Validation failed: N unresolved calls`,
listed as `<target> from <src>: b <target> ... - target not in any function`.
- **Build gotcha**: when codegen runs inside `cmake --build`, ninja may link stale objects of the files codegen
  just rewrote (seen as `undefined symbol: sub_X` for removed entries). Run the codegen target (or `rexglue codegen`)
  first, then build; or build twice.
- **Hooks for module code go into that module's DLL**: a strong `sub_X` in bf3.exe can't override a weak function in
  bf3_BF_Main.dll. BF.Main hooks live in `project/src/bf_main/` and are added with `target_sources(bf3_BF_Main ...)`
  in CMakeLists.txt (they include the module's own `bf3_pch.h`). Launcher hooks stay in `project/src/`.
- Each DLL module becomes its own shared library (`bf3_BF_Main`, `bf3_Engine_BuildInfo`); codegen writes
  `generated/default/module_registry.cpp` + `dll_targets.cmake` listing only the modules of that run.
- BF.Main generates ~300 MB of C++ (291 .cpp in total). Functions 0x834EA5D0 (1.2 MB) and 0x834F8010 (1.3 MB)
  exceed max_file_size_bytes: warning only.
- `docs/reference/trialshd_CLAUDE_SKILL.md` has the forms that worked for Trials HD on the same SDK version.

## Rules
- Never distribute or request game assets.
- Prefer minimal, reversible changes (manifest entries, stubs) over rewrites. Never hand-edit `generated/`.
- Always cite the guest address when proposing a fix, and log it below.
- Query GhidraMCP for specific addresses or functions; never dump the whole binary.

## Fix log
| Address | Fix | Reason |
|---|---|---|
| BF.Splash module | `[[modules]]` entry commented out | Codegen loads all modules into one address space; Splash (0x83480000-0x83E10000) overlaps BF.Main, so Splash was analyzed from Main's bytes: 7299 bogus unresolved branches (2000/2000 sampled sources decode to the reported target only in BF.Main's image). Launcher's Splash load still to be handled |
| BF.Main 0x83307A48, 0x830D25F8, 0x8366CE60, 0x832EB7E0 | `[modules.functions]` sizeless `{}` | Leaf functions in gaps between .pdata entries, reached only by tail-call `b` from 0x831103CC, 0x830DEBB4, 0x8366F714, 0x832EE868 |
| Engine.BuildInfo .xidata 0x89010010..0x89010FE0 (254) | `{ size = 0x10 }` each, from `scripts/xidata_thunks.py` | Executable linker thunks `lis/addi r11; mtctr; bctr` with no .pdata; 8 were `b` targets (e.g. 0x89010940 from 0x8901E420). Import stubs later in .xidata left to the import table |
| Engine.BuildInfo 0x89017EEC | sizeless `{}` | Leaf in a .text gap (andi. r0,r5,3; mtctr; beqlr), tail call from 0x89017F3C |
| launcher sub_820112A0 | hook `src/launcher_hooks.cpp` (cvar `skip_splash`, default on) sets state+8 = 1 before the original runs | sub_820112A0 loads `GAME:\BF.%s%s.xex` ("Splash" while byte state+8 == 0, else "Main"), calls export ordinal 1 with the state, unloads, sets state+8. Splash isn't recompiled: run 2 died with `Execute(83CBFF00): function not in function table` / ordinal 1 out of bounds. A failed load relaunches default.xex (sub_82017D78) |
| BF.Main 0x83E99420 + 231 more | sizeless `{}` from `scripts/find_unregistered.py` | Run 3: FATAL unregistered call to 0x83E99420, a static initializer whose only reference is a pointer at 0x83FF3F24 (.data). Same class swept in all modules |
| BF.Main setjmp 0x83DF2720 / longjmp 0x83DF29F0 | `setjmp_address` / `longjmp_address` in BF.Main's `[[modules]]` | Run 4: `*** STOP: 0x00000000` = exit(1) from luaD_throw (0x838F9F18): Lua's longjmp just returned without the declaration, so a Lua error fell through to panic. Found via the doexit call chain (exit_trace.cpp) |
| GPU plugin | `GPU_PLUGINS xenos` + default `gpu_plugin = "xenos"` | Run 5: stalled at VdInitializeRingBuffer, no GPU emulation |
| Toolchain: v0.10.0 -> nightly-20261002 | SDK swap (see Environment) | Run 6-8: null call in job worker sub_8346D590 (12 s) or hang (4 s), nondeterministic: v0.10.0 emitted no fences for lwsync/sync/eieio. Fences alone did NOT fix the null call |
| GPU plugin: memexport readback | patches/rexglue/0001: snapshot export ranges before the draw, write back only bytes the draw changed | BF3 declares its whole GPU-writable heap F1A9F000 (0x60000, holds the job pool) as a memexport range; stock readback copied the whole range from the GPU copy and rolled back CPU writes (free-list ABA -> double alloc -> jobs run twice -> null calls, hang). With readback off the game waits forever on exported data. Result: title screen "PRESS START TO BEGIN" (2026-10-25) |
| BF.Main sub_8346D590 (job worker) | `src/bf_main/job_worker.cpp`: faithful C++ port that skips fn == 0 | Run 9: no crash, ran 90 s; skipped 64-127 jobs in one burst at ~11 s, loading screen faded out (new tip), then black window + silent log = likely hang. OPEN: skipped jobs may not be "already finished" (published before fn written?), check who sets job hi bit 0 and who waits on parent+48 |
| 33 sweep hits (BF.Main 18 incl. 0x838B2CFC, 0x83001C00; all 6 Default; all 9 Engine.BuildInfo) | commented out in manifest (`REMOVED`) | Interior labels inside .pdata functions (likely SEH handler/scope targets; same 0x5BC/0x848-byte CRT functions in all modules). 3 showed as codegen `Unresolved b target`, 0x83001C00 (bdz target in sub_83001B30) only as a compile error `undeclared label loc_83001C00`. find_unregistered.py now skips targets inside .pdata functions |
| GPU plugin: resolve to 16-bit integer formats | patches/rexglue/0002 (util/draw.cpp GetResolveInfo): for k_16/k_16_16/k_16_16_16_16 destinations with number format UINT/SINT, rebase exp_bias by -16/-15 and resolve as UNORM/SNORM | BF3 resolves its 7e3 HDR scene to 16_16_16_16 UINT with exp_bias 11 (round(v * 2048)); the resolve shaders only pack normalized values (saturate * 65535), so every texel became 0xFFFF |
| GPU plugin: texture fetch num_format INTEGER | patches/rexglue/0002 (dxbc_translator_fetch.cpp): when fetch constant dword 3 bit 0 is set, scale the fetched value by the format's max (255 / 65535, signed 127 / 32767) before exp_adjust | Host textures are normalized; BF3 fetches the HDR texture as integers with exp_adjust -11, so it read 65535x too dark |
| GPU plugin: small resolves read back | patches/rexglue/0002: cvar `readback_resolve_small_max` (default 0x4000): with readback_resolve none, resolves up to that size are still copied back to guest memory (fast, one frame late) | The CPU reads the 32x1 average luminance resolve (0x1E364000) for the exposure |
| GPU plugin: blending on full-range fixed16 RTs | patches/rexglue/0003: when RT0 is k_16_16(_16_16) (host RTV path, full range) and its blend uses SRC_ALPHA / INV_SRC_ALPHA, the pixel shader also writes the unscaled alpha to SV_Target1 and those factors become SRC1_ALPHA / INV_SRC1_ALPHA (modification pixel.dual_source_alpha) | The full -32..32 range is emulated by dividing the output by 32, which also divided the blend factor: BF3's half-res particle buffer (premultiplied ONE / INV_SRC_ALPHA) accumulated alpha up to 8 instead of 1, and the per-16x16-tile particle composite (tile mask R40 96x44, stencil-marked point sprites, sub-pass D3884 overwrites "opaque" tiles) pasted dark or white blocks over smoke |
| BF.Main mfmsr/mtmsrd (every lwarx/stwcx. bracket) | manifest `skip_msr = true` in BF.Main's [[modules]] | Emulated as ReXGlue's process-wide global_critical_region mutex (REX_ENTER/LEAVE_GLOBAL_LOCK), which serialized the job system; atomics are CAS anyway and on hardware the bracket only masks local interrupts. Gameplay 38 -> 56 fps uncapped |
| BF.Main frame wait sub_83838CF0 (f1 = min frame time, 1/30) | `src/bf_main/frame_rate.cpp`: f1 = 1/BF3_MAX_FPS (default unlimited) while HudScreen is up; menus/loading keep 30 | Unbounded FPS request; uncapped boot/loading hit races (texture bind of a half-built texture sub_8313C6E0 from sub_83D21C50 -> guarded: bind none; GPU sync slot of sub_82FD6968 never released -> WAIT_REG_MEM 0x11A97xxx forever) |
| BF.Main FadeManager (0x840BAE54) fade in sub_83247D38 | `src/bf_main/fade_fix.cpp`: if a FadeEntity's StartFaded (sub_83247E10, name 0x82DCF6C0) isn't followed by a fade in within 5 s, fade in (1 s) from the level manager update (level_boot.cpp sub_833AF040) | In MP levels started from PRACTICE the level's FadeEntity never gets its FadeIn event: the color grading LUT job (sub_83C8EB28, matrix from sub_83D1B078) used an all-zero matrix and the UI was faded too, so the screen stayed black |
| BF.Main server team (ServerPlayer::setTeam sub_833CACD0, team at player+0x324) | `src/bf_main/practice.cpp`: the player that entered the level (ServerPlayerEnteredLevelMessage +0x28, seen in the dispatcher sub_83026CB8) gets team 1 (BF3_TEAM=2: RU) when the spawn screen appears; the server's network handler sub_836C7AA0 also applies NetworkSelectTeamMessage (+0x38) | Without a Blaze game nobody assigns a team: the player stays TeamNeutral, the spawn screen shows ID_M_NEUTRAL / "DEPLOY AS undefined" / kitItem_N and DEPLOY does nothing. sub_836C7AA0 ignores NetworkSelectTeamMessage in this build |
| BF.Main ServerPreRoundEntity (type 0x8418958C, check sub_837D3BE0) | `src/bf_main/practice.cpp` SoloRound: at the spawn screen, every pre-round entity gets data +0x60/+0x6C = 1 and its copies +0xA4 = 1 (start players), +0xA8 = 0 (restart players), and the round starts (+0xB4 = +0xAC, +0xB8 |= 0x80, sub_83838328(entity, 1)) | TDM needs 8 players before the round starts; in pre-round the soldier can turn but not walk or fire |

## Known notes
- Module layout: Default.xex is a small launcher; the game is in DLL-type XEX modules (imports xam/xboxkrnl).
  BF.Splash (0x83480000-0x83E10000) lies inside BF.Main's range (0x82D80000-0x84260000), so the two can't be
  resident together: the launcher presumably unloads Splash before loading Main. This matters for how ReXGlue
  registers [[modules]] (`init --scan-dll` looks for `.dll` files, but these ship as `.xex`). Not yet verified.
- Disc 1 also has `Content/0000000000000000/45410950/00000002/4541095000000000`: PIRS package
  "Battlefield 3 HD Content" (content type 0x2, 1576255488 bytes), the optional HD texture install. On a console it is
  installed to the HDD; the runtime will need it under its content dir (00000002 = marketplace content) or skip it.
- Data is Frostbite 2: `Data/initfs_Xenon`, `Data/layout.toc`, `Data/Xenon/*.sb/.toc` superbundles, `Movies/*.wmv`.
  Disc 1 = COOP_*/MP_* levels + MpChunks.sb; Disc 2 = SP_* levels + SpChunks.sb; both have FrontEnd, UI, Loc, Weapons.
- The loose `dump/Disc N/default.XEX` + VIDEO_TS/AUDIO_TS/$SystemUpdate are the DVD-video partition, not the game:
  the 16KB "play this on Xbox 360" stub (md5 6565efcdcc36752290f223d5da37d07d on both discs). Ignore them.
- PE sections: BF.Main has `typeinfo` and `fieldinf` (likely Frostbite reflection: type/field info) plus `ctr`;
  all modules have `.pdata` (function table for Ghidra/boundary checks) and `.XBLD`; Engine.BuildInfo has
  `.xidata`/`.xedata` (it exports to the others). Not yet examined.
- `scripts/xex_image.py` from Trials HD handles basic compression only; it does not work here (LZX). Use `rex_extract image`.
- Game data must be on NTFS: the runtime lowercases guest paths (`game:\default.xex`), so the case-sensitive
  `\\wsl.localhost` share fails ("Entrypoint XEX not found", modal dialog, exit code 116). Disc 1 minus
  `$SystemUpdate` and `Content/` (HD pack) is copied to `C:\Games\BF3-recomp\game` (5.7 GB, 71 files, sizes checked).
- Launcher flow (Default.xex): sub_820112A0 is the module loader (see fix log); export ordinal 1 is the module
  entry, called with the launcher state object. `0x820130D8` is `__savegprlr_28`, not a getter (decompiler confusion).
- Testing: `SHOTS=3000 scripts/win_run.sh <secs> <level>` deploys, runs from C:\Games\BF3-recomp with
  `--game_data_root=C:\Games\BF3-recomp\game`, logs to `logs/win-run.log`, user data to `logs/win-userdata`.
- `find_unregistered.py` hits that land inside a function show up after codegen as `Unresolved b target X from Y`;
  the culprit is the added entry between X and Y. Remove it and regenerate.
- Headless decompile: `BF3Decompile.java <out.txt> <start>-<end> | <addr>...` (launcher: 82010000-8201C650).
- Crash tracing (`project/src/crash_trace.h`, installed in OnPostInitLogging): logs the host stack as
  `<module>+0xRVA` for guest null-page faults and for the runtime's `[FATAL] Call to invalid or unregistered function`
  (a log sink, null_mutex because it logs through the same loggers). Linker maps (`bf3.map`, `bf3_BF_Main.map`,
  ...) are written by CMakeLists.txt; `scripts/win_run.sh` resolves frames with `scripts/resolve_rva.py`.
  Windows reports the module as `bf3_BF_Main.DLL` (resolver matches case-insensitively).
- `*** STOP: 0x00000000` = the game called exit(): BF.Main CRT doexit is sub_838B5860; `src/bf_main/exit_trace.cpp`
  logs its args and the guest call chain (back chain 0(r1), saved LR at -8 of the caller's r1). Frostbite embeds Lua:
  luaD_rawrunprotected 0x838F9300, luaD_throw 0x838F9E38 (panic + exit(1) when no errorJmp), lua_pcall-ish 0x838F6A68,
  luaL_loadbuffer-ish 0x838F7B38. Script helpers like `return tableToString(_G['%s'])` (0x82D845D0) run at boot.
- GPU: without `gpu_plugin` the game stalls after VdInitializeRingBuffer ("no GPU emulation loaded"). Fixed by
  `rexglue_setup_target(bf3 GPU_PLUGINS xenos)` (stages rexgpu-xenos.dll) and Bf3App::OnPreSetup defaulting to xenos.
  D3D12 on the host's RX 7800 XT. First rendered frames: the loading screen with a gameplay tip (2026-10-24).
- BF.Main job system: worker loop sub_8346D590 claims jobs with ldarx/stdcx on a 64-bit {next, flags} word and calls
  the job fn at +8 (then clears it); sub_8346E020 pops the list. Code brackets atomics with mfmsr/mtmsrd r13,1
  (interrupt disable on the console; no-op here).
- `XMA: Write to unknown register (0601)` floods debug logs (same as Trials HD). `SpChunks.toc` open fails on Disc 1
  (it's Disc 2's campaign TOC): expected.
- ROOT CAUSE of the job-system corruption (2026-10-25): GPU memexport readback. The game declares its whole 0x60000-byte
  E-range heap F1A9F000 (phys 0x11AA0000) as a memexport target, and the job pool (head F1A9F180, nodes F1AA....) lives
  in that heap. ReXGlue (readback_memexport defaults to TRUE, unlike Xenia) copies the whole declared range back
  (IssueDraw_MemexportReadbackFull/FastPath -> memcpy(TranslatePhysical(base), readback, size)); the fast path even
  copies the previous draw's data. That rolled the pool's tagged free-list head back ~40 updates -> double alloc ->
  jobs run twice -> fn == 0 null calls and the hang. Found with a hardware write watch (BF3_WATCH, crash_trace.h) on the
  head incl. its physical aliases: the writer was VCRUNTIME140!memcpy from rexgpu-xenos's command processor via the raw
  physical view (host 0x200000000 + phys). E-range phys = (addr & 0x1FFFFFFF) + 0x1000.
  - readback_memexport=false: no corruption, but the main thread then waits forever in the D3D GPU wait sub_82FCC8D8
    (ring read pointer never advances; GPU Commands thread waits inside packet processing). readback_resolve
    some/full doesn't change that. So the game needs some exported data in memory (likely a WAIT_REG_MEM on it).
  - src/gpu_readback_fix.cpp (memcpy IAT hook in rexgpu-xenos.dll, shadow of uploads, write only bytes the GPU
    changed, forces readback_memexport_fast=false): cuts written bytes from 2.1 MB to ~10 KB per 8 readbacks, but the
    plugin's GPU copy of the heap is sometimes OLDER than the last upload (e.g. 0xDDDDDDDD vs 3F001FEF at +0x28), so
    some stale bytes still land: fn==0 still occurs. Needs a GPU-side before/after diff in the plugin itself.
- UI screens (src/bf_main/ui_screens.cpp hooks sub_8328B990 = push screen, name at *(screen+0x18); exported
  bf3_ui_top_screen / bf3_ui_screen_count, read by src/ui_state.cpp). Seen: ComCenterHeaderScreen, StartScreen (pushed
  at load, again when the title is interactive, again after the idle loop), PopupGeneric, EmptyScreen, MainMenuScreen,
  OptionsNavigationXenonScreen + Options{Controls,Gameplay,Audio,Video}ConsoleScreen, ExtraMenuScreen,
  GoOnlineLoadingScreen (Multiplayer). Multiplayer/Co-op/My Soldier: PopupGeneric "sign in to Xbox LIVE".
  Pad script: `wait:StartScreen#2;3000:START;3000:START;3000:START;wait:MainMenuScreen;4000:DOWN;...`
- PC SETTINGS (src/pc_settings.cpp): F1, or the prompt shown bottom-right while any Options*Screen is on top.
  Tabs KEYBOARD & MOUSE (BF3 action names over the emulated pad buttons, 3 key slots, conflict marking), MOUSE
  (mnk_mouse, mnk_sensitivity), DISPLAY (fullscreen, vsync). Saves only those keys to bf3.toml. Writes take
  InputLock() exclusively (pad_inject.cpp holds it shared around every guest XamInputGetState); game input is blocked
  while the screen is open. BF3_TEST_PC_SETTINGS=<tab> opens it automatically on the first Options screen (tests).
  Known gap: menu and infantry actions share emulated buttons (Esc = Back and Crouch); needs context-aware binds
  once in-game screens are known. Mouse look also captures the cursor in menus.
- Level boot (BF3_LEVEL=<level>[:<layer inclusion>], e.g. MP_Subway or MP_Subway:GameMode=ConquestSmall0):
  - Default: boot the front end, then switch (level_boot.cpp, hook of the level manager update sub_833AF040): 3 s after
    CoreEnteredIngameMessage it feeds sub_833B1500 a ServerLoadLevelMessageBase built in guest memory (allocator
    0x84042270): +8 F8429A0E, +0x28 LevelSetup {char* name, options array (count at -4) of {char* category, char*
    value}, u32, u32 array}, +0x3d 0. The server unloads the front end and loads the level; the connected client
    follows (ClientConnectionLoadLevel -> ClientLevelLoaded).
  - BF3_LEVEL_DIRECT=1: the older route below (server loads the level at startup). The client can't join: its
    connection drops after ~7 s while the server is busy loading (not one of the cfg timeouts), then it never retries.
    It sets Online.Backend Backend_Peer and the Client/Server *TimeOut settings through spare Game.cfg comment lines.
  - BackendType values: Lan 0, Blaze 1, Peer 2, Local 3 (Online.Backend Backend_Peer -> type 2 without fallback).
  - BF3_MSG_TRACE=1 logs server/client messages; scripts/msg_names.py <log> names the hashes.
  - kernel_fixes.cpp: stale cached event handles in guest dispatcher headers (ReXGlue stashes 'REX\0' + handle in
    wait_list_flink/blink; reused memory keeps the old handle, which may now name another object type ->
    XEvent::Set+0x8 crash in rexruntime). Validated before KeSetEvent/KeResetEvent and RtlEnter/LeaveCriticalSection
    (RtlLeave calls xeKeSetEvent inside the runtime), cleared after RtlInitializeCriticalSection(AndSpinCount).
  - Packed vtable thunks 0x83B26248/70/88 (reached via a runtime dispatch table in sub_83B262E8) added to the manifest.
  - lua_dump.cpp rewrites "Game.Level Levels/Frontend/Frontend" in Scripts/Game.cfg (initfs chunk, CRLF) in place;
    padding goes on a comment line (trailing spaces became part of the value and failed the level check).
  - initfs_Xenon is XOR-obfuscated: key = file[0x128:0x128+257], body = file[0x22C:], byte ^ key[i%257] ^ 0x7B.
    Scripts/Game/Configure.lua reads commandLine, /local/local.cfg, Game.Level, <level>.cfg.
  - level_boot.cpp: server backend factory table 0x840DFBA4 (Xenon registers types 2, 3; MP boot asks for 1 ->
    null call) falls back to 2; BF3_BACKEND=<n> forces one. Level check sub_83845C30 allowed GameMode values:
    RushLarge0/1 ConquestLarge0/1 ConquestSmall0-3 SquadRush0-3 TeamDeathMatch0-3 SquadDeathMatch0-3 TutorialMP(_S).
  - Server level state machine: handler sub_833B1500 (ServerStartedMessage 3AD97D0A, ServerInternalLoadLevelMessage
    11FF6EFC, ServerLevelLoadedMessage 236C395A, ServerUnloadLevelMessage 6E1933E0, ServerLevelUnloadedMessage
    D3318581, ServerLoadLevelMessageBase F8429A0E, ServerStopMessageBase B455ADA6, ServerLoadGameMessage AB12C7A3;
    message type hashes are djb2-xor of the class name). Flags: +0x3c internal load, +0x3d unloading, +0x65 unload
    requested, +0x66 unload after load, +0x70 level. Unload (sub_833AB7E8 -> sub_83839C50) resets the 48 MB level heap
    0x84041D50 (sub_83013DA8), so allocations still in flight fault at guest 0xC.
  - The UI scripts call stopServer (app vtable+0x34 sub_82FE4350 -> sub_8339DF30 "Server shutdown") ~7 s after boot;
    ignored for level boots. CoreQuitMessage (Core 7C813E9E / E913E8AC) sets client+473 -> client update false.
  - Status 2026-10-26: the switch reaches MP_Subway's spawn UI (SpawnScreenTicketCounterTDMScreen, SpawnButtonConsoleScreen,
    SpawnScreen, ChatLogScreen, TabListenerScreen pushed ~20 s after the switch) but the screen stays black.
  - BF3_HEAP_TRACE=1: live-allocation tracker for the level heap keyed by guest call chain.
  - BF3_MAIN_HEAP_MB sizes the level heap (default 48, 160 for level boots).
- Crash trace also walks the host stack on the runtime's "Unhandled guest access violation" message.
- scripts/containing_func.py <addr>...: the recompiled function containing a guest address.
- PC-native button icons (src/pc_icons.cpp): the glyph atlas is ui/assets/buttons (CFX movie in Data/Xenon/UI.sb),
  image buttons_i1 = texture 2048x64 format 9 (ARGB8888, BE texels, X360 2D-tiled) in chunk ecb69283...; Xbox + PS3
  glyphs side by side. Found in guest physical memory by fingerprint (u64 at +0x78 of the first tile, FNV-1a of 4 KB),
  pristine copy kept, Xbox glyph boxes redrawn (GDI, 4x supersampled) as keycaps for the bound keys, written back via
  the 0xA0000000 view. Re-drawn when bindings change. Start reused the mirrored Back glyph: bf_main/ui_buttons.cpp
  patches the inflated movie (zlib inflate sub_8318B500, the only one) so shape 64 uses sub-image 25 (PS3 R1) unmirrored.
  scripts/fb_bundles.py (superbundle list/extract), scripts/x360_untile.py, scripts/gfx_dump.py, scripts/avm1_dis.py.
- Menu mouse (src/pc_mouse.cpp + bf_main/ui_mouse.cpp): window subclass -> bf3_ui_mouse_event queue -> delivered in
  GFxMovieRoot::Advance (sub_83A434A0, vtable +0x94) via HandleEvent (sub_83A348F0, +0xB4; event {type 1 move/2 down/
  3 up/4 wheel, f32 x, f32 y, f32 scroll, u32 button, u32 index}, viewport px). Movie: manager(sub_8328B990 r3)+48 ->
  holder, holder+12; +0x40/44 buffer 1280x720, +0x9B4 mouse count (0 on Xenon -> set 1), +0x2468 focus 0x40000.
  Menu mode: mouse look off, triggers masked, right-click / mouse back = B pulse. BF3_MOUSE_SCRIPT tests it.
- UI platform: AS HelperFunctions.getPlatform -> UIDataInterfaceComp.getData(0xD822972E) -> engine handler
  sub_837F3CC0 answers "xenon" (0x82D83914) via sub_8328D658; "win32" (0x82D8391C) gives PC screens (MainMenuScreenPC
  offline menu, OptionsControls/Gameplay/Audio/Video/KeyBindingScreen, PC popups, clickable buttons/tabs). The flow
  asks at two sites (via sub_83294100): 0x836E3D60 per pushed screen, 0x832884E0 for the main menu. Hybrid (default):
  main menu stays console (Multiplayer/Co-op/My Soldier/Extras), pushed screens get PC variants once it is up.
  The PC KEY BINDINGS tab (empty KB/mouse columns on Xenon) opens PC SETTINGS instead.
- Main menu edits (bf_main/main_menu.cpp): mainmenuscreen StaticItems (0x28 B per item once loaded, count at -4):
  STORE removed, ID_M_EXTRA strings renamed EXIT (unknown IDs show as-is); ExtraMenuScreen push = quit flag.
  Flow graphs: scripts/ebx_dump.py <ebx> (Xenon EBX: u32 header, instances each preceded by a GUID, arrays after
  the instance payload) and scripts/ui_graph.py <dump> <port> (follow UINodeConnections from a port).
- PRACTICE (main menu): the hidden DEV MENU item (+0x25 ExcludeInRetail cleared, strings renamed in place) opens
  DevScreen, DICE's level select (SP / MP / co-op). Its LoadLevel action (table 0x84016150: key 0xE9F0E7F5 ->
  factory sub_836DE440 -> execute sub_836EE268(action, params: char*[] {level, mode})) restarts the server, which the
  client can't join while it loads (drops after ~7 s); MP levels are instead switched in place on the running server
  (level_boot.cpp RequestLevel, Team Deathmatch). Result: PRACTICE -> OPERATION METRO -> client ingame, TDM spawn
  screens pushed, but the screen stays black (only the loading indicator). Server backend fallback (no Blaze) is
  unconditional now; level heap 160 MB by default.
- In-level black screen (PRACTICE -> Metro), FIXED 2026-10-25: four causes, found with the GPU frame tracer (below).
  1) resolve of the 7e3 HDR scene to 16_16_16_16 UINT exp_bias 11 saturated (0xFFFF); 2) the integer texture fetch
  of it was normalized; 3) the CPU-read luminance resolve wasn't read back; 4) the level's FadeEntity starts faded and
  its FadeIn never fires (fade_fix.cpp). Frame layout (Metro, 1280x704 internal, ~990 draws, 76 resolves): G-buffer
  4 RTs at EDRAM 0 resolved to 0C2DC000.. (normals, albedo, ...), depth 10D48000, lighting into a 7e3 RT (format 12)
  resolved to 0C2DC000 (HDR, uint * 2^11), bloom/luminance chain 0D2CC000.., log-luminance 32x1 -> 1E364000 (CPU
  read), tonemap PS 4909D8C0B44D6AF8 samples HDR, bloom, vignette (k_8 at 10400000) and the 32^3 color grading LUT
  at 1E343000 (built on the CPU by the job sub_83C8EB28: blend source LUTs, 4x4 color matrix + 3 curves in
  sub_83C8E808, tile, copy), UI drawn after the composite, frontbuffer 11478000.
- GPU frame tracer (patch 0002, cvars): `--gpu_frame_trace_at=<s>` (after the first swap) logs every draw (shader
  hashes, RT/blend/depth regs, fetch constants of used textures, PS constants c0-c15/c252-c255 for rect lists) and
  resolve of one frame (`gpu_frame_trace_every`, `gpu_frame_trace_frames`); `--gpu_frame_trace_dir=<UNC dir>` dumps
  each resolve's destination; `--gpu_frame_trace_ranges=addr:len,...` dumps memory at trace start;
  `--gpu_frame_trace_probe=true` also dumps float/fixed16 color resolves as 16_16_16_16_FLOAT (pre-conversion
  values); `--gpu_watch_range=addr:len` logs resolves, memexports and CPU-write invalidations touching a range.
  `scripts/ftrace_view.py <log> <dump dir> [png dir]` untiles the dumps, prints channel stats and writes PNGs plus a
  labelled contact sheet. `scripts/practice_run.sh [secs] [level] [args]` = the PRACTICE -> Metro recipe (in level at
  ~95 s). `--dump_shaders=<dir>` writes ucode disassembly (shader_<hash>.ucode.frag).
- PRACTICE deploy (2026-10-25): with the team set, the spawn screen shows the kit (ASSAULT: M16A3, M9, medic kit,
  defibrillator, squad sprint) and clicking DEPLOY spawns the soldier (pre-round "WAITING FOR PLAYERS, 7 needed").
  Deploy path: UI action sub_836EE788 -> sub_837E0C90(spawn system = UI manager sub_83280730() vtable +0xD0
  (0x84197820, 1), kit, flag) -> NetworkSelectTeam (flag 0, no team) / PlayerSelectedWeapon x3 /
  SelectedUnlockAssets / SpawnOnSelected; server: sub_836C7AA0 spawns if player+0x3B4 == 0 and sub_8342C7D8(conn).
  BF3_AUTO_DEPLOY=<kit> deploys from the host 8 s after the spawn screen (tests). With the solo round start the
  soldier walks, turns, fires and reloads (pad script sticks: `LX=/LY=/RX=/RY=<-32768..32767>`, e.g.
  `6000:LY=32767:3000;500:RT:1500`).
- Open: level load hung once at the loading screen (14:42, BF3_CFG BFServer.* lines set; the rerun loaded).
  BFServerSettings (BFServer.RoundStartPlayerCount etc.) don't reach the pre-round entity. BF3_MSG_TRACE=2 logs the first 3
  of every message type at the dispatcher (scripts/msg_names.py names them); lr 83C529EC = client->server delivery
  on the server, 83C51AE0 = server->client delivery.
- PRACTICE launcher (src/pc_practice.cpp, host screen): opens over DevScreen when PRACTICE is clicked. Map (the 9
  Disc 1 MP maps), game mode (TeamDeathMatch0, SquadDeathMatch0, ConquestLarge0, RushLarge0, SquadRush0), team; LAUNCH
  -> bf3_practice_set_team + bf3_practice_launch (DLL exports; in-place level switch). DEVELOPER MENU keeps DICE's
  screen, BACK pulses B. BF3_PRACTICE_AUTO=<map id>:<mode>:<team> launches by itself 2 s after opening (tests).
  Verified: Metro TDM, Grand Bazaar TDM, Metro Conquest (RU). Host screens that block game input also stop the menu
  mouse (pad_inject GameInputBlocked).
- UI screen stack: the UI manager (sub_8328B990's r3) keeps the live screens at +0x0C..+0x10 (screen +0x18 name);
  ui_screens.cpp exports bf3_ui_screen_list (ui_state::LiveScreens), BF3_UI_TRACE=1 logs it on change. pc_mouse
  uses it: mouse look only with HudScreen up and no Menu/Spawn/Popup/Options/BackScreen screen (the last pushed
  screen is TabListenerScreen on the deploy screen and DefaultTrackingScreen in gameplay, so it can't tell).
  In-game pause menu: IngameMenuMP.
- Audio capture (src/audio_capture.cpp): BF3_AUDIO_CAPTURE=<UNC wav path>[:<seconds>] records the frames the game
  submits (XAudioSubmitRenderDriverFrame import of bf3_BF_Main.dll: 256 samples x 6 channels, planar big-endian
  float, 48 kHz) into a 6-channel float WAV, and logs frames/s, silent frames and peak per second. Metro capture
  (2026-10-25): steady ~188 frames/s, no frame-boundary discontinuities, repeats or dropouts; FL/FR/BL/BR carry the
  mix, FC and LFE are always 0. Output stage (rexruntime SDL driver): 6 ch to an 8 ch Realtek endpoint.
  XMA: "write to unknown register 0x601" ~400/s (Xenia logs it too).
- Performance (2026-10-25, Metro gameplay, RX 7800 XT): GPU thread mostly idle; main thread waits on jobs
  (sub_83007B50 in client update sub_831FDC08); job workers spend much time in NtSetEvent (jobs signalling
  completion via sub_838D16A8) and ObjectTable::LookupObject, both on rexruntime's global_critical_region mutex.
  Next step for more FPS: a runtime with a finer object-table lock (rexruntime is prebuilt; source in tools/rexglue-src).
  Thread sampling: --thread_dump_at=<s> --thread_dump_every=1, then scripts/resolve_rva.py.
- GPU sync slots: sub_82FD6968 allocates a slot (array at device+0x5F70), stores 0, queues WAIT_REG_MEM(slot ==
  FFFFFFFF); sub_82FD6B78 writes FFFFFFFF when the resource's CPU user is done (slot ptr at resource+0x80).
- Guest write watch: BF3_WATCH=<guest addr> BF3_WATCH_AT=<s> BF3_WATCH_ANY=1 logs the host stacks of the first
  writes to an 8-byte word through any view (found the LUT writer). BF3_LUT_TRACE=1 (lut_trace.cpp) logs the LUT jobs
  and their color matrices, BF3_FADE_TRACE=1 every fade request, BF3_CFG="Setting value;..." adds Game.cfg lines
  (GameRender.FadeEnable/FadeWaitingEnable false did not help).
- Open: ROV path (--render_target_path_d3d12=rov) and --async_shader_compilation=false crashed during level load
  (null read in sub_8314C4A8 from sub_8314F478), probably a timing race; not investigated.
- Xbox LIVE removed: no sign-in spoofing (live_emulation.cpp deleted). Local online stack (bf_main/local_online.cpp,
  BF3_LOCAL_ONLINE=0 off): flow data 0x9E2114F5 (online state; sub_837F0090 from online manager 0x840D63D0+0x20)
  answered "Online"; 0x938E1FD9 HasStats (mysoldiergraph) true; player name (sub_830271A0 "xenon-retail" machine
  name, used by the name getter sub_83704EB0) = soldier name. Local profile (src/local_profile.cpp):
  user_data_root/soldier_profile.txt, SOLDIER PROFILE prompt on first online section, XamUserGetName/SigninInfo
  answer the name. MY SOLDIER: stats, progression, customize, dog tags, co-op stats all open (fresh rank 0).
  MULTIPLAYER / CO-OP then stop at "Please Insert Multiplayer/Co-op Disc" (disc detection; Campaign asks for Disc 2).
  BF3_DATA_TRACE=1 logs flow data reads (key, type, value).
- Contact sheets: `scripts/contact_sheet.sh <first> <count> <out.png> [cols]` (ffmpeg tile of logs/shots).
- Lua: only config/startup (Scripts/Game/Startup.lua, getTableAsString); BF3_LUA_DUMP=<dir> dumps chunks
  (src/bf_main/lua_dump.cpp hooks luaL_loadbuffer sub_838F7B38). Menus are Scaleform (instance1.pushScreen).
- STATUS 2026-10-25 23:00: MAIN MENU reached (CAMPAIGN, MULTIPLAYER, CO-OP, MY SOLDIER, OPTIONS, STORE, EXTRAS; Battle
  Feed "YOU ARE OFFLINE"). Title screen at ~50 s, START -> main menu. Scripted input:
  `BF3_PAD_SCRIPT="55000:START;60000:START;66000:A" scripts/win_run.sh 95 info` (src/pad_inject.cpp hooks
  bf3_BF_Main.dll's import of __imp__XamInputGetState once the DLL loads; ms counted from the first input poll).
  PC keyboard/mouse defaults in src/pc_input.cpp (mnk_mode + mnk_mouse, BF3-PC-style binds).
- STATUS 2026-10-25: boots to the title screen ("PRESS START TO BEGIN") with the patched GPU plugin; no job-system
  errors. The memcpy-hook workaround (src/gpu_readback_fix.cpp) was removed, superseded by the plugin patch.
- STATUS 2026-10-24 (end of session 1): boots through launcher -> BF.Main -> Engine.BuildInfo, renders the loading
  screen (tips, LOADING) via D3D12, loading screen fades out at ~13 s, then black + no logs (probable hang in/after the
  job system). Next: find the waiter (dump guest thread states at the hang; see who spins/waits on parent+48 or
  sub_8346BAE0's completion), and verify the job-skip assumption (who sets job.hi bit 0).
