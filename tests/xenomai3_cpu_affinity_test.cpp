#include <cstdio>
#include <cstdlib>

#include <rtt/os/fosi_internal_interface.hpp>

#if CONFIG_XENO_VERSION_MAJOR >= 3
namespace {
int spawn_mode = 0;
int spawn_calls = 0;
}

extern "C" int rt_task_spawn(RT_TASK*, const char*, int, int, int mode,
                             void (*)(void*), void* cookie)
{
    spawn_mode = mode;
    ++spawn_calls;
    std::free(cookie);
    return 0;
}

extern "C" int rt_task_yield()
{
    return 0;
}
#endif

int main()
{
#if CONFIG_XENO_VERSION_MAJOR < 3
    return 0;
#else
    const unsigned masks[] = {0, ~0u, 0x8, 0x9};
    const unsigned native_masks[] = {0, 0, T_CPU(3), T_CPU(0) | T_CPU(3)};
    for (unsigned index = 0; index != sizeof(masks) / sizeof(masks[0]); ++index) {
        RTOS_TASK task = {};
        spawn_calls = 0;
        const int result = RTT::os::rtos_task_create(
            &task, 0, masks[index], "affinity-test", ORO_SCHED_OTHER,
            128000, 0, 0);
        std::free(task.name);
        if (result != 0 || spawn_calls != 1) return 1;
        if ((static_cast<unsigned>(spawn_mode) & T_CPUMASK) != native_masks[index]) {
            std::fprintf(stderr, "Affinity %#x encoded native mask %#x, expected %#x\n",
                         masks[index], static_cast<unsigned>(spawn_mode) & T_CPUMASK,
                         native_masks[index]);
            return 2;
        }
        if (!(spawn_mode & T_JOINABLE)) return 3;
        if (task.cpu_affinity != (masks[index] ? masks[index] : ~0u)) return 4;
    }
    return 0;
#endif
}
