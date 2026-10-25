// Guest controller input hook (see pad_inject.cpp): input lock for settings changes, blocking game input while a host
// screen is open, and scripted test input (BF3_PAD_SCRIPT).
#pragma once

#include <shared_mutex>

namespace bf3 {

// Redirects bf3_BF_Main.dll's XamInputGetState import once the module loads.
void InstallPadInject();

// Held shared around every guest input poll. ReXGlue's keyboard driver copies the keybind_* strings on each poll, so
// changing them needs this exclusively (unguarded writes corrupted the heap in the Trials HD port).
std::shared_mutex& InputLock();

// While set, the game sees an idle controller (a host screen such as PC Settings is open).
void SetGameInputBlocked(bool blocked);

}  // namespace bf3
