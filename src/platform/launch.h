#pragma once
// How the program starts and switches between the title screen and worlds. The engine is built for one world per
// run, so each world (and the title screen) is its own process, and switching starts the next one:
//  - The running instance starts the next with --handoff and keeps its window up (a loading screen fed with the new
//    instance's log lines) until the new one has drawn its first frames. The new one opens cloaked at the same window
//    position and size, then reveals itself, and the old one exits: the window never disappears.
//  - Without a console (minecraftCplusplusW.exe, a shortcut, a handoff from such a run) the log goes to
//    %LOCALAPPDATA%\minecraftCplusplus\logs\latest.log (the previous one is kept, dated); an error before the first
//    frame is shown in a message box.
#include "core/common.h"
#include "platform/window.h"

#include <mutex>
#include <thread>

namespace launch {

// First thing in wmain: log file, handoff pipe, --window.
void initProcess(int argc, wchar_t** argv);
bool handoffChild();                 // started by another instance (open the window cloaked, call ready())
const Window::Placement* placement();  // --window from the previous instance, or null
// After the first frames: reveals the window and tells the previous instance to go.
void ready(Window& window);
// The arguments this instance was started with (without the program, --handoff and --window).
std::vector<std::wstring> arguments();
// Program arguments that only concern one run (the world / server, test scripts...): not passed on when switching.
bool perRunArgument(std::wstring_view arg, int& values);
// This instance's arguments that every instance it starts keeps (overrides like --saves-dir, --mods-dir, --jar).
std::vector<std::wstring> stickyArguments();

// The next instance, from the side of the one that starts it.
class Child {
public:
    enum class State { None, Running, Ready, Failed };
    ~Child();
    // args: after the program name. window: placement and focus are handed to the child.
    bool start(const std::vector<std::wstring>& args, const Window& window);
    State state();
    std::vector<std::string> takeLines();  // its log lines since the last call
    std::string error() const;             // the last "error:" line when it failed
    void cancel();                         // ends it (a connection the player gave up on)

private:
    void read();
    HANDLE process_ = nullptr, pipe_ = nullptr;
    std::thread reader_;
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
    std::string error_;
    bool ready_ = false, eof_ = false, cancelled_ = false;
};

}  // namespace launch
