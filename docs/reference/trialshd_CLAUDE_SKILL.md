# Skill: Trials HD ReXGlue Recomp

**Purpose:** Assist with static recompilation of Trials HD (XBLA, Title ID 5841095A) using ReXGlue, Ghidra, and GhidraMCP.

## Environment
- Working dir: `/home/dustin/workspace/trialshd-pc`
- XEX: `dump/default.xex` (md5 `6108f17a1efd1a557e797d5246301df3`, same file loaded in Ghidra)
- Project: `project/` (manifest: `project/trialshd_manifest.toml`)
- SDK: `tools/rexglue-sdk` (v0.10.0, linux-amd64)
- GhidraMCP: reachable from WSL (mirrored networking)
- Toolchain: system clang 23 (preset hardcodes `clang++-20`; override on configure)

## Commands (verified against v0.10.0)
```bash
# init (flags differ from older docs; use absolute paths, relative ones resolve oddly)
tools/rexglue-sdk/bin/rexglue init --project-name trialshd \
  --xex-path $PWD/dump/default.xex --game-root $PWD/dump --project-root $PWD/project

# Use the `local-debug` preset from project/CMakeUserPresets.json. It inherits linux-amd64-debug
# and sets clang/clang++ (the stock preset hardcodes clang++-20 and resets the cache when re-run)
# plus CMAKE_PREFIX_PATH for the SDK and -j8 (15GB RAM in WSL).
cmake --preset local-debug                                     # from project/
cmake --build --preset local-debug --target trialshd_codegen > ../logs/codegen.log 2>&1
cmake --preset local-debug   # RE-CONFIGURE after the first codegen so generated/default/sources.cmake is included
cmake --build --preset local-debug > ../logs/build.log 2>&1
# binary: project/out/build/local-debug/trialshd
```
Symptom of a missing reconfigure: link fails with `undefined reference to PPCImageConfig`.

## Manifest syntax notes
- Function boundaries go under the entrypoint module, not top-level:
  ```toml
  [entrypoint.functions]
  0x821C3170 = { size = 0x8 }   # or { end = 0x... }; not both
  ```
- Other per-module tables seen in the binary: `[[switch_tables]]` (address, register, labels),
  `[[invalid_instructions]]` (data, size), `[[midasm_hook]]` (address, name, return_on_true/false,
  jump_address*, after_instruction), `setjmp_address`, `longjmp_address`, `exception_handler_funcs`,
  `analysis.max_jump_extension`, `analysis.data_region_threshold`, `analysis.large_function_threshold`.

## Rules
- Never distribute or request game assets.
- Prefer minimal, reversible changes.
- Always cite the original address or function name when proposing a fix.
- Prefer early returns / stubs over large rewrites for unused paths.
- When using GhidraMCP, query specific addresses or function names rather than dumping entire binaries.

## Fix log
| Address | Fix | Reason |
|---|---|---|
| 0x821C3170 | function, size 0x8 | MI this-adjustor thunk (`addi r3,r3,8; b 0x821C19C0`) in thunk row 0x821C3158..; branched to from 0x821C317C |
| 0x8221C0F8 | function, size 0x20 | list-insert leaf; tail call from FUN_82226ef0 @ 0x82226EF8 |
| 0x821DD448 | function, size 0x28 | state-setter leaf (+0x23C); tail call from stub @ 0x821CBAF8 |
| 0x821E0938 + 60 more | functions, sizeless `{}` | Runtime FATAL "unregistered function 0x821E0938": MI thunks/leaves reachable only via vtables. Found by `scripts/find_unregistered.py` (data-section pointer sweep; SEH scope tables at 0x820CE1DC.. excluded, since their end/target fields are interior labels) |
| 0x820F0868 + 15 more | functions, sizeless `{}` | Runtime FATAL "unregistered function 0x820F0868": callbacks/vcall thunks whose address is built with lis+addi/ori. Same script; relative-switch bases (lis/addi then bctr within 4 insns) excluded |
| setjmp 0x822FA320 / longjmp 0x822FA000 | `setjmp_address` / `longjmp_address` in `[entrypoint]` | Null read of guest 0x1A4 in libjpeg read_markers (sub_8232B608): XUI decodes xboxlivelogo.png with libjpeg first; JERR_NO_SOI -> error_exit -> longjmp, which without config restored regs from jmp_buf and returned (r31 = 0) |

## Known notes
- Fixing boundaries: prefer sizeless entries (`0xADDR = {}`) and let block discovery size them. After any sweep,
  check codegen.log for new "Unresolved ... from" lines: they mean an entry split an existing function.
- libjpeg in the binary: consume_markers = sub_82330F80, read_markers = sub_8232B608, first_marker = sub_8232B528.
- Game XUI uses a custom `lol://` protocol backed by the .pak archives. NtCreateFile failures for `game:\data\Xui\...`
  (.xus, en-us variants) are optional loose-file probes, not errors.
- 0x826CD1A0 (`.embsec_`, ~25KB VMX code) emits ~1.3MB of C++, exceeding max_file_size_bytes. Legit large function, warning only.
- Ghidra has little/no function coverage in the `.embsec_` sections (0x8269EE00-0x826F7EC7).

## Running / testing
- `scripts/win_run.sh [secs] [level]` deploys the Windows build (exe + rexruntime/rexgpu-xenos DLLs, only if
  changed) and runs it on the host. Log: `logs/win-run.log`, user data: `logs/win-userdata` (via \\wsl.localhost).
  Use `grep -a`: the log can contain binary bytes. At debug level it rotates quickly (XMA 0x601 spam).
- Crash tracing: `src/crash_trace.h` logs a host stack walk for guest null-page faults (`[crash_trace]` lines).
  win_run.sh resolves the frames to `sub_XXXXXXXX` names with `scripts/resolve_rva.py` and `trialshd.map`.
- `dump/default.image.bin` is the decrypted/decompressed image (from `scripts/xex_image.py`), mapped at 0x82000000.
- Boots and tests run on the **Windows host**. WSL is only for codegen, analysis, and compile checks.
- (WSL runtime isn't used: no Vulkan ICD, and it hangs after `SDL video driver: x11`.)

## Windows build (cross-compiled from WSL)
Toolchain: clang 23 `--target=x86_64-pc-windows-msvc` + lld-link, MSVC CRT/Windows SDK 10.0.26100 from xwin.
```bash
cd project
cmake --preset win-cross-release                                  # CMakeUserPresets.json
cmake --build --preset win-cross-release > ../logs/build-win.log 2>&1
# -> out/build/win-cross-release/{trialshd.exe, rexruntime.dll}
cp out/build/win-cross-release/{trialshd.exe,rexruntime.dll} "/mnt/c/Users/Dustin/Downloads/Trials HD/"
```
Pieces:
- `tools/rexglue-sdk-win/`: Windows SDK, copied from Downloads.
- `tools/xwin/`: CRT + SDK, `xwin --accept-license --arch x86_64 splat --output tools/xwin`.
- `tools/cross/win-amd64-xwin.cmake`: the toolchain file. Gotchas:
  - xwin headers must be `-idirafter`. With `-isystem`, MSVC's `<emmintrin.h>` shadows clang's and the link fails on `_mm_*` undefined symbols.
  - Release CRT only (`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`, try-compile in Release); xwin has no `msvcrtd.lib`.
- `tools/cross/rexglue/`: wrapper package that loads the Windows SDK config but points `rex::rexglue` (codegen) at the Linux binary.
- **Local SDK patch:** in `tools/rexglue-sdk-win/include/simde/x86/sse.h:4757`, `simde_mm_stream_pi` skips the
  `__builtin_nontemporal_store` path when `_MSC_VER` is defined, because clang 23 rejects it for the `__m64` union.
  The original is saved as `sse.h.orig`. Re-apply after any SDK update.

Runtime deps (checked with `llvm-readobj --coff-imports`): `rexruntime.dll` plus system DLLs and the VC++ 2015-2022
redist (installed on the host). Release doesn't link TracyClient. No GPU plugin is configured (`rexgpu-xenos` isn't staged).

## Windows-side rule
The ONLY things written outside WSL are `trialshd.exe` and the DLLs it needs, in `C:\Users\Dustin\Downloads\Trials HD\`.
Game files on the host are in `C:\Users\Dustin\Downloads\Trials HD\Data` (contains default.xex).

## PC input
- Keyboard: `src/pc_input.h` turns on ReXGlue's MnK controller emulation and sets PC keybinds (arrows = D-pad,
  Enter/Space = A, Esc/Backspace = B, X/Y, P = Start, Tab = Back). Only flags still at their default are changed.
- Mouse: `src/xui_mouse.cpp` hooks the per-frame UI tick `sub_826634C0` (strong definition overrides the weak
  recompiled alias; the original is `__imp__sub_826634C0`). Hover focuses controls, left click = A on a control,
  right click = B, wheel = D-pad. Carousel menus (track/bike lists: focus parked on an off-screen XuiButton, rows
  are equal-size XuiGroups, selected row scaled > 1) use click-to-select / click-selected-to-activate.
  `--xui_mouse=false` disables it. Win32 cursor/buttons/wheel are in `src/host_mouse.cpp`.
- Game XUI internals (statically linked, no XAM Xui imports) are documented in `src/xui_guest.h`: handle table
  0x8272A430 (pages of 512 x 12-byte entries), focus table 0x8272A2E4, XuiProcessInput = sub_82183158,
  SetFocus = sub_82184500, element layout (pos +0x30, size +0xC, scale +0x3C, tree links +0x18..+0x28).
- Input path: thread sub_82663EA8 polls XamInputGetKeystrokeEx -> queue 0x82743ED0 -> sub_82664288 dequeues
  one per frame in sub_826634C0 -> sub_82183158.

## UI test tooling
- `SHOTS=<ms> scripts/win_run.sh ...` saves game-window screenshots (PrintWindow, window only) to `logs/shots/`.
- `--xui_mouse_sim='frames,x,y,btn;...'` replays a scripted mouse in 1280x720 canvas coords (btn - L R U D).
- `--xui_probe` dumps the focused element, its ancestors and the active scene tree on each focus change
  (and injects D-pad presses unless a sim is running). Add `--log_flush_interval=1` so logs survive the kill.
- XUI SendMessage = sub_8218F7A0(h, msg). Overridden in xui_mouse.cpp: a stale handle returns E_INVALIDARG instead of
  dereferencing null. Seen once as a crash on guest thread 0xF8000208 during fast real-mouse hovering (the thread died,
  so the game kept rendering with dead input). Stale-handle hits are logged as "SendMessage to stale handle".
- Open: an idle run once exited by itself with code 116 (log lost). Watch for it.

## KEY BINDINGS menu
- `src/xui_menu_inject.cpp`: clones the last MainMenu XuiNavButton (visual "Trials-Button") into a new
  KEY BINDINGS button (Id KeyBindingsButton) one row lower and links NavUp/NavDown. It overrides XuiProcessInput
  (sub_82183158): A on that button opens the screen, and all game input is swallowed while the screen is open.
- `src/keybind_menu.cpp`: an ImGui dialog (ReXApp::OnCreateDialogs). Fonts are Impact and Bahnschrift from
  %WINDIR%\Fonts, added in OnConfigureFonts (the renderer has no dynamic font textures). Saving writes ONLY the
  keybind_* lines of exe_dir/trialshd.toml. Never use rex::cvar::SaveConfig: it also persists command-line
  overrides such as log_file and user_data_root.
- XUI API found: XuiCreateObject sub_82193378(name,&h); AddChild sub_82180BA8; GetProperty sub_8218BFD0 and
  SetProperty sub_8218C0D8 (h, id, index, XUIElementPropVal*); GetChildById sub_82181268; SetText sub_821818E8;
  property defs live in class info +0x1C/+0x20 (see xui_guest.h).

## Input-test caveats
- Injected keys (keybd_event) reach window keybinds (F4 works) but NOT the MnK controller emulation: MnK gates on
  its has_focus_ flag and only feeds pad state, which ReXGlue turns into keystrokes. Keyboard menu navigation
  therefore can't be verified by automation. Ask the user. Mouse (our own code) and the ImGui screen can be.
- Use `--mnk_mode=false` and `--xui_mouse_sim` for isolated runs so the user's own input can't drive the game.
  Launching the game steals focus from whatever the user is typing in.
- `scripts/win_input.ps1` sends real clicks/keys (m:/c:/r:/k:/w: steps, 1280x720 coords) and briefly takes the
  cursor. If a test saves bindings, delete trialshd.toml afterwards.
- `src/input_guard.cpp`: the game's XAM input wrappers (sub_82124668/670/678/688/750) hold a shared lock;
  KEY BINDINGS Save takes it exclusively while rewriting keybind_* cvars. MnK copies all 25 keybind strings on
  every poll (~1000/s from the guest input thread), so unguarded writes corrupted the heap: crashes
  0xC0000374 (ntdll) and an AV in rexruntime vector<DeviceInfo>::clear. OnShutdown sets a flag so guest polling
  stops before the runtime tears down input (the shutdown AV). ReXGlue's own F4 settings overlay still has the
  unguarded race.
- The keybind screen allows shared keys (riding vs menus) and highlights them in gold; it never unbinds other actions.
- Windows crash details: Application event log (Id 1000, "Application Error"), read with Get-WinEvent. Logs
  buffered at the time of a hard crash are lost.
