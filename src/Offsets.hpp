#pragma once
#include <cstring>
#include <mach-o/dyld.h>

inline uintptr_t get_cached_base()
{
    static uintptr_t cached_base = []() -> uintptr_t
    {
        const uint32_t count = _dyld_image_count();
        for (uint32_t i = 0; i < count; ++i)
        {
            const char* name = _dyld_get_image_name(i);
            if (name && strstr(name, "RobloxPlayer"))
                return static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(i));
        }
        return 0UL;
    }();
    return cached_base;
}

#define ASLR(x) ((x) + get_cached_base())

namespace Offsets
{
constexpr uintptr_t PrintOffset = 0x1001AC52C;
constexpr uintptr_t GetStateOffset = 0x100C7F61C;
constexpr uintptr_t ScriptContextOffset = 0x528;

inline uintptr_t Print()
{
    return ASLR(PrintOffset);
}

inline uintptr_t GetState()
{
    return ASLR(GetStateOffset);
}

inline uintptr_t ScriptContext()
{
    return ASLR(ScriptContextOffset);
}
} // namespace Offsets

namespace Roblox
{
using printdef = int (*)(int, const char*, ...);
using getstatedef = uintptr_t (*)(uintptr_t, uint64_t*, uint64_t*);

inline auto Print = reinterpret_cast<printdef>(Offsets::Print());
inline auto GetState = reinterpret_cast<getstatedef>(Offsets::GetState());
} // namespace Roblox
