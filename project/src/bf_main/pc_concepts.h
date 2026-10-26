// PC keys -> BF3 input concepts in the local player's entry input (see pc_concepts.cpp).
#pragma once

namespace bf3_main {
// The local player's last mapped entry is the soldier (not a vehicle seat); mouse_aim.cpp only aims then.
bool LocalOnFoot();
}  // namespace bf3_main
