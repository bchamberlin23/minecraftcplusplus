#pragma once
#include "core/common.h"

#include <windows.h>

#include <array>

class Window {
public:
    // Where a window was (platform/launch.h passes it to the next instance): outer top-left, client size.
    struct Placement {
        int x = 0, y = 0, width = 0, height = 0;
        bool maximized = false;
    };
    // placement: open there instead of the default position and size. cloaked: the window exists (and is sized,
    // so it can be rendered to) but stays invisible until reveal().
    bool create(const wchar_t* title, int width, int height, const Placement* placement = nullptr, bool cloaked = false);
    // Automated runs (before create): the window opens off-screen without ever taking focus, and capturing the mouse
    // only changes the game's state (the real cursor is never confined, hidden or moved).
    void setHeadless(bool headless) { headless_ = headless; }
    bool headless() const { return headless_; }
    void reveal();  // uncloaks, raises and focuses the window
    Placement placement() const;
    bool pump();  // processes messages; false once the window is closed
    bool keyDown(int vk) const;  // VK_SHIFT / VK_CONTROL / VK_MENU also match their left / right keys and back
    bool focused() const { return GetForegroundWindow() == hwnd_; }
    bool iconified() const { return IsIconic(hwnd_) != 0; }
    u64 lastInputMs() const { return lastInputMs_; }  // GetTickCount64 of the last key / mouse input (onInputReceived)
    // Borderless fullscreen on the monitor the window is on (Window.setFullscreen); off restores the window.
    void setFullscreen(bool on);
    bool fullscreen() const { return fullscreen_; }
    void setMouseCaptured(bool captured);
    void setQuitShortcuts(bool on) { quitShortcuts_ = on; }  // Window.setQuitShortcuts: off, Alt+F4 does not close
    bool mouseCaptured() const { return captured_; }
    void takeMouseDelta(float& dx, float& dy) {
        dx = dx_, dy = dy_;
        dx_ = dy_ = 0;
    }
    struct MouseEvents {
        int left = 0, right = 0, middle = 0, wheel = 0;  // presses / wheel notches since the last call
    };
    MouseEvents takeMouseEvents() {
        MouseEvents e = events_;
        events_ = {};
        return e;
    }
    // Presses of every virtual key (keyboard keys and mouse buttons, VK_LBUTTON...) while the mouse was captured, since
    // the last call: key bindings see quick presses between frames (KeyMapping.click).
    std::array<u16, 256> takePresses() {
        const std::array<u16, 256> p = presses_;
        presses_ = {};
        return p;
    }
    bool takeResize(u32& w, u32& h) {
        if (!resized_) return false;
        resized_ = false;
        w = width_, h = height_;
        return true;
    }
    void setTitle(const std::string& title);

    // UI input, queued in order: key presses / releases, typed characters, mouse buttons and wheel with the cursor.
    struct InputEvent {
        enum Type : u8 { Key, Char, Button, Wheel } type = Key;
        // Key: virtual key (left / right specific for Shift, Ctrl and Alt: VK_LSHIFT...); Char: code point;
        // Button: 0 left 1 right 2 middle 3 / 4 side buttons; Wheel: notches
        int code = 0;
        bool down = false;  // Key / Button
        bool repeat = false;
        int x = 0, y = 0;  // cursor in client pixels (Button / Wheel)
    };
    std::vector<InputEvent> takeInput() { return std::move(input_); }
    void cursorPos(int& x, int& y) const;
    void centerCursor();
    void setClickCaptures(bool on) { clickCaptures_ = on; }  // false while a UI screen is open
    std::string clipboard() const;
    void setClipboard(const std::string& utf8);
    // Scripted input (automated tests): queued like real input; injected keys count as held until released.
    void inject(const InputEvent& e);
    void setScriptedCursor(int x, int y) { scriptCursor_ = true, scriptX_ = x, scriptY_ = y; }
    HWND hwnd() const { return hwnd_; }
    u32 width() const { return width_; }
    u32 height() const { return height_; }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    u32 width_ = 0, height_ = 0;
    bool resized_ = false, closed_ = false, captured_ = false, headless_ = false, quitShortcuts_ = true;
    float dx_ = 0, dy_ = 0;
    u64 lastInputMs_ = GetTickCount64();
    MouseEvents events_;
    std::vector<InputEvent> input_;
    bool clickCaptures_ = true;
    bool injectedDown_[256] = {};
    bool scriptCursor_ = false;
    int scriptX_ = 0, scriptY_ = 0;
    wchar_t highSurrogate_ = 0;
    bool cloaked_ = false;
    std::array<u16, 256> presses_{};
    bool fullscreen_ = false;
    WINDOWPLACEMENT windowed_{};  // where the window was before fullscreen
    void press(int vk) {
        if (captured_ && vk > 0 && vk < 256) ++presses_[size_t(vk)];
    }
    void push(InputEvent::Type type, int code, bool down, bool repeat = false);
};
