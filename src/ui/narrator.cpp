#include "ui/narrator.h"

#include "audio/audio.h"
#include "ui/toasts.h"

#include <sapi.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace ui::narrator {

namespace {

// The SAPI voice lives on its own COM thread; requests are queued.
class Voice {
public:
    Voice() {
        thread_ = std::thread([this] { run(); });
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return started_; });
    }
    ~Voice() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    bool ok() const { return ok_; }
    void say(std::wstring text, bool interrupt, int volume) {
        {
            std::lock_guard lock(mutex_);
            if (interrupt) queue_.clear();
            queue_.push_back({std::move(text), interrupt, volume});
        }
        cv_.notify_all();
    }

private:
    struct Request {
        std::wstring text;
        bool interrupt;
        int volume;
    };
    void run() {
        ISpVoice* voice = nullptr;
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) &&
            SUCCEEDED(CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, __uuidof(ISpVoice), reinterpret_cast<void**>(&voice))))
            ok_ = true;
        LOG("narrator: %s", ok_ ? "SAPI voice ready" : "no SAPI voice (the narrator is not available)");
        {
            std::lock_guard lock(mutex_);
            started_ = true;
        }
        cv_.notify_all();
        while (true) {
            Request r;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
                if (quit_) break;
                r = std::move(queue_.front());
                queue_.pop_front();
            }
            if (!voice) continue;
            voice->SetVolume(USHORT(std::clamp(r.volume, 0, 100)));
            // Interrupting speech purges what is playing; otherwise it queues behind it (both asynchronous).
            voice->Speak(r.text.c_str(), SPF_ASYNC | SPF_IS_NOT_XML | (r.interrupt ? SPF_PURGEBEFORESPEAK : 0), nullptr);
        }
        if (voice) {
            voice->Speak(nullptr, SPF_PURGEBEFORESPEAK, nullptr);
            voice->Release();
        }
        CoUninitialize();
    }

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    bool started_ = false, quit_ = false, ok_ = false;
};

Voice& voice() {
    static Voice v;
    return v;
}

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(std::max(n, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

int volume() {  // Options.getFinalSoundSourceVolume(VOICE)
    audio::AudioSystem* a = audio::instance();
    return a ? int(a->volume(audio::Category::Master) * a->volume(audio::Category::Voice) * 100.0f) : 100;
}

bool chatOn(int s) { return s == 1 || s == 2; }    // NarratorStatus.shouldNarrateChat
bool systemOn(int s) { return s == 1 || s == 3; }  // shouldNarrateSystem

int lastButton = -1, lastFocus = -1;
Screen lastScreen = Screen::None;

}  // namespace

bool available() { return voice().ok(); }

void say(const std::string& text, bool interrupt) {
    if (!text.empty() && voice().ok()) {
        LOG("narrator: %s", text.c_str());
        voice().say(wide(text), interrupt, volume());
    }
}

void systemNow(UiState& s, const Text& message) {
    if (systemOn(s.settings.narrator)) say(message.plain(), true);
}

void statusChanged(UiState& s) {
    const int st = std::clamp(s.settings.narrator, 0, 3);
    static const char* const kStatus[4] = {"options.narrator.off", "options.narrator.all", "options.narrator.chat", "options.narrator.system"};
    say(s.res.translate("options.narrator") + " : " + s.res.translate(kStatus[st]), true);
    if (!available())
        showToast(Text(s.res.translate("narrator.toast.disabled")), Text(s.res.translate("options.narrator.notavailable")), "narrator");
    else if (st == 0) showToast(Text(s.res.translate("narrator.toast.disabled")), Text(), "narrator");
    else showToast(Text(s.res.translate("narrator.toast.enabled")), Text(s.res.translate(kStatus[st])), "narrator");
    lastButton = -1;
}

void chat(UiState& s, const Text& message, int source) {
    const int st = s.settings.narrator;
    if (source == 2 ? chatOn(st) : st != 0) say(message.plain(), false);  // sayChatQueued / saySystemChatQueued
}

void system(UiState& s, const Text& message) {
    if (systemOn(s.settings.narrator)) say(message.plain(), false);
}

void frame(UiState& s) {
    // ScreenNarrationCollector, simplified: a screen opening narrates its title and how to use it
    // (triggerImmediateNarration), then the focused widget (keyboard) or the one under the mouse when it changes
    // ("Button: label", its position and its usage), only with Narrates All / System.
    if (!systemOn(s.settings.narrator) || !available() || s.screen == Screen::None) {
        lastButton = lastFocus = -1, lastScreen = s.screen;
        return;
    }
    if (s.screen != lastScreen) {
        lastButton = lastFocus = -1, lastScreen = s.screen;
        std::string title = s.narrationTitle.plain();
        if (title.empty()) {  // the screens that do not set one: their Screen title
            if (s.screen == Screen::Options) title = s.res.translate("options.title");
            else if (s.screen == Screen::Death) title = s.res.translate(s.hardcore ? "deathScreen.title.hardcore" : "deathScreen.title");
            else if (s.screen == Screen::Inventory) title = s.res.translate("container.crafting");
            else if (s.screen == Screen::Container) title = s.activeMenu().title.plain();
        }
        say((title.empty() ? std::string() : title + ". ") + s.res.translate("narrator.screen.usage"), true);
        return;
    }
    auto narrate = [&](int index, bool focused) {
        const Button& b = s.buttons[size_t(index)];
        const std::string label = (b.labelFn ? b.labelFn() : b.label).plain();
        std::string text = b.slider ? label : formatJava(s.res.translate("narration.button"), {label});
        if (s.buttons.size() > 1)
            text += ". " + formatJava(s.res.translate("narrator.position.screen"), {std::to_string(index + 1), std::to_string(s.buttons.size())});
        const char* usage = b.slider ? (focused ? "narration.slider.usage.focused" : "narration.slider.usage.hovered")
                                     : (focused ? "narration.button.usage.focused" : "narration.button.usage.hovered");
        say(text + ". " + s.res.translate(usage), true);
    };
    const int focus = s.focusedButton >= 0 && size_t(s.focusedButton) < s.buttons.size() ? s.focusedButton : -1;
    if (focus != lastFocus) {
        lastFocus = focus;
        if (focus >= 0) return narrate(focus, true);
    }
    int hovered = -1;
    for (size_t i = 0; i < s.buttons.size(); ++i) {
        const Button& b = s.buttons[i];
        if (b.active && s.mouseX >= b.x && s.mouseY >= b.y && s.mouseX < b.x + b.w && s.mouseY < b.y + b.h) hovered = int(i);
    }
    if (hovered == lastButton) return;
    lastButton = hovered;
    if (hovered >= 0 && hovered != focus) narrate(hovered, false);
}

bool hotkey(UiState& s, const Window::InputEvent& e) {
    if (e.type != Window::InputEvent::Key || !e.down || e.repeat || e.code != 'B' || !s.window->keyDown(VK_CONTROL)) return false;
    if (!s.settings.narratorHotkey || !available()) return false;
    s.settings.narrator = (s.settings.narrator + 1) % 4;
    s.settings.save();
    statusChanged(s);
    if (s.screen != Screen::None) s.buildButtons();  // the Narrator button shows the new value
    return true;
}

}  // namespace ui::narrator
