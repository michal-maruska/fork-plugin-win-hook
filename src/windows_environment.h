#pragma once
#include <windows.h>

#include "platform.h" // wherever your core template lives
#include "circular.h"
#include <cstdarg>
#include <cstdio>

struct WindowsEvent {
    KBDLLHOOKSTRUCT raw;
    bool key_down;
};

struct WindowsArchiveEvent {
    DWORD vk;
    DWORD time; };

class WindowsEnvironment : public forkNS::platformEnvironment<DWORD, DWORD, WindowsArchiveEvent, WindowsEvent> {
public:
    using PlatformEvent   = WindowsEvent;
    using PlatformArchive = WindowsArchiveEvent;
    using Keycode         = DWORD;
    using Time            = DWORD;

    struct archived_event_t { DWORD kc; DWORD t; };

    // 'FORK', arbitrary but recognizable — must be static for MSVC constexpr
    static constexpr ULONG_PTR INJECTED_MARKER = 0x464F524B;

    // platformEnvironment interface
    Keycode detail_of(const WindowsEvent& e) const override { return e.raw.vkCode; }
    Time time_of(const WindowsEvent& e) const override { return e.raw.time; }
    bool press_p(const WindowsEvent& e) const override { return e.key_down; }
    bool release_p(const WindowsEvent& e) const override { return !e.key_down; }
    bool ignore_event(const WindowsEvent& e) override { (void)e; return false; }
    bool output_frozen() override { return false; }
    void push_time(Time now) override { (void)now; }

    void archive_event(PlatformArchive& ae, const WindowsEvent& e) override {
        ae.vk = e.raw.vkCode;
        ae.time = e.raw.time;
    }

    void relay_event(const WindowsEvent& ev) const override {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(ev.raw.vkCode);
        input.ki.wScan = static_cast<WORD>(ev.raw.scanCode);
        input.ki.dwFlags = ev.key_down ? 0 : KEYEVENTF_KEYUP;
        input.ki.dwExtraInfo = INJECTED_MARKER;
        SendInput(1, &input, sizeof(INPUT));
    }

    void log(const char* fmt ...) const override {
        va_list ap;
        va_start(ap, fmt);
        vlog(fmt, ap);
        va_end(ap);
    }
    void vlog(const char* fmt, va_list ap) const override {
        vprintf(fmt, ap);
    }
    void fmt_event(const char* msg, const WindowsEvent& e) const override {
        printf("%s: vk=%lu %s time=%lu\n", msg,
               (unsigned long)detail_of(e),
               press_p(e) ? "down" : "up",
               (unsigned long)time_of(e));
    }
    void free_event(WindowsEvent* pe) const override { (void)pe; }
    void rewrite_event(WindowsEvent& pe, Keycode code) override {
        pe.raw.vkCode = code;
    }
};
