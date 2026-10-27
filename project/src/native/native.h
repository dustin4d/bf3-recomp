// Native (decompiled) code: functions that replace recompiled BF.Main functions as overrides. See docs/DECOMP.md.
//
// Each native function is marked with BF3_NATIVE(<guest address>, <stage>) right before its override; the stage
// records how far it is from the recompiled code (scripts/decomp/progress.py counts them against the v0.1 hit set):
//   lifted     the recompiled body copied by scripts/decomp/lift.py (behaviour identical by construction)
//   typed      guest registers/memory replaced by typed parameters and structs at the ABI boundary
//   idiomatic  refactored into readable C++ (classes, loops, no goto/ctx juggling inside)
//   verified   idiomatic + passing the differential tests against __imp__sub_X
// The override keeps REX_COVERAGE_HIT so coverage still counts the function; __imp__sub_X stays the original.
#pragma once

namespace bf3::native {
enum class Stage { lifted, typed, idiomatic, verified };
}  // namespace bf3::native

#define BF3_NATIVE(guest_addr, stage)                                                        \
  static_assert((guest_addr) >= 0x82000000u && (guest_addr) < 0x8A000000u, "guest address"); \
  static_assert(::bf3::native::Stage::stage == ::bf3::native::Stage::stage)
