#include "Offsets.hpp"
#include "VM/include/lua.h"

#include <chrono>
#include <thread>

// skidded from yubx mac :D
lua_State* GetRobloxState(uintptr_t ScriptContext) {
    uint64_t a2 = 0;
    uint64_t a3 = 0;

    uint64_t packed = Roblox::GetState(ScriptContext, &a2, &a3);
    return reinterpret_cast<lua_State*>(static_cast<uintptr_t>(static_cast<uint32_t>(packed)));
}

void star_main()
{
    std::this_thread::sleep_for(std::chrono::seconds(25));

    if (!get_cached_base())
    {
        printf("no .\n");
        return;
    }

    Roblox::Print(0, "print work");

    // seg faults
    GetRobloxState(Offsets::ScriptContext());

}

__attribute__((constructor)) void on_inject()
{
    std::thread(star_main).detach();
}
