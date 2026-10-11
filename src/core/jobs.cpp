#include "core/jobs.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {
std::vector<std::thread> g_threads;
std::mutex g_mutex;
std::mutex g_callMutex;  // one batch at a time (callers on several threads take turns)
std::condition_variable g_wake, g_done;
const std::function<void(size_t)>* g_fn = nullptr;
size_t g_count = 0;
std::atomic<size_t> g_next{0};
unsigned g_pending = 0;
uint64_t g_generation = 0;
bool g_quit = false;

void runItems() {
    for (;;) {
        size_t i = g_next.fetch_add(1, std::memory_order_relaxed);
        if (i >= g_count) break;
        (*g_fn)(i);
    }
}

void workerMain() {
    // Background work must never steal the render thread's core.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    uint64_t seen = 0;
    for (;;) {
        {
            std::unique_lock lock(g_mutex);
            g_wake.wait(lock, [&] { return g_quit || g_generation != seen; });
            if (g_quit) return;
            seen = g_generation;
        }
        runItems();
        std::lock_guard lock(g_mutex);
        if (--g_pending == 0) g_done.notify_one();
    }
}
}  // namespace

namespace Jobs {
void init(unsigned threads) {
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned i = 1; i < threads; ++i) g_threads.emplace_back(workerMain);
}

void shutdown() {
    {
        std::lock_guard lock(g_mutex);
        g_quit = true;
    }
    g_wake.notify_all();
    for (auto& t : g_threads) t.join();
    g_threads.clear();
}

unsigned threadCount() { return unsigned(g_threads.size()) + 1; }

void parallelFor(size_t count, const std::function<void(size_t)>& fn) {
    if (count == 0) return;
    if (count == 1 || g_threads.empty()) {
        for (size_t i = 0; i < count; ++i) fn(i);
        return;
    }
    std::lock_guard call(g_callMutex);
    {
        std::lock_guard lock(g_mutex);
        g_fn = &fn;
        g_count = count;
        g_next.store(0, std::memory_order_relaxed);
        g_pending = unsigned(g_threads.size());
        ++g_generation;
    }
    g_wake.notify_all();
    runItems();
    std::unique_lock lock(g_mutex);
    g_done.wait(lock, [] { return g_pending == 0; });
}
}  // namespace Jobs
