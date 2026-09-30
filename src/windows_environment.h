#pragma once
#include <windows.h>

#include "platform.h" // wherever your core template lives
#include "circular.h"

struct WindowsEvent {
    KBDLLHOOKSTRUCT raw;
    bool key_down;
};

struct WindowsArchiveEvent {
    DWORD vk;
    DWORD time; };

class WindowsEnvironment {
public:
    using PlatformEvent   = WindowsEvent;
    using PlatformArchive = WindowsArchiveEvent;
    using Keycode         = DWORD;
    using Time             = DWORD;

    struct archived_event_t { DWORD kc; DWORD t; };

    static Keycode detail_of(const WindowsEvent& e) { return e.raw.vkCode; }
    static DWORD time_of(const WindowsEvent& e)     { return e.raw.time; } // see earlier wraparound note
    // Time    time_of(const WindowsEvent& e)   const { return e.raw.time; }
    // wraps ~49.7 days, unsigned subtraction handles it

    void archive_event(PlatformArchive& ae, const WindowsEvent& e) const {
        ae.vk = e.raw.vkCode;
        ae.time = e.raw.time;
    }

    // called by KeyProcessor when it decides to emit a (possibly rewritten) key
    constexpr ULONG_PTR INJECTED_MARKER = 0x464F524B; // 'FORK', arbitrary but recognizable

    void relay_event(Keycode vk, bool key_down) const {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(vk);
        input.ki.dwFlags = key_down ? 0 : KEYEVENTF_KEYUP;
        input.ki.dwExtraInfo = INJECTED_MARKER;
        SendInput(1, &input, sizeof(INPUT));
    }
};
