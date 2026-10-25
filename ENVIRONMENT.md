# Battlefield 3 Static Recomp + GhidraMCP Development Environment Setup

**Working directory:** `/home/dustin/workspace/bf3-recomp` (WSL Arch Linux, or any Linux)

**Goal:** Set up a clean environment for AI-assisted static recompilation of `Battlefield 3` (`Xbox 360`, title ID
TBD) with `rexglue` v`0.10.0`. Codegen and builds run on Linux; the target is Windows x64, cross-compiled
(or built natively on Windows). Ghidra + the platform loader + GhidraMCP run on the host; Claude reaches them
over the network (WSL mirrored networking).

**Legal note:** Only use a dump of a copy you legally own. Never commit, copy elsewhere, or distribute game files.

---

## 1. Directory Layout

```
/home/dustin/workspace/bf3-recomp/
├── tools/                  # recompiler SDK(s), cross toolchain, sysroots
│   ├── rexglue-sdk/       # Linux SDK (runs codegen)
│   ├── rexglue-sdk-win/   # Windows SDK (headers/libs for the target build), if separate
│   ├── xwin/               # MSVC CRT + Windows SDK for cross-builds
│   └── cross/              # toolchain file + wrapper packages
├── dump/                   # extracted game files (default.xex + assets). NEVER committed
├── project/                # recompiler project (manifest, CMake, generated C++, src/ hooks)
├── scripts/                # analysis + deploy/test helpers
├── docs/                   # CLAUDE_SKILL.md: verified commands, manifest syntax, fix log
├── logs/                   # build, codegen and runtime logs
├── CLAUDE.md
└── README.md
```

```bash
mkdir -p /home/dustin/workspace/bf3-recomp/{tools,dump,project,scripts,docs,logs}
cd /home/dustin/workspace/bf3-recomp && git init
printf 'dump/\nlogs/\nproject/out/\ntools/\n' > .gitignore
```

---

## 2. System Dependencies (Arch Linux / WSL)

```bash
sudo pacman -Syu --noconfirm
sudo pacman -S --noconfirm base-devel cmake ninja clang llvm lld git wget unzip python python-pip
# Optional
sudo pacman -S --noconfirm jdk21-openjdk   # only if running Ghidra in Linux/headless
```

Verify:
```bash
clang --version   # whatever the recompiler requires (ReXGlue 0.10: 20+; newer clang works with a user preset)
cmake --version   # 3.25+
ninja --version
python --version  # 3.10+ for the helper scripts
```

If the SDK's stock CMake presets hardcode a compiler (e.g. `clang++-20`), don't edit them: add a user preset
(section 6) that inherits the stock one and overrides the compiler.

---

## 3. Download and Install the Recompiler SDK

Pick the recompiler for the source platform. Check each project's current README; tooling changes quickly.

| Platform | Executable | Extract with | Recompiler | Ghidra loader |
|---|---|---|---|---|
| Xbox 360 | `default.xex` | Velocity, wxPirs (XBLA/STFS), extract-xiso (disc) | ReXGlue (XenonRecomp lineage) | XEXLoaderWV |
| N64 | `.z64` ROM | none | N64Recomp + N64ModernRuntime | N64LoaderWV |
| GameCube/Wii | `main.dol` / `.rel` | Dolphin (Extract Entire Disc) | check current options | Ghidra-GameCube-Loader |
| PS2 | `SLUS_xxx.xx` ELF | any ISO tool | check current options (PS2Recomp) | ghidra-emotionengine-reloaded |
| Other | | | search for the current community recompiler | |

Example (ReXGlue):
```bash
cd /home/dustin/workspace/bf3-recomp/tools
wget https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-linux-amd64.zip
unzip rexglue-sdk-0.10.0-linux-amd64.zip && mv rexglue-sdk-0.10.0-linux-amd64 rexglue-sdk
# Windows target SDK (headers/libs for the cross build):
wget https://github.com/rexglue/rexglue-sdk/releases/download/v0.10.0/rexglue-sdk-0.10.0-win-amd64.zip
unzip rexglue-sdk-0.10.0-win-amd64.zip && mv rexglue-sdk-0.10.0-win-amd64 rexglue-sdk-win
```

Test:
```bash
tools/rexglue-sdk/bin/rexglue --help     # record the real flags; they differ between versions
```

**Windows builds**, either:
- **Cross-compile from Linux** (recommended: one machine drives every build): clang `--target=x86_64-pc-windows-msvc` +
  lld-link, with the MSVC CRT and Windows SDK from xwin:
  ```bash
  cargo install xwin   # or download a release binary
  xwin --accept-license --arch x86_64 splat --output tools/xwin
  ```
  Toolchain file in `tools/cross/`. Known gotchas: pass the xwin include dirs with `-idirafter`, not `-isystem`
  (otherwise MSVC's intrinsics headers shadow clang's and `_mm_*` symbols go undefined at link); use the release
  CRT only (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`, try-compile in Release), since xwin ships no debug CRT.
  If codegen runs as part of the build, point the codegen tool at the Linux binary while headers/libs come from
  the Windows SDK (a small wrapper CMake package in `tools/cross/`).
- **Or build natively on Windows** with Visual Studio 2022 + the clang-cl toolset and the Windows SDK zip.

---

## 4. Game Dump

1. Extract the game from your own media/package with the platform's tool (section 3 table).
2. Copy the extracted files into `/home/dustin/workspace/bf3-recomp/dump/`.
3. Confirm `dump/default.xex` is the real executable (not the container), and record its hash:
   ```bash
   md5sum dump/default.xex    # every address in the docs is only valid for this exact build
   ```
4. If the executable is encrypted/compressed (XEX, SELF, NSO), write `scripts/default.xex_image.py` to produce a flat
   decrypted image (`dump/default.xex.image.bin`, mapped at the load base) for scripted scans.

---

## 5. Ghidra + Platform Loader + GhidraMCP (host)

1. Install Ghidra (12.x) + JDK 21.
2. Install the platform loader: **File → Install Extensions → +** → loader zip → restart.
3. Create a project and import the **same** `default.xex` as `dump/` (compare hashes). Load symbols/PDB if any exist.
4. Run Auto Analysis. Note sections Ghidra covers poorly (packed/embedded code) in `docs/CLAUDE_SKILL.md`.

**GhidraMCP:** install the GhidraMCP extension and bridge, start its HTTP server (default port 8080), and
register the MCP server with Claude Code. With mirrored networking, WSL reaches it at `localhost`.
Check with `get_program_info` that the loaded program is the right binary.

---

## 6. Initialize the Recompiler Project

Example (ReXGlue 0.10; use absolute paths, relative ones resolve oddly):
```bash
cd /home/dustin/workspace/bf3-recomp
tools/rexglue-sdk/bin/rexglue init --project-name bf3 \
  --xex-path $PWD/dump/default.xex --game-root $PWD/dump --project-root $PWD/project
```

Add `project/CMakeUserPresets.json` (ours; the stock `CMakePresets.json` stays untouched):
- `local-debug`: inherits the stock Linux debug preset, sets `CMAKE_C_COMPILER=clang`,
  `CMAKE_CXX_COMPILER=clang++`, `CMAKE_PREFIX_PATH=/home/dustin/workspace/bf3-recomp/tools/rexglue-sdk`, and a `-j` sized to RAM
  (recompiled games make huge translation units; about 2GB per job).
- `win-cross-release`: the cross toolchain file from section 3, Release.

Codegen and build:
```bash
cd project
cmake --preset local-debug
cmake --build --preset local-debug --target bf3_codegen > ../logs/codegen.log 2>&1
cmake --preset local-debug          # RE-CONFIGURE: codegen writes the source list the build includes
cmake --build --preset local-debug > ../logs/build.log 2>&1          # Linux compile check
cmake --preset win-cross-release
cmake --build --preset win-cross-release > ../logs/build-win.log 2>&1
```
A missing reconfigure shows up as undefined image-config symbols at link (ReXGlue: `PPCImageConfig`).

---

## 7. Claude Instructions

Create `/home/dustin/workspace/bf3-recomp/CLAUDE.md` (short: layout, build commands, rules) and `/home/dustin/workspace/bf3-recomp/docs/CLAUDE_SKILL.md`
(long: verified commands, manifest syntax, fix log, findings). The `game-static-recomp` skill covers the workflow.

### docs/CLAUDE_SKILL.md skeleton

**Purpose:** Assist with static recompilation of `Battlefield 3` using `rexglue`, Ghidra, and GhidraMCP.

**Environment:**
- Working dir: `/home/dustin/workspace/bf3-recomp`
- Executable: `dump/default.xex` (md5 `TBD`, same file loaded in Ghidra)
- Project: `project/` (manifest: `project/bf3_manifest.toml`)
- SDK: `tools/rexglue-sdk` (v`0.10.0`)
- GhidraMCP reachable from WSL

**Typical tasks:**
- Register functions codegen missed (vtable/callback targets, thunks) in the manifest.
- Declare setjmp/longjmp, switch tables, and invalid-instruction data regions.
- Write stubs / early returns for unused systems.
- Debug crashes by correlating crash traces, generated C++, and Ghidra decompiler output.
- Add PC features (input, menus, settings) as host-side overrides of guest functions in `src/`.

**Rules:**
- Never distribute or request game assets.
- Prefer minimal, reversible changes (manifest entries, stubs) over rewrites. Never hand-edit `generated/`.
- Always cite the guest address when proposing a fix, and log it in the fix log.
- Query GhidraMCP for specific addresses or functions; never dump the whole binary.
- Boots and tests run on the target machine. Write nothing there except the exe + required DLLs in `TBD (Windows host dir)`.

**Fix log:**
| Address | Fix | Reason |
|---|---|---|

---

## 8. First Run Checklist

- [x] `rexglue --help` works and the real flags are recorded
- [x] `dump/default.xex` is present, valid, and its hash recorded (here: `dump/Disc N/game/`, see docs)
- [ ] Project initialized; manifest points at the executable
- [ ] Codegen produces C++ under `project/generated/`
- [ ] Linux compile check links (after reconfigure)
- [ ] Windows cross-build produces `bf3.exe` + runtime DLLs (check imports with `llvm-readobj --coff-imports`)
- [x] Ghidra loads the same executable and analysis finds functions
- [ ] GhidraMCP is reachable from WSL
- [ ] `scripts/win_run.sh` deploys only to `TBD (Windows host dir)`, runs with logs pointed back into `logs/`, and
      refuses to run while the game is already open

---

## 9. Next Steps After Setup

1. Get a binary that starts, even if it crashes immediately.
2. Fix the first fatal error class (usually unregistered functions); sweep for the whole class with a script
   instead of fixing one address per run.
3. Add a crash tracer + RVA-to-guest-function resolver so every crash names a guest function.
4. Iterate to the main menu, then to the user's "done" definition.
5. Add PC features (input, menus, settings) through `src/` overrides.
6. Keep `docs/CLAUDE_SKILL.md` current after every fix.

---

**References:**
- ReXGlue: https://github.com/rexglue/rexglue-sdk (wiki: Getting Started)
- N64Recomp: https://github.com/N64Recomp/N64Recomp
- xwin: https://github.com/Jake-Shadle/xwin
- XEXLoaderWV / N64LoaderWV: https://github.com/zeroKilo
- GhidraMCP: https://github.com/LaurieWired/GhidraMCP
