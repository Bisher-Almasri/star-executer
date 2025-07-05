#pragma once
#include <cstring>
#include <mach-o/dyld.h>

inline uintptr_t get_cached_base() {
  static uintptr_t cached_base = []() -> uintptr_t {
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; ++i) {
      const char *name = _dyld_get_image_name(i);
      if (name && strstr(name, "RobloxPlayer"))
        return static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(i));
    }
    return 0UL;
  }();
  return cached_base;
}

#define ASLR(x) ((x) + get_cached_base())

    namespace Offsets {
    inline uintptr_t Print() { return ASLR(0x1001AC52C); }
    inline uintptr_t GetState() { return ASLR(0x100C7F61C); }
    inline uintptr_t ScriptContextJob() { return ASLR(0x528); }
    inline uintptr_t GetSchedular() { return ASLR(0x1032E99EC); }
    namespace TaskSchedular
    {
        inline uintptr_t JobStart() { return ASLR(0x1F0); }
        inline uintptr_t JobEnd() { return ASLR(0x1F8); }
        inline uintptr_t JobName() { return ASLR(0x18); }
    }
} // namespace Offsets

namespace Roblox {
using printdef = int (*)(int, const char *, ...);
using getstatedef = uintptr_t (*)(uintptr_t, int64_t *, int64_t *);

inline auto Print = reinterpret_cast<printdef>(Offsets::Print());
inline auto GetState = reinterpret_cast<getstatedef>(Offsets::GetState());
} // namespace Roblox
