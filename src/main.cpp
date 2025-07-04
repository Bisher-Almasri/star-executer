#include <thread>
#include <chrono>
#include <cstdio>
#include "Offsets.hpp"

void star_main()
{
    std::this_thread::sleep_for(std::chrono::seconds(25));

    if (!get_cached_base())
    {
        printf("dfghytgrfergthngrferghnm .\n");
        return;
    }

    Roblox::Print(0, "print work");
}

__attribute__((constructor)) void on_inject()
{
    std::thread(star_main).detach();
}
