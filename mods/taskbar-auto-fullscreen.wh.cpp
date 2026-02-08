// ==WindhawkMod==
// @id              taskbar-auto-fullscreen
// @name            Auto Move Taskbar on Fullscreen
// @description     Automatically moves the primary taskbar to a secondary monitor when fullscreen applications are detected, and restores it when the application closes
// @version         1.0
// @author          Created by Assistant, based on taskbar-primary-on-secondary-monitor by m417z
// @github          https://github.com/m417z
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lversion
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// This mod is based on and uses code from "Primary taskbar on secondary monitor"
// by m417z (https://github.com/m417z). The core hooking mechanism is adapted from
// that mod, with added fullscreen detection and automatic taskbar management.
//
// For bug reports and feature requests, please open an issue on the Windhawk mods repository:
// https://github.com/ramensoftware/windhawk-mods/issues

// ==WindhawkModReadme==
/*
# Auto Move Taskbar on Fullscreen

Automatically detects when you launch fullscreen applications (games, media players, etc.) on your primary monitor and moves the Windows taskbar to your secondary monitor. The taskbar stays on the secondary monitor until you close the fullscreen application.

## Why Use This Mod?

When gaming or watching videos in fullscreen, you often need to access the taskbar (system tray, notifications, action center, etc.) without exiting fullscreen. This mod solves that problem by automatically moving the taskbar to your secondary monitor when it detects fullscreen mode.

## How It Works

1. **Fullscreen Detected**: When you launch a fullscreen app on your primary monitor (e.g., a game, VLC in fullscreen), the mod automatically moves your taskbar to the secondary monitor
2. **Stays There**: Even if you minimize the game or Alt+Tab out, the taskbar remains on the secondary monitor as long as the application is still running
3. **Use Freely**: Click the taskbar, access system tray, check notifications - all without the taskbar disappearing
4. **Auto Restore**: When you completely close the fullscreen application, the taskbar automatically returns to your primary monitor

## Key Features

- **Smart Detection**: Uses the same reliable method as the "Primary taskbar on secondary monitor" mod
- **Process-Based Tracking**: Monitors the fullscreen application window - taskbar only returns when you actually close the app
- **Performance Optimized**: 
  - Low priority background thread won't impact game performance
  - Smart window change detection reduces unnecessary checks
  - Optional logging can be disabled for maximum performance
- **Works With**:
  - Games (Alan Wake 2, any fullscreen game)
  - Media players (VLC, Windows Media Player)
  - Any application that goes fullscreen on your primary monitor
- **Handles Edge Cases**:
  - Borderless windowed mode (with 10px tolerance)
  - Multiple monitor setups
  - Portrait/landscape monitor orientations

## Requirements

- **Windows 10 or Windows 11**
- **At least 2 monitors** connected to your system
- **Important**: If you have "Primary taskbar on secondary monitor" mod installed, make sure it is **DISABLED** (this mod replaces its functionality automatically)

## Recommended Settings

- **Secondary Monitor**: 1 (first non-primary monitor)
- **Poll Interval**: 2000-3000ms (2-3 seconds is plenty fast)
- **Enable Logging**: false (disable for best performance during gaming)

## Technical Details

This mod hooks the `TrayUI::_SetStuckMonitor` function in `taskbar.dll` (the same function used by the manual toggle mod) and intercepts Windows' taskbar positioning logic. When fullscreen is detected, it forces the taskbar to use your secondary monitor instead of the primary.

## Credits

Based on the excellent "Primary taskbar on secondary monitor" mod by m417z (https://github.com/m417z). This mod extends that functionality with automatic fullscreen detection.

## Troubleshooting

**Taskbar doesn't move**: Make sure you have at least 2 monitors and check that "Primary taskbar on secondary monitor" mod is disabled.

**Performance issues**: Increase the poll interval to 3000-5000ms and disable logging.

**Wrong monitor**: Change the "Secondary Monitor" setting (1 = first non-primary, 2 = second non-primary, etc.)
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- secondaryMonitor: 1
  $name: Secondary Monitor
  $description: Which secondary monitor to move the taskbar to (1 = first non-primary monitor, 2 = second non-primary monitor, etc.)
- pollInterval: 2000
  $name: Poll Interval (milliseconds)
  $description: How often to check for fullscreen applications. Higher values use less CPU. Recommended range is 2000-3000ms (2-3 seconds).
- enableLogging: false
  $name: Enable Debug Logging
  $description: Enable detailed logging for troubleshooting. Disable this for best performance during gaming. Logs can be viewed in Windhawk's log viewer or with DbgView.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <atomic>

struct {
    int secondaryMonitor;
    int pollInterval;
    bool enableLogging;
} g_settings;

std::atomic<bool> g_initialized;
std::atomic<bool> g_unloading;
std::atomic<bool> g_forceSecondary;

HMONITOR g_primaryMonitor = nullptr;
HMONITOR g_secondaryMonitor = nullptr;
HWND g_taskbarHwnd = nullptr;
HANDLE g_monitorThread = nullptr;
bool g_running = false;
HWND g_fullscreenApp = nullptr;  // Track which app triggered fullscreen mode

void Log(const wchar_t* msg) {
    if (!g_settings.enableLogging) return; // Skip logging if disabled for performance
    
    Wh_Log(L"%s", msg);
    wchar_t buffer[600];
    swprintf(buffer, 600, L"[TaskbarAutoFS] %s\n", msg);
    OutputDebugStringW(buffer);
}

HWND GetTaskbarWnd() {
    HWND hTaskbarWnd = FindWindow(L"Shell_TrayWnd", nullptr);
    DWORD processId = 0;
    if (!hTaskbarWnd || !GetWindowThreadProcessId(hTaskbarWnd, &processId) ||
        processId != GetCurrentProcessId()) {
        return nullptr;
    }
    return hTaskbarWnd;
}

HMONITOR GetMonitorByIndex(int index) {
    HMONITOR monitorResult = nullptr;
    int currentIndex = 0;
    
    auto monitorEnumProc = [&](HMONITOR hMonitor) -> BOOL {
        MONITORINFOEX mi;
        mi.cbSize = sizeof(MONITORINFOEX);
        
        if (GetMonitorInfo(hMonitor, &mi)) {
            if (mi.dwFlags & MONITORINFOF_PRIMARY) {
                return TRUE; // Skip primary
            }
            
            if (currentIndex == index) {
                monitorResult = hMonitor;
                return FALSE;
            }
            currentIndex++;
        }
        return TRUE;
    };
    
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR hMonitor, HDC hdc, LPRECT lprcMonitor, LPARAM dwData) -> BOOL {
            auto& proc = *reinterpret_cast<decltype(monitorEnumProc)*>(dwData);
            return proc(hMonitor);
        },
        reinterpret_cast<LPARAM>(&monitorEnumProc));
    
    return monitorResult;
}

void RefreshMonitors() {
    g_primaryMonitor = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    g_secondaryMonitor = GetMonitorByIndex(g_settings.secondaryMonitor - 1);
    
    if (g_primaryMonitor) {
        MONITORINFOEX mi;
        mi.cbSize = sizeof(MONITORINFOEX);
        if (GetMonitorInfo(g_primaryMonitor, &mi)) {
            wchar_t msg[200];
            swprintf(msg, 200, L"Primary monitor: %s", mi.szDevice);
            Log(msg);
        }
    }
    
    if (g_secondaryMonitor) {
        MONITORINFOEX mi;
        mi.cbSize = sizeof(MONITORINFOEX);
        if (GetMonitorInfo(g_secondaryMonitor, &mi)) {
            wchar_t msg[200];
            swprintf(msg, 200, L"Secondary monitor: %s", mi.szDevice);
            Log(msg);
        }
    }
}

// Hook MonitorFromPoint
using MonitorFromPoint_t = decltype(&MonitorFromPoint);
MonitorFromPoint_t MonitorFromPoint_Original;
HMONITOR WINAPI MonitorFromPoint_Hook(POINT pt, DWORD dwFlags) {
    if (pt.x == 0 && pt.y == 0 && g_forceSecondary && g_secondaryMonitor) {
        return g_secondaryMonitor;
    }
    return MonitorFromPoint_Original(pt, dwFlags);
}

// Hook TrayUI::_SetStuckMonitor
using TrayUI__SetStuckMonitor_t = HRESULT(WINAPI*)(void* pThis, HMONITOR monitor);
TrayUI__SetStuckMonitor_t TrayUI__SetStuckMonitor_Original;
HRESULT WINAPI TrayUI__SetStuckMonitor_Hook(void* pThis, HMONITOR monitor) {
    // Fast path: if not forcing anything, just pass through immediately
    if (!g_forceSecondary && g_unloading) {
        return TrayUI__SetStuckMonitor_Original(pThis, monitor);
    }
    
    // Only log if logging is enabled (reduces overhead)
    if (g_settings.enableLogging) {
        Wh_Log(L"TrayUI::_SetStuckMonitor called");
    }
    
    if (!g_unloading && g_forceSecondary && g_secondaryMonitor) {
        if (g_settings.enableLogging) {
            Wh_Log(L"  Forcing secondary monitor");
        }
        monitor = g_secondaryMonitor;
    } else if (!g_forceSecondary && g_primaryMonitor) {
        if (g_settings.enableLogging) {
            Wh_Log(L"  Using primary monitor");
        }
        monitor = g_primaryMonitor;
    }
    
    if (!monitor) {
        monitor = MonitorFromPoint_Original({0, 0}, MONITOR_DEFAULTTONEAREST);
    }
    
    return TrayUI__SetStuckMonitor_Original(pThis, monitor);
}

bool IsWindowFullscreen(HWND hWnd) {
    if (!hWnd || !IsWindowVisible(hWnd)) return false;
    
    HWND desktop = GetDesktopWindow();
    HWND shell = GetShellWindow();
    if (hWnd == desktop || hWnd == shell || hWnd == g_taskbarHwnd) {
        return false;
    }
    
    LONG style = GetWindowLong(hWnd, GWL_STYLE);
    if (!(style & WS_VISIBLE)) return false;
    
    RECT windowRect;
    if (!GetWindowRect(hWnd, &windowRect)) return false;
    
    HMONITOR hMonitor = MonitorFromPoint_Original(
        {windowRect.left, windowRect.top}, MONITOR_DEFAULTTONEAREST);
    if (hMonitor != g_primaryMonitor) return false;
    
    MONITORINFO mi;
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfo(hMonitor, &mi)) return false;
    
    int monitorWidth = mi.rcMonitor.right - mi.rcMonitor.left;
    int monitorHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
    int windowWidth = windowRect.right - windowRect.left;
    int windowHeight = windowRect.bottom - windowRect.top;
    
    bool coversMonitor = (windowWidth >= monitorWidth - 10 && 
                         windowHeight >= monitorHeight - 10);
    
    return coversMonitor;
}

void ApplyTaskbarSettings() {
    if (!g_taskbarHwnd) {
        g_taskbarHwnd = GetTaskbarWnd();
    }
    
    if (g_taskbarHwnd) {
        // Trigger CTray::_HandleDisplayChange
        SendMessage(g_taskbarHwnd, 0x5B8, 0, 0);
    }
}

DWORD WINAPI MonitorThreadFunc(LPVOID lpParam) {
    Log(L"Monitor thread started");
    
    if (!g_secondaryMonitor) {
        Log(L"ERROR: No secondary monitor!");
        return 1;
    }
    
    int iteration = 0;
    HWND lastCheckedWindow = nullptr;
    
    // Set thread priority to low to avoid impacting game performance
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    
    while (g_running) {
        iteration++;
        
        // Only log every 60 iterations to reduce log spam
        if (iteration % 60 == 1) {
            wchar_t msg[100];
            swprintf(msg, 100, L"Thread alive - iteration %d", iteration);
            Log(msg);
        }
        
        HWND hWnd = GetForegroundWindow();
        
        // Check if our tracked fullscreen app is still alive
        if (g_fullscreenApp && !IsWindow(g_fullscreenApp)) {
            // The fullscreen app was closed!
            Log(L">>> FULLSCREEN APP CLOSED - RESTORING TASKBAR TO PRIMARY");
            g_fullscreenApp = nullptr;
            g_forceSecondary = false;
            ApplyTaskbarSettings();
        }
        
        // Optimization: Only check fullscreen status if foreground window changed
        bool needsCheck = (hWnd != lastCheckedWindow);
        lastCheckedWindow = hWnd;
        
        if (needsCheck) {
            bool isFullscreen = IsWindowFullscreen(hWnd);
            
            if (isFullscreen && !g_fullscreenApp) {
                // New fullscreen app detected
                wchar_t title[256] = {0};
                GetWindowTextW(hWnd, title, 256);
                wchar_t msg[300];
                swprintf(msg, 300, L">>> FULLSCREEN: %s", title[0] ? title : L"<no title>");
                Log(msg);
                
                g_fullscreenApp = hWnd;  // Track this app
                g_forceSecondary = true;
                ApplyTaskbarSettings();
            }
        }
        
        Sleep(g_settings.pollInterval);
    }
    
    Log(L"Monitor thread stopped");
    return 0;
}

bool HookTaskbarSymbols() {
    // Load taskbar.dll - the symbols are there, not in explorer.exe!
    HMODULE module = LoadLibrary(L"taskbar.dll");
    if (!module) {
        Log(L"ERROR: Could not load taskbar.dll");
        return false;
    }
    
    Log(L"taskbar.dll loaded successfully");
    
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {
                LR"(public: long __cdecl TrayUI::_SetStuckMonitor(struct HMONITOR__ *))",
                LR"(public: void __cdecl TrayUI::_SetStuckMonitor(struct HMONITOR__ *))",
            },
            &TrayUI__SetStuckMonitor_Original,
            TrayUI__SetStuckMonitor_Hook,
        },
    };

    return HookSymbols(module, symbolHooks, ARRAYSIZE(symbolHooks));
}

void LoadSettings() {
    g_settings.secondaryMonitor = Wh_GetIntSetting(L"secondaryMonitor");
    g_settings.pollInterval = Wh_GetIntSetting(L"pollInterval");
    g_settings.enableLogging = Wh_GetIntSetting(L"enableLogging");
}

BOOL Wh_ModInit() {
    Log(L"========================================");
    Log(L"Initializing Auto Taskbar on Fullscreen");
    Log(L"========================================");
    
    LoadSettings();
    
    g_taskbarHwnd = GetTaskbarWnd();
    if (!g_taskbarHwnd) {
        Log(L"WARNING: Taskbar window not found");
    }
    
    RefreshMonitors();
    
    if (!g_primaryMonitor || !g_secondaryMonitor) {
        Log(L"ERROR: Need at least 2 monitors");
        return FALSE;
    }
    
    if (!HookTaskbarSymbols()) {
        Log(L"ERROR: Failed to hook taskbar symbols");
        return FALSE;
    }
    
    Log(L"Taskbar symbols hooked successfully");
    
    WindhawkUtils::Wh_SetFunctionHookT(MonitorFromPoint, MonitorFromPoint_Hook,
                                       &MonitorFromPoint_Original);
    
    g_initialized = true;
    
    g_running = true;
    g_monitorThread = CreateThread(nullptr, 0, MonitorThreadFunc, nullptr, 0, nullptr);
    
    if (!g_monitorThread) {
        Log(L"ERROR: Failed to create monitor thread");
        return FALSE;
    }
    
    Log(L"Mod initialized successfully!");
    Log(L"========================================");
    return TRUE;
}

void Wh_ModUninit() {
    Log(L"Uninitializing...");
    
    g_unloading = true;
    g_running = false;
    
    if (g_monitorThread) {
        WaitForSingleObject(g_monitorThread, 5000);
        CloseHandle(g_monitorThread);
    }
    
    // Always restore taskbar when unloading
    g_fullscreenApp = nullptr;
    g_forceSecondary = false;
    ApplyTaskbarSettings();
    
    Log(L"Uninitialized");
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    RefreshMonitors();
}
