#pragma once

#include <cstdint>
#include <cstddef>

namespace stq {

struct Pool;
struct Task;

/// \brief Create a new task pool.
/// \param worker_count Number of worker threads. If -1, the number of hardware threads is used.
/// \return The new task pool.
Pool* pool_create(int worker_count = -1);

/// \brief Destroy a task pool.
/// \param pool Task pool.
void pool_destroy(Pool* pool);

/// \brief Submit a new task with dependencies.
/// \param pool Task pool to submit to.
/// \param func Function to execute.
/// \param payload Payload to pass to the function.
/// \param deps Parent tasks to wait for.
/// \param deps_count Number of parent tasks.
/// \return The new task.
Task* task_submit_deps(Pool* pool, void (*func)(void*), void* payload, Task** deps, size_t deps_count);

/// \brief Submit a new task.
/// \param pool Task pool to submit to.
/// \param func Function to execute.
/// \param payload Payload to pass to the function.
/// \return The new task.
Task* task_submit(Pool* pool, void (*func)(void*), void* payload);

void task_release(Task* task);

/// \brief Wait for a task to finish.
/// \param task Task to wait for.
void task_wait(Task* task);

} // namespace stq

#ifdef STQ_IMPLEMENTATION

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifndef STQ_ENABLE_ASSERT
#define STQ_ENABLE_ASSERT 1
#endif

#ifndef STQ_ENABLE_TRACE
#define STQ_ENABLE_TRACE 0
#endif

#if STQ_ENABLE_ASSERT
#include <cassert>
#endif

#if STQ_ENABLE_TRACE
#include <cstdio>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-pedantic"
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#elif defined(__GNUG__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat="
#endif

#if STQ_ENABLE_TRACE
#define STQ_TRACE(fmt, ...) printf(fmt, ##__VA_ARGS__)
#else
#define STQ_TRACE(fmt, ...)
#endif

#if STQ_ENABLE_ASSERT
#define STQ_ASSERT(cond) assert(cond)
#else
#define STQ_ASSERT(cond)
#endif

namespace stq {

using StopCriterion = bool (*)(void*);

struct Pool;

struct Task {
    Pool* pool;
    void (*func)(void*);
    void* payload;

    /// Reference counter. When it reaches 0, the task is deleted.
    std::atomic<uint64_t> ref_count{0};

    /// Whether the task is done.
    std::atomic<bool> done{false};

    /// Number of waits on this task, issued by task_wait().
    std::atomic<uint32_t> wait_count{0};

    /// Number of parents this task waits for.
    std::atomic<uint32_t> parent_wait_count{0};

    std::vector<Task*> children;
};

struct Queue {
    std::deque<Task*> tasks;
    std::mutex tasks_mutex;
    std::condition_variable tasks_cv;

    std::mutex task_children_mutex;

    ~Queue()
    {
        for (Task* task : tasks) {
            release(task);
        }
    }

    Task* alloc()
    {
        Task* task = new Task();
        retain(task);
        return task;
    }

    void retain(Task* task) { task->ref_count++; }

    void release(Task* task)
    {
        if (task->ref_count-- == 1) {
            for (Task* child : task->children) {
                if (child->parent_wait_count-- == 1) {
                    push(child);
                }
            }
            STQ_TRACE("destroy task %p\n", this);
            delete task;
        }
    }

    void add_dependency(Task* task, Task* parent)
    {
        {
            std::lock_guard<std::mutex> lock(task_children_mutex);
            parent->children.push_back(task);
        }
        task->parent_wait_count++;
    }

    void push(Task* task)
    {
        retain(task);
        {
            std::unique_lock<std::mutex> lock(tasks_mutex);
            tasks.push_back(task);
        }
        tasks_cv.notify_one();
    }

    Task* pop(StopCriterion stop_criterion, void* stop_payload)
    {
        std::unique_lock<std::mutex> lock(tasks_mutex);
        tasks_cv.wait(lock, [&] { return !tasks.empty() || stop_criterion(stop_payload); });
        if (stop_criterion(stop_payload))
            return nullptr;
        Task* task = tasks.front();
        tasks.pop_front();
        return task;
    }

    void notify_all() { tasks_cv.notify_all(); }
};

struct Pool {
    Queue queue;
    std::vector<std::thread> workers;
    std::atomic<bool> terminate{false};

    ~Pool()
    {
        terminate.store(true);
        queue.notify_all();
        for (std::thread& worker : workers)
            if (worker.joinable())
                worker.join();
    }
};

inline void do_work(Pool* pool, StopCriterion stop_criterion, void* stop_payload)
{
    Task* task = pool->queue.pop(stop_criterion, stop_payload);
    if (task) {
        STQ_TRACE("run task %p\n", task);
        if (task->func)
            task->func(task->payload);
        task->done.store(true);
        if (task->wait_count.load() > 0) {
            task->pool->queue.notify_all();
        }
        pool->queue.release(task);
    }
}

Pool* pool_create(int worker_count)
{
    STQ_TRACE("pool_create(worker_count=%d)\n", worker_count);
    STQ_ASSERT(worker_count >= -1);

    Pool* pool = new Pool();

    if (worker_count == -1) {
        worker_count = std::thread::hardware_concurrency();
        if (worker_count < 1)
            worker_count = 1;
    }

    for (int i = 0; i < worker_count; i++) {
        pool->workers.push_back(std::thread(
            [pool]
            {
                auto stop_criterion = [](void* payload) { return ((std::atomic<bool>*)payload)->load(); };
                void* stop_payload = &pool->terminate;
                while (!pool->terminate) {
                    do_work(pool, stop_criterion, stop_payload);
                }
            }
        ));
    }

    return pool;
}

void pool_destroy(Pool* pool)
{
    STQ_TRACE("pool_destroy(pool=%p)\n", pool);
    STQ_ASSERT(pool != nullptr);

    delete pool;
}

Task* task_submit_deps(Pool* pool, void (*func)(void*), void* payload, Task** deps, size_t deps_count)
{
    STQ_TRACE(
        "task_submit_deps(pool=%p, func=%p, payload=%p, deps=%p, deps_count=%zu)\n",
        pool,
        func,
        payload,
        deps,
        deps_count
    );
    STQ_ASSERT(pool != nullptr);
    STQ_ASSERT(func != nullptr);

    Task* task = pool->queue.alloc();
    task->pool = pool;
    task->func = func;
    task->payload = payload;

    if (deps && deps_count > 0) {
        for (size_t i = 0; i < deps_count; i++) {
            Task* dep = deps[i];
            if (dep) {
                STQ_ASSERT(dep->pool == pool);
                pool->queue.add_dependency(task, dep);
            }
        }
        if (task->parent_wait_count.load() == 0) {
            pool->queue.push(task);
        }
    } else {
        pool->queue.push(task);
    }

    return task;
}

Task* task_submit(Pool* pool, void (*func)(void*), void* payload)
{
    return task_submit_deps(pool, func, payload, nullptr, 0);
}

void task_release(Task* task)
{
    STQ_TRACE("task_release(task=%p)\n", task);
    STQ_ASSERT(task != nullptr);

    task->pool->queue.release(task);
}

void task_wait(Task* task)
{
    STQ_TRACE("wait(task=%p)\n", task);
    STQ_ASSERT(task != nullptr);

    if (task->done.load())
        return;

    task->wait_count++;

    auto stop_criterion = [](void* payload) { return ((std::atomic<bool>*)payload)->load(); };
    void* stop_payload = &task->done;
    while (!stop_criterion(stop_payload)) {
        do_work(task->pool, stop_criterion, stop_payload);
    }

    task->wait_count--;
}

} // namespace stq

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUG__)
#pragma GCC diagnostic pop
#endif

#endif // STQ_IMPLEMENTATION
