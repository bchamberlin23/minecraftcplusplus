#include "platform/crash.h"

#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>

#pragma comment(lib, "dbghelp.lib")

namespace {

std::atomic<bool> g_crashing{false};

// Logs one frame per line: module!symbol+offset (file:line) — the PDB sits next to the exe.
void logStack(HANDLE process, HANDLE thread, CONTEXT context) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip, frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp, frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp, frame.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            !frame.AddrPC.Offset)
            break;
        const DWORD64 pc = frame.AddrPC.Offset;
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 symOffset = 0;
        const bool haveSym = SymFromAddr(process, pc, &symOffset, sym);
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineOffset = 0;
        const bool haveLine = SymGetLineFromAddr64(process, pc, &lineOffset, &line);
        char module[MAX_PATH] = "?";
        if (const DWORD64 base = SymGetModuleBase64(process, pc)) {
            GetModuleFileNameA(HMODULE(base), module, MAX_PATH);
        }
        const char* moduleName = std::strrchr(module, '\\') ? std::strrchr(module, '\\') + 1 : module;
        if (haveLine)
            LOG("  #%02d %s!%s+0x%llx (%s:%lu)", i, moduleName, haveSym ? sym->Name : "?", (unsigned long long)symOffset,
                line.FileName, line.LineNumber);
        else
            LOG("  #%02d %s!%s+0x%llx [0x%llx]", i, moduleName, haveSym ? sym->Name : "?", (unsigned long long)symOffset,
                (unsigned long long)pc);
    }
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* info) {
    if (g_crashing.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;  // a second thread crashing meanwhile
    const EXCEPTION_RECORD& rec = *info->ExceptionRecord;
    LOG("crash: exception 0x%08lx at 0x%p on thread %lu%s", rec.ExceptionCode, rec.ExceptionAddress,
        GetCurrentThreadId(),
        rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2
            ? (rec.ExceptionInformation[0] == 1 ? " (writing)" : rec.ExceptionInformation[0] == 8 ? " (executing)" : " (reading)")
            : "");
    if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2)
        LOG("crash: address 0x%llx", (unsigned long long)rec.ExceptionInformation[1]);
    logStack(GetCurrentProcess(), GetCurrentThread(), *info->ContextRecord);

    // Minidump for a debugger: %LOCALAPPDATA%\minecraftCplusplus\crashes\crash-<date>.dmp
    wchar_t local[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::path(local) / L"minecraftCplusplus" / L"crashes";
        std::filesystem::create_directories(dir, ec);
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf(name, 64, L"crash-%04u%02u%02u-%02u%02u%02u.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        const std::filesystem::path file = dir / name;
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), info, FALSE};
            const auto type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);
            if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, type, &mei, nullptr, nullptr))
                LOG("crash: minidump written to %s", file.string().c_str());
            CloseHandle(h);
        }
    }
    std::fflush(stdout);
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;  // end the process
}

}  // namespace

void installCrashHandler() { SetUnhandledExceptionFilter(onCrash); }
