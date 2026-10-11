#pragma once
#include "core/common.h"

#include <windows.h>

class Window {
public:
    bool create(const wchar_t* title, int width, int height);
    bool pump();  // processes messages; false once the window is closed
    bool keyDown(int vk) const;
    void setMouseCaptured(bool captured);
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
        int code = 0;       // Key: virtual key; Char: code point; Button: 0 left 1 right 2 middle; Wheel: notches
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
    bool resized_ = false, closed_ = false, captured_ = false;
    float dx_ = 0, dy_ = 0;
    MouseEvents events_;
    std::vector<InputEvent> input_;
    bool clickCaptures_ = true;
    bool injectedDown_[256] = {};
    bool scriptCursor_ = false;
    int scriptX_ = 0, scriptY_ = 0;
    wchar_t highSurrogate_ = 0;
    void push(InputEvent::Type type, int code, bool down, bool repeat = false);
};
