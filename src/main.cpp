#include "Offsets.hpp"
#include "VM/include/lua.h"

#include <chrono>
#include <thread>


// skidded from macsploit
namespace scheduler {
struct job {
    void** vtable;
    char padding[0x88];
    std::string job_name;
};

std::vector<job*> scheduler_jobs;
}
int64_t thread_type = 0;

// skidded from yubx mac :D
lua_State* GetRobloxState(uintptr_t ScriptContext)
{
    uint64_t scheduler = *(uint64_t*)Offsets::GetSchedular();
    uint64_t start = *(uint64_t*)(scheduler + Offsets::TaskSchedular::JobStart());
    uint64_t end = *(uint64_t*)(scheduler + Offsets::TaskSchedular::JobEnd());

    while (start < end)
    {
        auto job = *(scheduler::job**)start;
        scheduler::scheduler_jobs.push_back(job);

        if (job->job_name == "WaitingHybridScriptsJob")
        {
            int64_t trigger = 0;
            uint64_t packed = Roblox::GetState(ScriptContext, &trigger, &thread_type);
            return reinterpret_cast<lua_State*>(static_cast<uintptr_t>(static_cast<uint32_t>(packed)));
        }
    }
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
}

__attribute__((constructor)) void on_inject()
{
    std::thread(star_main).detach();
}
