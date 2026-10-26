// Debug API (development builds only: BF3_DEV_TOOLS, see CMakeLists.txt): a command server on 127.0.0.1 that tests
// and tools (scripts/bf3dbg.py) use to drive and inspect the game. See debug_api.cpp for the protocol and commands.
#pragma once

namespace bf3::dev {

// Starts the server thread (port 4790; BF3_DEBUG_API=<port> picks another, BF3_DEBUG_API=0 turns it off).
void Install();

}  // namespace bf3::dev
