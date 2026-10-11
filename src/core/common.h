#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;

double nowSeconds();

struct Timer {
    double t0 = nowSeconds();
    double ms() const { return (nowSeconds() - t0) * 1000.0; }
};

// One printf-formatted line to stdout (flushed), also handed to the log sink when one is set
// (platform/launch.h: a parent instance's loading screen follows its child's startup this way).
void logLine(const char* fmt, ...);
using LogSink = void (*)(const char* line, size_t size);
void setLogSink(LogSink sink);
#define LOG(...) logLine(__VA_ARGS__)

bool readFile(const fs::path& path, std::vector<u8>& out);
