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
- SDK: `tools/rexglue-sdk` (v0.10.0 stable, linux-amd64, runs codegen) and `tools/rexglue-sdk-win` (v0.10.0,
  win-amd64, headers/libs for the cross build). Installed 2026-10-24 from the GitHub release, sha256 checked:
  linux `0388edb3...59328a6`, win `ad4485da...e0c11b`. Nightlies (0.10.0.x-dev) exist; not used.
- xwin 0.10.0 (`tools/xwin-bin/`, sha256 checked) splatted to `tools/xwin/` (CRT 14.44.35220, Windows SDK
  10.0.26100, same files as the Trials HD setup). Cache in `tools/xwin-cache/`, log `logs/xwin-splat.log`.
- `tools/cross/` (committed): `win-amd64-xwin.cmake` toolchain + `rexglue/` wrapper package (Windows SDK config,
  codegen pointed at the Linux binary), copied from Trials HD. Gotchas in the reference doc. Not yet exercised here.
- GhidraMCP: reachable from WSL (mirrored networking). Next phase: import `dump/Disc 1/game/Default.xex` and
  `BF.Main.xex` (then BF.Splash, Engine.BuildInfo) with XEXLoaderWV, confirm image md5s/bases above with
  `get_program_info`, then find how the launcher loads the DLL modules (XexLoadImage / XexLoadExecutable imports).
- Ghidra project: `C:\Users\Dustin\BF3-RECOMP` (user-created). `/modules/` = the four .xex imported with
  XEXLoaderWV, unanalyzed, untouched. `/merged/BF.Main.xex` = BF.Main + Default + Engine.BuildInfo at their real
  addresses + BF.Splash in overlay space `BF_Splash` (addresses `BF_Splash::83480000`): 38 blocks, 66770 functions
  (from .pdata), labels/imports copied. Not yet auto-analyzed. Open `/merged/BF.Main.xex` for GhidraMCP.
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
Headless gotchas: `-postScript` takes a script name only, and `C:\Users\Dustin\ghidra_scripts` wins over
`-scriptPath`, so repo scripts carry a `BF3` prefix. The wrapper exits 1 on any ERROR line, including the
harmless LZX one: read the log. Imports take about 1 min; the program image base stays 0 (XEX base is not applied).
Nothing else verified yet for this project. `docs/reference/trialshd_CLAUDE_SKILL.md` has the forms that worked for
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
