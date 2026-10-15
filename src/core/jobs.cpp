#include "core/jobs.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace {
// Callers on several threads run their batches at once (a terrain region's coarse rebuild no longer holds up the
// full-detail meshing); the workers take items from the newest batch first, so a short batch overtakes a long one.
struct Batch {
    const std::function<void(size_t)>* fn;
    size_t count;
    std::atomic<size_t> next{0};
    unsigned workers = 0;  // workers inside it (g_mutex)
};
std::vector<std::thread> g_threads;
std::mutex g_mutex;
std::condition_variable g_wake, g_done;
std::vector<Batch*> g_batches;           // submission order (g_mutex)
std::atomic<uint64_t> g_submitted{0};    // batches ever submitted: a worker looks for a newer one after each item
bool g_quit = false;

Batch* newestWithWork() {
    for (auto it = g_batches.rbegin(); it != g_batches.rend(); ++it)
        if ((*it)->next.load(std::memory_order_relaxed) < (*it)->count) return *it;
    return nullptr;
}

// Items of the batch until it runs out (or, for a worker, until a newer batch arrives).
void runItems(Batch& b, bool yieldToNewer) {
    const uint64_t submitted = g_submitted.load(std::memory_order_relaxed);
    for (;;) {
        const size_t i = b.next.fetch_add(1, std::memory_order_relaxed);
        if (i >= b.count) break;
        (*b.fn)(i);
        if (yieldToNewer && g_submitted.load(std::memory_order_relaxed) != submitted) break;
    }
}

void workerMain() {
    // Background work must never steal the render thread's core.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        Batch* b = nullptr;
        {
            std::unique_lock lock(g_mutex);
            // the batch found here (items are taken without the lock: a second look could find none left)
            g_wake.wait(lock, [&] { return g_quit || (b = newestWithWork()) != nullptr; });
            if (g_quit) return;
            ++b->workers;
        }
        runItems(*b, true);
        std::lock_guard lock(g_mutex);
        if (--b->workers == 0) g_done.notify_all();
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
    Batch b{&fn, count};
    {
        std::lock_guard lock(g_mutex);
        g_batches.push_back(&b);
        g_submitted.fetch_add(1, std::memory_order_relaxed);
    }
    g_wake.notify_all();
    runItems(b, false);
    // Every item is taken: out of the list, then wait for the workers still running one of them.
    std::unique_lock lock(g_mutex);
    g_batches.erase(std::find(g_batches.begin(), g_batches.end(), &b));
    g_done.wait(lock, [&] { return b.workers == 0; });
}
}  // namespace Jobs
