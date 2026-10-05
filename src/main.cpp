#include <windows.h>
#include "machine.h"
#include "fork_base.h"
#include "windows_environment.h"
#include "circular_archive.h"

#include <fstream>
#include <sstream>   // for std::wistringstream, used right below in the same function
#include <shellapi.h>

#include <shlobj.h>
#include <filesystem>

using Env       = WindowsEnvironment;
using Archive   = CircularArchive<ForkInfo, Env::PlatformArchive>;
using Processor = forkNS::forkingMachine<Env, Archive>;

namespace forkNS { extern template class forkingMachine<Env, Archive>; }

Env*      g_env = new Env();
Processor g_processor(g_env);
HHOOK     g_kbd_hook = nullptr;
HHOOK     g_mouse_hook = nullptr;
UINT_PTR  g_timer_id  = 0;
HWND      g_msg_window = nullptr;

constexpr UINT_PTR TIMER_ID = 1;


void schedule_deadline(Env::Time deadline) {
    if (g_timer_id) {
        KillTimer(g_msg_window, g_timer_id);
        g_timer_id = 0;
    }
    // Env::Time is in kb->time units (ms since boot); SetTimer wants a relative ms delay
    DWORD now = GetTickCount();
    DWORD delay_ms = (deadline > now) ? (deadline - now) : 0; // unsigned subtraction, wraparound-safe
    g_timer_id = SetTimer(g_msg_window, TIMER_ID, delay_ms, nullptr);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        g_processor.accept_confirmation();
        if (g_timer_id) { KillTimer(g_msg_window, g_timer_id); g_timer_id = 0; }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}


LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode < 0) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

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
        Env::Time deadline = g_processor.accept_event(ev);
        if (deadline != 0) {
            schedule_deadline(deadline);
        }
        return 1; // swallow original; replacement (if any) already re-injected
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

#define MAX_KEYCODE 255
const bool SET=true;

// same "keycode fork-keycode" text format as the Xorg tool reads
bool load_config_from_file(const std::wstring& path, Processor& processor) {
    std::wifstream in(path);
    if (!in) return false;

    std::wstring line;
    while (std::getline(in, line)) {
        std::wistringstream ls(line);
        unsigned key, fork;
        if (ls >> key >> fork) {
            if (key < MAX_KEYCODE) {
                processor.configure_key(fork_configure_key_fork, key, fork, SET);
            }
        }
    }
    return true;
}

std::wstring config_path() {
    PWSTR appdata = nullptr;
    SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata);
    std::filesystem::path p = std::filesystem::path(appdata) / L"ForkingMachine" / L"config.txt";
    CoTaskMemFree(appdata);
    return p.wstring();
}

LRESULT CALLBACK MsgWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == TIMER_ID) {
            Env::Time deadline = g_processor.accept_time(GetTickCount());
            schedule_deadline(deadline);
            return 0;
        }
        break;
    case WM_APP + 1:
        if (lParam == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Reload config");
            AppendMenuW(menu, MF_STRING, 2, L"Exit");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd); // required so the menu dismisses correctly
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
            if (cmd == 1) load_config_from_file(config_path(), g_processor);
            if (cmd == 2) PostQuitMessage(0);
            DestroyMenu(menu);
        }
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {

    g_processor.create_configs();

    WNDCLASS wc{}; wc.lpfnWndProc = MsgWindowProc; wc.hInstance = hInstance; wc.lpszClassName = "ForkMsgWin";
    RegisterClass(&wc);
    g_msg_window = CreateWindow("ForkMsgWin", "", 0, 0,0,0,0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    NOTIFYICONDATA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_msg_window;          // reuse the message-only window from the timer code
    nid.uID = 1;

    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 1;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION); // placeholder; real icon later
    wcscpy_s(nid.szTip, L"ForkingMachine");
    Shell_NotifyIcon(NIM_ADD, &nid);

    g_kbd_hook   = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);
    g_mouse_hook = SetWindowsHookEx(WH_MOUSE_LL,    LowLevelMouseProc,    hInstance, 0);
    if (!g_kbd_hook || !g_mouse_hook) return 1;

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(g_kbd_hook);
    UnhookWindowsHookEx(g_mouse_hook);
    return 0;
}
