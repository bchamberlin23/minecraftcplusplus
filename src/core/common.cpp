#include "core/common.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>

double nowSeconds() {
    static const double inv = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * inv;
}

bool readFile(const fs::path& path, std::vector<u8>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) {
        CloseHandle(h);
        return false;
    }
    out.resize(size_t(size.QuadPart));
    size_t done = 0;
    bool ok = true;
    while (done < out.size()) {
        DWORD chunk = DWORD(std::min<size_t>(out.size() - done, size_t(1) << 30));
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, chunk, &got, nullptr) || got == 0) {
            ok = false;
            break;
        }
        done += got;
    }
    CloseHandle(h);
    return ok;
}

namespace {
std::atomic<LogSink> g_logSink{nullptr};
}  // namespace

void setLogSink(LogSink sink) { g_logSink = sink; }

void logLine(const char* fmt, ...) {
    static const double start = nowSeconds();
    static const bool stamps = std::getenv("MCPP_LOG_TIME") != nullptr;  // seconds since start before each line
    if (stamps) std::printf("[%8.3f] ", nowSeconds() - start);
    char stack[2048];
    std::vector<char> heap;
    char* buf = stack;
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(stack, sizeof(stack) - 1, fmt, args);
    va_end(args);
    if (n < 0) return;
    if (size_t(n) >= sizeof(stack) - 1) {
        heap.resize(size_t(n) + 2);
        va_start(args, fmt);
        std::vsnprintf(heap.data(), heap.size() - 1, fmt, args);
        va_end(args);
        buf = heap.data();
    }
    buf[n] = '\n';
    std::fwrite(buf, 1, size_t(n) + 1, stdout);  // one write: lines from different threads do not interleave
    std::fflush(stdout);
    if (const LogSink sink = g_logSink.load()) sink(buf, size_t(n));
}
