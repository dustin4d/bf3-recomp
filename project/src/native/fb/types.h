// Frostbite 2 basic types as they sit in BF.Main's guest memory (big-endian, 32-bit pointers), for native code that
// still runs against the recompiled game. Used by the generated reflected_types.h (scripts/decomp/gen_types.py).
#pragma once

#include <cstdint>

#include <rex/system/xtypes.h>
#include <rex/types.h>

namespace fb {

// char* into guest memory.
struct CString {
  rex::be<uint32_t> ptr;
};

// fb::Array<T>: a guest pointer to the first element; the element count is the u32 just before it (ptr - 4).
template <typename T>
struct Array {
  rex::be<uint32_t> ptr;
};

// Reflected enums are stored as 32-bit values.
template <typename E>
struct Enum {
  rex::be<uint32_t> value;
  E get() const { return E(uint32_t(value)); }
};

struct Guid {
  uint8_t bytes[16];
};

static_assert(sizeof(CString) == 4 && sizeof(Array<int>) == 4 && sizeof(Enum<int>) == 4 && sizeof(Guid) == 16);

}  // namespace fb
