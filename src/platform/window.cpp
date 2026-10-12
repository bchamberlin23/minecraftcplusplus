#include "platform/window.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

bool Window::create(const wchar_t* title, int width, int height, const Placement* placement, bool cloaked) {
    if (placement && placement->width > 0 && placement->height > 0) width = placement->width, height = placement->height;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"minecraftCplusplus";
    RegisterClassExW(&wc);

    RECT r{0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    const bool placed = placement && placement->width > 0;
    int x = placed ? placement->x : CW_USEDEFAULT, y = placed ? placement->y : CW_USEDEFAULT;
    if (headless_)  // left of every monitor, never activated, no taskbar button
        x = GetSystemMetrics(SM_XVIRTUALSCREEN) - (r.right - r.left) - 64, y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    hwnd_ = CreateWindowExW(headless_ ? WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : 0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                            x, y, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;
    if (cloaked) {  // rendered to while invisible (the instance showing its loading screen stays in front)
        const BOOL on = TRUE;
        DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &on, sizeof(on));
        cloaked_ = true;
    }
    if (placed && placement->maximized && !headless_) ShowWindow(hwnd_, SW_SHOWMAXIMIZED);
    RECT client;
    GetClientRect(hwnd_, &client);
    width_ = u32(client.right), height_ = u32(client.bottom);

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage = 0x02;      // mouse
    rid.hwndTarget = hwnd_;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    if (!(placed && placement->maximized) || headless_) ShowWindow(hwnd_, cloaked || headless_ ? SW_SHOWNOACTIVATE : SW_SHOW);
    return true;
}

void Window::reveal() {
    if (!cloaked_) return;
    const BOOL off = FALSE;
    DwmSetWindowAttribute(hwnd_, DWMWA_CLOAK, &off, sizeof(off));
    cloaked_ = false;
    if (headless_) return;
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(hwnd_);
}

Window::Placement Window::placement() const {
    Placement p;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (fullscreen_) wp = windowed_;  // the next instance opens windowed there (and goes fullscreen from options.txt)
    else if (!GetWindowPlacement(hwnd_, &wp)) return p;
    // The restored rectangle (workspace coordinates match screen coordinates for windows without a toolbar
    // docked on top); the client size is what create() takes.
    RECT outer = wp.rcNormalPosition, frame{0, 0, 0, 0};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    p.x = outer.left, p.y = outer.top;
    p.width = (outer.right - outer.left) - (frame.right - frame.left);
    p.height = (outer.bottom - outer.top) - (frame.bottom - frame.top);
    p.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (!fullscreen_ && IsZoomed(hwnd_));
    return p;
}

bool Window::pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

bool Window::keyDown(int vk) const {
    if (vk >= 0 && vk < 256 && injectedDown_[vk]) return true;
    // Scripts inject the generic modifier keys; bindings name the left / right ones.
    if ((vk == VK_LSHIFT || vk == VK_RSHIFT) && injectedDown_[VK_SHIFT]) return true;
    if ((vk == VK_LCONTROL || vk == VK_RCONTROL) && injectedDown_[VK_CONTROL]) return true;
    if ((vk == VK_LMENU || vk == VK_RMENU) && injectedDown_[VK_MENU]) return true;
    if (vk == VK_SHIFT && (injectedDown_[VK_LSHIFT] || injectedDown_[VK_RSHIFT])) return true;
    if (vk == VK_CONTROL && (injectedDown_[VK_LCONTROL] || injectedDown_[VK_RCONTROL])) return true;
    return GetForegroundWindow() == hwnd_ && (GetAsyncKeyState(vk) & 0x8000);
}

void Window::setFullscreen(bool on) {
    if (on == fullscreen_ || !hwnd_) return;
    if (on) {
        windowed_.length = sizeof(windowed_);
        GetWindowPlacement(hwnd_, &windowed_);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        fullscreen_ = true;
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        fullscreen_ = false;
        SetWindowPlacement(hwnd_, &windowed_);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    if (captured_ && !headless_) {  // the cursor stays confined to the new client area
        RECT r;
        GetClientRect(hwnd_, &r);
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);
    }
}

void Window::cursorPos(int& x, int& y) const {
    if (scriptCursor_) {
        x = scriptX_, y = scriptY_;
        return;
    }
    POINT p{};
    GetCursorPos(&p);
    ScreenToClient(hwnd_, &p);
    x = p.x, y = p.y;
}

void Window::centerCursor() {
    if (headless_) return;
    POINT p{LONG(width_ / 2), LONG(height_ / 2)};
    ClientToScreen(hwnd_, &p);
    SetCursorPos(p.x, p.y);
}

void Window::push(InputEvent::Type type, int code, bool down, bool repeat) {
    InputEvent e;
    e.type = type, e.code = code, e.down = down, e.repeat = repeat;
    cursorPos(e.x, e.y);
    input_.push_back(e);
}

void Window::inject(const InputEvent& e) {
    if (e.type == InputEvent::Key && e.code >= 0 && e.code < 256) injectedDown_[e.code] = e.down;
    if (e.type == InputEvent::Key && e.down && !e.repeat) press(e.code);
    if (e.type == InputEvent::Button && e.code >= 0 && e.code < 3)  // scripted buttons can be held (keyDown)
        injectedDown_[e.code == 0 ? VK_LBUTTON : e.code == 1 ? VK_RBUTTON : VK_MBUTTON] = e.down;
    InputEvent q = e;
    if (q.type == InputEvent::Button || q.type == InputEvent::Wheel) cursorPos(q.x, q.y);
    if (q.type == InputEvent::Button && q.down && captured_) {
        press(q.code == 0 ? VK_LBUTTON : q.code == 1 ? VK_RBUTTON : VK_MBUTTON);
        if (q.code == 0) ++events_.left;
        if (q.code == 1) ++events_.right;
        if (q.code == 2) ++events_.middle;
    }
    if (q.type == InputEvent::Wheel) events_.wheel += q.code;
    input_.push_back(q);
}

std::string Window::clipboard() const {
    std::string out;
    if (!OpenClipboard(hwnd_)) return out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
        if (const wchar_t* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                out.resize(size_t(n - 1));
                WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
            }
            GlobalUnlock(h);
        }
    CloseClipboard();
    return out;
}

void Window::setClipboard(const std::string& utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0 || !OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size_t(n) * sizeof(wchar_t))) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, static_cast<wchar_t*>(GlobalLock(g)), n);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

void Window::setMouseCaptured(bool captured) {
    if (captured == captured_) return;
    captured_ = captured;
    if (headless_) {
    } else if (captured) {
        RECT r;
        GetClientRect(hwnd_, &r);
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);
        while (ShowCursor(FALSE) >= 0) {
        }
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {
        }
    }
    dx_ = dy_ = 0;
}

void Window::setTitle(const std::string& title) { SetWindowTextA(hwnd_, title.c_str()); }

LRESULT CALLBACK Window::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        static_cast<Window*>(cs->lpCreateParams)->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

namespace {
// The left / right key of a generic modifier (Shift, Ctrl, Alt) from the scan code and the extended-key flag.
int sidedKey(WPARAM wp, LPARAM lp) {
    const bool extended = (lp >> 24) & 1;
    if (wp == VK_SHIFT) return int(MapVirtualKeyW(UINT((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX));
    if (wp == VK_CONTROL) return extended ? VK_RCONTROL : VK_LCONTROL;
    if (wp == VK_MENU) return extended ? VK_RMENU : VK_LMENU;
    return int(wp);
}
}  // namespace

LRESULT Window::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE: closed_ = true; return 0;
        case WM_SIZE:
            if (LOWORD(lp) && HIWORD(lp)) {
                width_ = LOWORD(lp), height_ = HIWORD(lp);
                resized_ = true;
            }
            return 0;
        case WM_LBUTTONDOWN:
            push(InputEvent::Button, 0, true);
            press(VK_LBUTTON);
            if (captured_)
                ++events_.left;
            else if (clickCaptures_)
                setMouseCaptured(true);
            return 0;
        case WM_RBUTTONDOWN:
            push(InputEvent::Button, 1, true);
            press(VK_RBUTTON);
            if (captured_) ++events_.right;
            return 0;
        case WM_MBUTTONDOWN:
            push(InputEvent::Button, 2, true);
            press(VK_MBUTTON);
            if (captured_) ++events_.middle;
            return 0;
        case WM_LBUTTONUP: push(InputEvent::Button, 0, false); return 0;
        case WM_RBUTTONUP: push(InputEvent::Button, 1, false); return 0;
        case WM_MBUTTONUP: push(InputEvent::Button, 2, false); return 0;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            const bool second = GET_XBUTTON_WPARAM(wp) == XBUTTON2;
            push(InputEvent::Button, second ? 4 : 3, msg == WM_XBUTTONDOWN);
            if (msg == WM_XBUTTONDOWN) press(second ? VK_XBUTTON2 : VK_XBUTTON1);
            return TRUE;
        }
        case WM_MOUSEWHEEL:
            events_.wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            push(InputEvent::Wheel, GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, false);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const int vk = sidedKey(wp, lp);
            const bool repeat = (lp >> 30) & 1;
            push(InputEvent::Key, vk, true, repeat);
            if (!repeat) press(vk);
            if (msg == WM_SYSKEYDOWN && wp != VK_F4) return 0;  // F10 / Alt would open the window menu
            break;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: push(InputEvent::Key, sidedKey(wp, lp), false); break;
        case WM_CHAR: {
            const wchar_t c = wchar_t(wp);
            if (c >= 0xD800 && c < 0xDC00) {
                highSurrogate_ = c;
                return 0;
            }
            int cp = c;
            if (c >= 0xDC00 && c < 0xE000) cp = highSurrogate_ ? 0x10000 + ((highSurrogate_ - 0xD800) << 10) + (c - 0xDC00) : 0xFFFD;
            highSurrogate_ = 0;
            if (cp >= 32 && cp != 127) push(InputEvent::Char, cp, true);
            return 0;
        }
        case WM_KILLFOCUS: setMouseCaptured(false); return 0;
        case WM_INPUT: {
            RAWINPUT raw;
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) !=
                    UINT(-1) &&
                raw.header.dwType == RIM_TYPEMOUSE && captured_ && !(raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                dx_ += float(raw.data.mouse.lLastX);
                dy_ += float(raw.data.mouse.lLastY);
            }
            break;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
