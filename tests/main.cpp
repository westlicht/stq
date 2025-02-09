#define STQ_ENABLE_TRACE 1
#define STQ_IMPLEMENTATION
#include "stq.h"

#include <cstdio>

#include <vector>
#include <thread>

using namespace stq;

int main()
{
    Pool* pool = pool_create();

    std::vector<Task*> tasks;

    for (int i = 0; i < 50; ++i) {
        Task* task = task_submit(
            pool,
            [](void* payload)
            {
                printf("task %p\n", payload);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            },
            (void*)uintptr_t(i)
        );
        tasks.push_back(task);
    }

    Task* wait_task = task_submit_deps(pool, [](void*) { }, nullptr, tasks.data(), tasks.size());

    for (Task* task : tasks) {
        task_release(task);
    }

    task_wait(wait_task);
    task_release(wait_task);

    pool_destroy(pool);

    return 0;
}