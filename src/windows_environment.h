#pragma once
#include <windows.h>

#include "platform.h" // wherever your core template lives
#include "circular.h"

struct WindowsEvent {
    KBDLLHOOKSTRUCT raw;
};

struct WindowsEnvironment {
    static DWORD keycode_of(const WindowsEvent& e) { return e.raw.vkCode; }
    static DWORD time_of(const WindowsEvent& e)     { return e.raw.time; } // see earlier wraparound note

    struct archived_event_t { DWORD kc; DWORD t; };
    using last_events_t = std::deque<archived_event_t>; // or your fixed-size ring, size 10

    // called by KeyProcessor when it decides to emit a (possibly rewritten) key
    static void output(DWORD vk, bool key_down) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = static_cast<WORD>(vk);
        input.ki.dwFlags = key_down ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &input, sizeof(INPUT));
    }
};
