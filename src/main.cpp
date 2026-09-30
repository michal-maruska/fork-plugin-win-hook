#include <windows.h>
#include "machine.h"
#include "fork_base.h"
#include "windows_environment.h"
#include "circular_archive.h"
using Env       = WindowsEnvironment;
using Archive   = CircularArchive<ForkInfo, Env::PlatformArchive>;
using Processor = forkNS::forkingMachine<Env, Archive>;

namespace forkNS { extern template class forkingMachine<Env, Archive>; }

Env       g_env;
Processor g_processor(g_env);
HHOOK     g_hook = nullptr;


LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        // our own output, pass through untouched
        if (kb->dwExtraInfo == INJECTED_MARKER) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }


        bool key_down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        WindowsEvent ev{*kb, key_down};

        // fixme:
        if (g_processor.handle_incoming(ev)) {
            return 1; // suppressed: g_env.output() already sent the replacement, if any
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    Processor processor;
    g_processor = &processor;

    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    if (!g_hook) return 1;

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_hook);
    return 0;
}
