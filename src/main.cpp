#include <windows.h>

#include "machine.h"
#include "fork_base.h"
#include "windows_environment.h"
#include "circular_archive.h"
#include <winreg.h>


using Env       = WindowsEnvironment;
using Archive   = CircularArchive<ForkInfo, Env::PlatformArchive>;
using Processor = forkNS::forkingMachine<Env, Archive>;

namespace forkNS { extern template class forkingMachine<Env, Archive>; }

Env*      g_env = new Env();
Processor g_processor(g_env);
HHOOK     g_hook = nullptr;


LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        // our own output, pass through untouched
        if (kb->dwExtraInfo == Env::INJECTED_MARKER) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        bool key_down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        WindowsEvent ev{*kb, key_down};

        // hand over to forkingMachine; it will call WindowsEnvironment::relay_event
        // which re-injects via SendInput with INJECTED_MARKER
        (void)g_processor.accept_event(ev);
        return 1; // swallow original; replacement (if any) already re-injected
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}


bool load_config(DWORD& out_value) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ForkingMachine", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    DWORD size = sizeof(DWORD);
    LONG r = RegQueryValueExW(key, L"SomeSetting", nullptr, nullptr, reinterpret_cast<BYTE*>(&out_value), &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    g_processor.create_configs();

    g_processor.set_debug(1);
    g_processor.configure_key(fork_configure_key_fork, 65, 160, 1); // 64 is A

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
