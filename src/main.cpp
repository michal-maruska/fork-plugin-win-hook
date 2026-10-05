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

#include <winreg.h>

using Env       = WindowsEnvironment;
using Archive   = CircularArchive<ForkInfo, Env::PlatformArchive>;
using Processor = forkNS::forkingMachine<Env, Archive>;

FILE* g_event_log = nullptr;
uint64_t g_event_seq = 0;

namespace forkNS { extern template class forkingMachine<Env, Archive>; }

Env       g_env;
Processor g_processor(&g_env);
HHOOK     g_kbd_hook = nullptr;
HHOOK     g_mouse_hook = nullptr;
UINT_PTR  g_timer_id  = 0;
HWND      g_msg_window = nullptr;
bool      g_debug_enabled = false;

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

#if 0
struct FileDumper {
    void operator()(const Entry& e) {
        if (!g_event_log) return;
        fprintf(g_event_log, "%llu\t%u\t%u\t%d\t%s\n",
                ++g_event_seq, e.second.vk, e.second.time, e.first.forked, to_string(e.first.reason));
        fflush(g_event_log);
    }
};
FileDumper file_dumper;
#endif

constexpr auto kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr auto kRunValueName = L"ForkingMachine";

bool is_start_on_login_enabled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return false;
    DWORD type;
    bool exists = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(hKey);
    return exists;
}

void set_start_on_login(bool enable) {
    HKEY hKey;
    RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hKey);
    if (enable) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
                        reinterpret_cast<BYTE*>(exePath), (wcslen(exePath) + 1) * sizeof(wchar_t));
    } else {
        RegDeleteValueW(hKey, kRunValueName);
    }
    RegCloseKey(hKey);
}


#define MAX_KEYCODE 255
#define MAX_FORKS 16
const bool SET=true;
const bool GET=false;
const short NO_FORK = 0;
const int NO_VALUE = 0;

void save_configuration_to_registry(Processor& processor) {
    ULONG binary_values[MAX_FORKS];
    int top = 0;

    for (USHORT key = 0; key < MAX_KEYCODE; key++) {
        USHORT value = static_cast<USHORT>(processor.configure_key(fork_configure_key_fork, key, 0, GET));
        if (value != NO_FORK) {
            binary_values[top++] = (key << 16) | value;
            if (top == MAX_FORKS) break;
        }
    }

    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\ForkingMachine", 0, nullptr,
                         0, KEY_WRITE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"binary-forks", 0, REG_BINARY,
                        reinterpret_cast<const BYTE*>(binary_values), top * sizeof(ULONG));
        RegCloseKey(hKey);
    }
}

static LSTATUS
restore_global_value(IN HKEY hKey,
                     LPCWSTR valueNameW,
                     Processor& processor,
                     int attribute)
{
    // UNICODE_STRING ValueName;
    // RtlInitUnicodeString(&ValueName, ValueNameW);
    DWORD value;
    DWORD type  = 0;
    DWORD size = sizeof(DWORD);

    LSTATUS status = RegQueryValueExW(hKey,
                                      valueNameW,
                                      nullptr,
                                      &type,
                                      reinterpret_cast<LPBYTE>(&value),
                                      &size);

    if (status == ERROR_SUCCESS) {
        if (type != REG_DWORD) {
            // Unexpected type — bail out or coerce as needed.
            return ERROR_DATATYPE_MISMATCH;
        }
        processor.configure_global((enum fork_configuration_t) attribute, value, SET);
    } else {
        g_env.log("configuration value %S not found\n", valueNameW);
    }
    return status;
}


void restore_configuration_from_registry(Processor& processor) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\ForkingMachine", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return; // no saved config: fine, defaults stand

    restore_global_value(hKey,
                         L"debug",
                         processor,
                         fork_configure_debug);

    // stupid: processor should be also g_
    g_debug_enabled = processor.configure_global(fork_configure_debug, NO_VALUE, GET);

    ULONG binary_values[MAX_FORKS];
    DWORD size = sizeof(binary_values), type;
    if (RegQueryValueExW(hKey, L"binary-forks", nullptr, &type,
                          reinterpret_cast<BYTE*>(binary_values), &size) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < size / sizeof(ULONG); i++) {
            USHORT key = binary_values[i] >> 16;
            USHORT fork = binary_values[i] & 0xFFFF;
            if (key < MAX_KEYCODE) {
                processor.configure_key(fork_configure_key_fork, key, fork, SET);
            }
        }
    }
    RegCloseKey(hKey);
}


// same "keycode fork-keycode" text format as the Xorg tool reads
bool load_config_from_file(const std::wstring& path, Processor& processor) {
    if (!std::filesystem::exists(path)) {
        g_env.log("config file not found at: %ls\n", path.c_str());
        return false;
    }
    std::wifstream in(path);
    if (!in) {
        g_env.log("failed to open file %ls\n", path.c_str());
        return false;
    }

    std::wstring line;
    while (std::getline(in, line)) {
        std::wistringstream ls(line);
        unsigned key, fork;
        if (ls >> key >> fork) {
            if (key < MAX_KEYCODE) {
                g_env.log("forking %d to  %d\n", key, fork);
                processor.configure_key(fork_configure_key_fork, key, fork, SET);
            }
        }
    }
    return true;
}

std::wstring config_path() {
    PWSTR appdata = nullptr;
    SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata);
    std::filesystem::path dir = std::filesystem::path(appdata) / L"ForkingMachine";
    CoTaskMemFree(appdata);

    std::filesystem::create_directories(dir);   // no-op if it already exists; creates it (and any missing parent) if not

    return (dir / L"config.txt").wstring();
}

constexpr int HOTKEY_ANNOTATE = 1;
LRESULT CALLBACK MsgWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HOTKEY:
        if (wParam == HOTKEY_ANNOTATE) {
            uint64_t marked_seq = g_event_seq;             // capture "now" immediately, before any UI delay
#if 0
            std::wstring note = prompt_for_text(hwnd);       // small modal, see below
            if (!note.empty() && g_event_log) {
                fwprintf(g_event_log, L"ANNOTATION\tup_to_seq=%llu\t%ls\n", marked_seq, note.c_str());
                fflush(g_event_log);
            }
#endif
        }
        return 0;
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
            AppendMenuW(menu, MF_STRING | (g_debug_enabled ? MF_CHECKED : 0), 3, L"Debug logging");
            AppendMenuW(menu, MF_STRING | (is_start_on_login_enabled() ? MF_CHECKED : 0), 4, L"Start at login");

            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(hwnd); // required so the menu dismisses correctly
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
            switch (cmd) {
            case 1:
                g_env.log("reload config\n");
                if (load_config_from_file(config_path(), g_processor)) {
                    save_configuration_to_registry(g_processor);
                };
                break;
            case 2: PostQuitMessage(0); break;
            case 3:
                g_debug_enabled = !g_debug_enabled; // as int
                g_processor.configure_global(fork_configure_debug, g_debug_enabled, SET);
                // ....
                break;
            case 4:
                set_start_on_login(!is_start_on_login_enabled());
            }
            DestroyMenu(menu);
        }
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {

    g_processor.create_configs();
    restore_configuration_from_registry(g_processor);   // from the earlier message

    // in WinMain:
    // g_event_log=open()
    // g_processor.register_dumper(file_dumper);

    WNDCLASS wc{}; wc.lpfnWndProc = MsgWindowProc; wc.hInstance = hInstance; wc.lpszClassName = L"ForkMsgWin";
    RegisterClassW(&wc);
    g_msg_window = CreateWindowExW(0, L"ForkMsgWin", L"", 0, 0,0,0,0, HWND_MESSAGE, nullptr, hInstance, nullptr);

    // toolbar:
    NOTIFYICONDATA nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_msg_window;          // reuse the message-only window from the timer code
    nid.uID = 1;

    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 1;
    nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION); // placeholder; real icon later
    wcscpy_s(nid.szTip, L"ForkingMachine");
    Shell_NotifyIcon(NIM_ADD, &nid);

    // hotkey:
    RegisterHotKey(g_msg_window, HOTKEY_ANNOTATE, MOD_CONTROL | MOD_ALT, VK_F9);

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
