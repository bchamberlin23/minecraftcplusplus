#include "platform/launch.h"

#include <fcntl.h>
#include <io.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <ctime>

namespace launch {

namespace {

std::vector<std::wstring> g_args;  // without the program
bool g_child = false, g_logFile = false, g_started = false;
Window::Placement g_placement;
bool g_havePlacement = false;
std::mutex g_pipeMutex;
HANDLE g_pipe = nullptr;  // handoff: log lines to the previous instance
std::mutex g_errorMutex;
std::string g_lastError;

// Every log line: the last error (for the message box) and, in a handoff child, the previous instance's screen.
void sink(const char* line, size_t size) {
    const std::string_view s(line, size);
    if (s.starts_with("error:")) {
        std::lock_guard lock(g_errorMutex);
        g_lastError = std::string(s.substr(6));
        while (!g_lastError.empty() && g_lastError.front() == ' ') g_lastError.erase(g_lastError.begin());
    }
    std::lock_guard lock(g_pipeMutex);
    if (!g_pipe) return;
    std::string out(s);
    for (char& c : out)
        if (c == '\n' || c == '\r') c = ' ';
    out += '\n';
    DWORD written = 0;
    if (!WriteFile(g_pipe, out.data(), DWORD(out.size()), &written, nullptr)) {
        CloseHandle(g_pipe);  // the previous instance is gone
        g_pipe = nullptr;
    }
}

fs::path logDir() {
    const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
    return fs::path(local ? local : L".") / "minecraftCplusplus" / "logs";
}

// latest.log -> <its date>.log; keeps the newest 10 old logs.
void openLogFile() {
    const fs::path dir = logDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path latest = dir / "latest.log";
    if (fs::exists(latest, ec)) {
        const auto t = fs::last_write_time(latest, ec);
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
        const std::time_t tt = std::chrono::system_clock::to_time_t(sys);
        std::tm tm{};
        localtime_s(&tm, &tt);
        char name[64];
        std::strftime(name, sizeof(name), "%Y-%m-%d_%H-%M-%S", &tm);
        fs::path dst = dir / (std::string(name) + ".log");
        for (int i = 2; fs::exists(dst, ec); ++i) dst = dir / (std::string(name) + "-" + std::to_string(i) + ".log");
        fs::rename(latest, dst, ec);  // fails while another instance writes it: then both append to it
        std::vector<fs::path> old;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".log" && e.path().filename() != "latest.log") old.push_back(e.path());
        std::sort(old.begin(), old.end());
        for (size_t i = 0; i + 10 < old.size(); ++i) fs::remove(old[i], ec);
    }
    FILE* f = nullptr;
    if (_wfreopen_s(&f, latest.c_str(), L"a", stdout) != 0 || !f) return;
    _dup2(_fileno(stdout), _fileno(stderr));
    SetStdHandle(STD_OUTPUT_HANDLE, HANDLE(_get_osfhandle(_fileno(stdout))));
    SetStdHandle(STD_ERROR_HANDLE, HANDLE(_get_osfhandle(_fileno(stdout))));
    g_logFile = true;
}

void showStartupError() {
    if (g_started || g_child || !g_logFile) return;
    std::string msg;
    {
        std::lock_guard lock(g_errorMutex);
        msg = g_lastError;
    }
    if (msg.empty()) return;
    msg = "minecraftCplusplus could not start:\n\n" + msg + "\n\nThe log is in " + (logDir() / "latest.log").string();
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, w.data(), int(w.size()));
    MessageBoxW(nullptr, w.c_str(), L"minecraftCplusplus", MB_OK | MB_ICONERROR);
}

bool isFileOrPipe(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    const DWORD t = GetFileType(h);
    return t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE;
}

std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') q.append(slashes * 2 + 1, L'\\');
        else q.append(slashes, L'\\');
        slashes = 0;
        q += c;
    }
    q.append(slashes * 2, L'\\');
    return q + L"\"";
}

}  // namespace

void initProcess(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view a = argv[i];
        if (a == L"--handoff" && i + 1 < argc) {
            g_pipe = HANDLE(uintptr_t(_wcstoui64(argv[++i], nullptr, 10)));
            SetHandleInformation(g_pipe, HANDLE_FLAG_INHERIT, 0);  // not into the local server's Java
            g_child = true;
        } else if (a == L"--window" && i + 1 < argc) {
            int max = 0;
            Window::Placement& p = g_placement;
            g_havePlacement = swscanf(argv[++i], L"%d,%d,%d,%d,%d", &p.x, &p.y, &p.width, &p.height, &max) >= 4 &&
                              p.width > 0 && p.height > 0;
            p.maximized = max != 0;
        } else {
            g_args.emplace_back(a);
        }
    }
    // No console window and no redirected output (started from a shortcut or by minecraftCplusplusW.exe): log to a file.
    if (!GetConsoleWindow() && !isFileOrPipe(GetStdHandle(STD_OUTPUT_HANDLE))) openLogFile();
    setLogSink(sink);
    std::atexit(showStartupError);
}

bool handoffChild() { return g_child; }
const Window::Placement* placement() { return g_havePlacement ? &g_placement : nullptr; }
std::vector<std::wstring> arguments() { return g_args; }

void ready(Window& window) {
    if (g_started) return;
    g_started = true;
    window.reveal();
    std::lock_guard lock(g_pipeMutex);
    if (!g_pipe) return;
    const char msg[] = "@@READY\n";
    DWORD written = 0;
    WriteFile(g_pipe, msg, DWORD(sizeof(msg) - 1), &written, nullptr);
    CloseHandle(g_pipe);
    g_pipe = nullptr;
}

bool perRunArgument(std::wstring_view a, int& values) {
    static const std::pair<const wchar_t*, int> kPerRun[] = {
        {L"--world", 1},        {L"--play", 0},          {L"--lan", 0},          {L"--connect", 1},
        {L"--script", 1},       {L"--child-script", 1},  {L"--seconds", 1},      {L"--screenshot", 1},
        {L"--shot-frame", 1},   {L"--cam", 1},           {L"--bench", 0},        {L"--fly", 0},
        {L"--selftest", 0},     {L"--save", 0},          {L"--gentest", 2},      {L"--gentest-chunks", 1},
        {L"--audio-test", 0},   {L"--audio-capture", 1}, {L"--readonly", 0},     {L"--menu", 1},
        {L"--radius", 1},       {L"--create-name", 1},   {L"--create-seed", 1},  {L"--create-mode", 1},
        {L"--create-difficulty", 1}, {L"--create-cheats", 1}, {L"--create-structures", 1},
        {L"--friend-join", 1}};
    for (auto [name, n] : kPerRun)
        if (a == name) {
            values = n;
            return true;
        }
    return false;
}

std::vector<std::wstring> stickyArguments() {
    std::vector<std::wstring> out;
    for (size_t i = 0; i < g_args.size(); ++i) {
        int values = 0;
        if (perRunArgument(g_args[i], values)) {
            i += size_t(values);
            continue;
        }
        out.push_back(g_args[i]);
    }
    return out;
}

// --- Child ------------------------------------------------------------------------------------------------------

Child::~Child() {
    if (process_ && state() == State::Running) cancel();
    if (reader_.joinable()) reader_.join();
    if (process_) CloseHandle(process_);
}

bool Child::start(const std::vector<std::wstring>& args, const Window& window) {
    wchar_t exe[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE write = nullptr;
    if (!CreatePipe(&pipe_, &write, &sa, 1 << 16)) return false;
    SetHandleInformation(pipe_, HANDLE_FLAG_INHERIT, 0);

    const Window::Placement p = window.placement();
    std::wstring cmd = quote(exe);
    for (const std::wstring& a : args) cmd += L" " + quote(a);
    cmd += L" --handoff " + std::to_wstring(uintptr_t(write));
    if (p.width > 0)
        cmd += L" --window " + std::to_wstring(p.x) + L"," + std::to_wstring(p.y) + L"," + std::to_wstring(p.width) + L"," +
               std::to_wstring(p.height) + L"," + (p.maximized ? L"1" : L"0");

    // Only the pipe (and redirected output, so a test's capture keeps the whole chain's log) is inherited.
    std::vector<HANDLE> inherit{write};
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), outCopy = nullptr;
    if (isFileOrPipe(out) && DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &outCopy, 0, TRUE,
                                             DUPLICATE_SAME_ACCESS)) {
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = outCopy;
        inherit.push_back(outCopy);
    }
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<u8> attrs(attrSize);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                              inherit.size() * sizeof(HANDLE), nullptr, nullptr);
    // A console run keeps the console; otherwise no console window flashes up.
    const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | (GetConsoleWindow() ? 0 : CREATE_NO_WINDOW);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(write);
    if (outCopy) CloseHandle(outCopy);
    if (!ok) {
        LOG("error: could not start %s (%lu)", fs::path(exe).string().c_str(), GetLastError());
        CloseHandle(pipe_);
        pipe_ = nullptr;
        return false;
    }
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    reader_ = std::thread([this] { read(); });
    std::string line;
    for (const std::wstring& a : args) line += " " + fs::path(a).string();
    LOG("launch: started pid %lu:%s", pi.dwProcessId, line.c_str());
    return true;
}

void Child::read() {
    std::string pending;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(pipe_, buf, sizeof(buf), &got, nullptr) && got) {
        pending.append(buf, got);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            pending.erase(0, nl + 1);
            std::lock_guard lock(mutex_);
            if (line == "@@READY") {
                ready_ = true;
                continue;
            }
            for (std::string_view prefix : {std::string_view("error:"), std::string_view("net: disconnected:")})
                if (line.starts_with(prefix)) {  // why it failed (a connection's reason comes last)
                    const size_t start = line.find_first_not_of(' ', prefix.size());
                    error_ = start == std::string::npos ? std::string() : line.substr(start);
                }
            lines_.push_back(std::move(line));
        }
    }
    CloseHandle(pipe_);
    std::lock_guard lock(mutex_);
    pipe_ = nullptr;
    eof_ = true;
}

Child::State Child::state() {
    if (!process_) return State::None;
    std::lock_guard lock(mutex_);
    if (ready_) return State::Ready;
    if (eof_ || cancelled_) return State::Failed;
    return State::Running;
}

std::vector<std::string> Child::takeLines() {
    std::lock_guard lock(mutex_);
    return std::move(lines_);
}

std::string Child::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void Child::cancel() {
    if (!process_) return;
    {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
    }
    TerminateProcess(process_, 1);  // its local server (if any) dies with it: a job object
}

}  // namespace launch
