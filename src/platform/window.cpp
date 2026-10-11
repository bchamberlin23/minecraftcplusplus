#include "platform/window.h"

bool Window::create(const wchar_t* title, int width, int height) {
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
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;
    RECT client;
    GetClientRect(hwnd_, &client);
    width_ = u32(client.right), height_ = u32(client.bottom);

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage = 0x02;      // mouse
    rid.hwndTarget = hwnd_;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    ShowWindow(hwnd_, SW_SHOW);
    return true;
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
    return GetForegroundWindow() == hwnd_ && (GetAsyncKeyState(vk) & 0x8000);
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
    InputEvent q = e;
    if (q.type == InputEvent::Button || q.type == InputEvent::Wheel) cursorPos(q.x, q.y);
    if (q.type == InputEvent::Button && q.down && captured_) {
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
    if (captured) {
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
            if (captured_)
                ++events_.left;
            else if (clickCaptures_)
                setMouseCaptured(true);
            return 0;
        case WM_RBUTTONDOWN:
            push(InputEvent::Button, 1, true);
            if (captured_) ++events_.right;
            return 0;
        case WM_MBUTTONDOWN:
            push(InputEvent::Button, 2, true);
            if (captured_) ++events_.middle;
            return 0;
        case WM_LBUTTONUP: push(InputEvent::Button, 0, false); return 0;
        case WM_RBUTTONUP: push(InputEvent::Button, 1, false); return 0;
        case WM_MBUTTONUP: push(InputEvent::Button, 2, false); return 0;
        case WM_MOUSEWHEEL:
            events_.wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            push(InputEvent::Wheel, GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, false);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            push(InputEvent::Key, int(wp), true, (lp >> 30) & 1);
            if (msg == WM_SYSKEYDOWN && wp != VK_F4) return 0;  // F10 / Alt would open the window menu
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP: push(InputEvent::Key, int(wp), false); break;
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
