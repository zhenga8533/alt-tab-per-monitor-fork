// ==WindhawkMod==
// @id              alt-tab-per-monitor-fork
// @name            Alt+Tab per monitor - Fork
// @description     Pressing Alt+Tab shows all open windows on the primary display. This mod shows only the windows on the current monitor.
// @version         1.4
// @author          zhenga8533
// @github          https://github.com/zhenga8533
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -ldwmapi -lole32 -loleaut32 -lversion
// ==/WindhawkMod==

// ==WindhawkModReadme==

/*
**# Alt+Tab per monitor - Fork**

A fork of [Alt+Tab per monitor](https://windhawk.net/mods/alt-tab-per-monitor)
by L3r0y.

By default, pressing Alt+Tab shows all open windows on the primary display. This
mod customizes the behavior so that Alt+Tab displays the switcher on the current
monitor, showing only the windows on that monitor. The current monitor is either
the monitor where the cursor is located or the monitor of the focused window.

Both Alt+Tab and Win+Alt+Tab can be independently configured with:
\- ****Display location****: Primary monitor or current monitor.
\- ****Windows to show****: All monitors, current monitor only, or current monitor
  with an exception for the primary monitor (shows all windows when the current
  monitor is the primary monitor).

Additionally:
\- If the current monitor has no windows, all windows can be shown instead.
\- Apps can be listed to always show in Alt+Tab, regardless of their monitor.
  UWP apps are hosted by `ApplicationFrameHost.exe`, so they can't be matched by
  their own process name.

![Gif](https://i.imgur.com/Hpg8TKh.gif)
*/

// ==/WindhawkModReadme==

// ==WindhawkModSettings==

/*
- currentMonitor: cursor
  $name: Current monitor
  $description: Choose which monitor is considered the current monitor
  $options:
  - cursor: Monitor where cursor is located
  - foregroundWindow: Monitor of the focused window

- altTabLocation: cursor
  $name: "Alt+Tab: Display location"
  $description: Choose which monitor the Alt+Tab switcher appears on
  $options:
  - cursor: Current monitor
  - primary: Primary monitor

- altTabWindows: cursorMonitor
  $name: "Alt+Tab: Windows to show"
  $description: Choose which windows to show when using Alt+Tab
  $options:
  - cursorMonitor: Windows from the current monitor
  - allMonitors: Windows from all monitors
  - cursorMonitorExceptPrimary: >-
      Windows from the current monitor, but all windows when on primary

- winAltTabLocation: primary
  $name: "Win+Alt+Tab: Display location"
  $description: >-
    Choose which monitor the Alt+Tab switcher appears on when using Win+Alt+Tab
  $options:
  - primary: Primary monitor
  - cursor: Current monitor

- winAltTabWindows: allMonitors
  $name: "Win+Alt+Tab: Windows to show"
  $description: Choose which windows to show when using Win+Alt+Tab
  $options:
  - allMonitors: Windows from all monitors
  - cursorMonitor: Windows from the current monitor
  - cursorMonitorExceptPrimary: >-
      Windows from the current monitor, but all windows when on primary

- showAllWhenMonitorEmpty: true
  $name: Show all windows when the current monitor has none
  $description: >-
    Instead of an empty switcher, show windows from all monitors

- alwaysShowApps: [""]
  $name: Always show apps
  $description: >-
    Process names (e.g. spotify.exe) or full paths of apps that are always
    shown in Alt+Tab, regardless of their monitor
*/

// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <dwmapi.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/windows.foundation.collections.h>

enum class CurrentMonitor {
    cursor,
    foregroundWindow,
};

enum class Location {
    primary,
    currentMonitor,
};

enum class WindowsToShow {
    allMonitors,
    currentMonitor,
    currentMonitorExceptPrimary,
};

struct {
    CurrentMonitor currentMonitor;
    Location altTabLocation;
    WindowsToShow altTabWindows;
    Location winAltTabLocation;
    WindowsToShow winAltTabWindows;
    bool showAllWhenMonitorEmpty;
} g_settings;

std::mutex g_alwaysShowAppsMutex;
std::shared_ptr<const std::vector<std::wstring>> g_alwaysShowApps;

enum class WinVersion {
    Unsupported,
    Win10,
    Win11,
};

WinVersion g_winVersion;
std::atomic<DWORD> g_threadIdForAltTabShowWindow;
std::atomic<DWORD> g_altTabThreadId;
std::atomic<DWORD> g_altTabCreatingThreadId;
std::atomic<ULONGLONG> g_altTabCreatedTickCount;
std::atomic<HMONITOR> g_altTabMonitor;
std::atomic<bool> g_altTabOpen;
bool g_dismissViewHooked;
constexpr ULONGLONG kDeltaThreshold = 200;

VS_FIXEDFILEINFO* GetModuleVersionInfo(HMODULE hModule, UINT* puPtrLen) {
    void* pFixedFileInfo = nullptr;
    UINT uPtrLen = 0;
    HRSRC hResource =
        FindResource(hModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);

    if (hResource) {
        HGLOBAL hGlobal = LoadResource(hModule, hResource);

        if (hGlobal) {
            void* pData = LockResource(hGlobal);

            if (pData) {
                if (!VerQueryValue(pData, L"\\", &pFixedFileInfo, &uPtrLen) ||
                    uPtrLen == 0) {
                    pFixedFileInfo = nullptr;
                    uPtrLen = 0;
                }
            }
        }
    }

    if (puPtrLen) {
        *puPtrLen = uPtrLen;
    }

    return (VS_FIXEDFILEINFO*)pFixedFileInfo;
}

WinVersion GetWindowsVersion() {
    VS_FIXEDFILEINFO* fixedFileInfo = GetModuleVersionInfo(nullptr, nullptr);

    if (!fixedFileInfo) {
        return WinVersion::Unsupported;
    }

    WORD major = HIWORD(fixedFileInfo->dwFileVersionMS);
    WORD minor = LOWORD(fixedFileInfo->dwFileVersionMS);
    WORD build = HIWORD(fixedFileInfo->dwFileVersionLS);
    WORD qfe = LOWORD(fixedFileInfo->dwFileVersionLS);
    Wh_Log(L"Version: %u.%u.%u.%u", major, minor, build, qfe);

    switch (major) {
        case 10:
            if (build < 22000) {
                return WinVersion::Win10;
            } else {
                return WinVersion::Win11;
            }

            break;
    }

    return WinVersion::Unsupported;
}

bool IsWinKeyPressed() {
    return GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0;
}

bool IsDesktopWindow(HWND windowHandle) {
    WCHAR className[32];

    if (!GetClassName(windowHandle, className, ARRAYSIZE(className))) {
        return false;
    }

    return wcscmp(className, L"Progman") == 0 ||
           wcscmp(className, L"WorkerW") == 0;
}

HMONITOR GetCurrentMonitor() {
    if (g_settings.currentMonitor == CurrentMonitor::foregroundWindow) {
        HWND foregroundWindow = GetForegroundWindow();

        // With the desktop focused, there's no meaningful window monitor, so
        // fall back to the cursor.
        if (foregroundWindow && !IsDesktopWindow(foregroundWindow)) {
            return MonitorFromWindow(foregroundWindow,
                                     MONITOR_DEFAULTTONEAREST);
        }
    }

    POINT pt;

    if (!GetCursorPos(&pt)) {
        return nullptr;
    }

    return MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
}

bool IsPrimaryMonitor(HMONITOR monitor) {
    MONITORINFO monInfo;
    monInfo.cbSize = sizeof(MONITORINFO);

    return GetMonitorInfo(monitor, &monInfo) &&
           (monInfo.dwFlags & MONITORINFOF_PRIMARY);
}

// Approximates the rules Alt+Tab uses to decide which windows to list.
bool IsAltTabCandidate(HWND windowHandle) {
    if (!IsWindowVisible(windowHandle) || IsDesktopWindow(windowHandle)) {
        return false;
    }

    LONG_PTR exStyle = GetWindowLongPtr(windowHandle, GWL_EXSTYLE);

    if (exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) {
        return false;
    }

    if (GetWindow(windowHandle, GW_OWNER) && !(exStyle & WS_EX_APPWINDOW)) {
        return false;
    }

    // Windows on other virtual desktops are cloaked.
    BOOL cloaked = FALSE;

    if (SUCCEEDED(DwmGetWindowAttribute(windowHandle, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked))) &&
        cloaked) {
        return false;
    }

    return true;
}

bool MonitorHasAltTabWindows(HMONITOR monitor) {
    struct Context {
        HMONITOR monitor;
        bool found;
    } context{monitor, false};

    EnumWindows(
        [](HWND windowHandle, LPARAM lParam) -> BOOL {
            auto* context = reinterpret_cast<Context*>(lParam);

            if (IsAltTabCandidate(windowHandle) &&
                MonitorFromWindow(windowHandle, MONITOR_DEFAULTTONEAREST) ==
                    context->monitor) {
                context->found = true;
                return FALSE;
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));

    return context.found;
}

std::wstring GetWindowProcessPath(HWND windowHandle) {
    DWORD processId = 0;
    GetWindowThreadProcessId(windowHandle, &processId);

    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);

    if (!process) {
        return {};
    }

    WCHAR path[MAX_PATH];
    DWORD pathSize = ARRAYSIZE(path);
    BOOL succeeded = QueryFullProcessImageName(process, 0, path, &pathSize);
    CloseHandle(process);

    return succeeded ? std::wstring(path, pathSize) : std::wstring();
}

bool IsAlwaysShownApp(HWND windowHandle) {
    std::shared_ptr<const std::vector<std::wstring>> alwaysShowApps;

    {
        std::lock_guard<std::mutex> lock(g_alwaysShowAppsMutex);
        alwaysShowApps = g_alwaysShowApps;
    }

    if (!alwaysShowApps || alwaysShowApps->empty()) {
        return false;
    }

    std::wstring path = GetWindowProcessPath(windowHandle);

    if (path.empty()) {
        return false;
    }

    PCWSTR fileName = wcsrchr(path.c_str(), L'\\');
    fileName = fileName ? fileName + 1 : path.c_str();

    for (const auto& app : *alwaysShowApps) {
        PCWSTR target = wcschr(app.c_str(), L'\\') ? path.c_str() : fileName;

        if (_wcsicmp(app.c_str(), target) == 0) {
            return true;
        }
    }

    return false;
}

bool HandleAltTabWindow(RECT* rect) {
    auto location = IsWinKeyPressed() ? g_settings.winAltTabLocation
                                      : g_settings.altTabLocation;

    if (location == Location::primary) {
        return false;
    }

    HMONITOR hMon = g_altTabMonitor;

    if (!hMon) {
        return false;
    }

    MONITORINFO monInfo;
    monInfo.cbSize = sizeof(MONITORINFO);

    if (!GetMonitorInfo(hMon, &monInfo)) {
        return false;
    }

    *rect = monInfo.rcWork;

    return true;
}

void* CWin32ApplicationView_vtable;
void* CWinRTApplicationView_vtable;
using CWin32ApplicationView_v_GetNativeWindow_t =
    HRESULT(WINAPI*)(void* pThis, HWND* windowHandle);
CWin32ApplicationView_v_GetNativeWindow_t
    CWin32ApplicationView_v_GetNativeWindow;
using CWinRTApplicationView_v_GetNativeWindow_t =
    HRESULT(WINAPI*)(void* pThis, HWND* windowHandle);
CWinRTApplicationView_v_GetNativeWindow_t
    CWinRTApplicationView_v_GetNativeWindow;

HRESULT GetWindowHandleFromApplicationView(void* applicationView,
                                           HWND* windowHandle) {
    *windowHandle = nullptr;
    void* vtable = *(void**)applicationView;

    HRESULT hr = E_FAIL;

    if (vtable == CWin32ApplicationView_vtable) {
        hr = CWin32ApplicationView_v_GetNativeWindow(applicationView,
                                                     windowHandle);
    } else if (vtable == CWinRTApplicationView_vtable) {
        hr = CWinRTApplicationView_v_GetNativeWindow(applicationView,
                                                     windowHandle);
    }

    return hr;
}

bool ShouldShowWindow(HWND windowHandle) {
    return MonitorFromWindow(windowHandle, MONITOR_DEFAULTTONEAREST) ==
               g_altTabMonitor ||
           IsAlwaysShownApp(windowHandle);
}

bool ShouldFilterViewVisibility() {
    DWORD threadId = GetCurrentThreadId();

    if (g_altTabCreatingThreadId == threadId) {
        return true;
    }

    // Windows can be added to the switcher after it was created, e.g. a window
    // that becomes active or flashes while Alt+Tab is open. Keep filtering on
    // the Alt+Tab thread until it's dismissed. Calls from other threads are left
    // alone, as the same function is also used by the taskbar and Win+Tab.
    if (g_altTabThreadId != threadId) {
        return false;
    }

    return g_altTabOpen ||
           (GetTickCount64() - g_altTabCreatedTickCount) <= kDeltaThreshold;
}

using CVirtualDesktop_IsViewVisible_t = HRESULT(WINAPI*)(void* pThis,
                                                         void* applicationView,
                                                         BOOL* isVisible);
CVirtualDesktop_IsViewVisible_t CVirtualDesktop_IsViewVisible_Original;

HRESULT WINAPI CVirtualDesktop_IsViewVisible_Hook(void* pThis,
                                                  void* applicationView,
                                                  BOOL* isVisible) {
    auto ret = CVirtualDesktop_IsViewVisible_Original(pThis, applicationView,
                                                      isVisible);

    if (FAILED(ret) || !*isVisible || !ShouldFilterViewVisibility()) {
        return ret;
    }

    HWND windowHandle;

    HRESULT hr =
        GetWindowHandleFromApplicationView(applicationView, &windowHandle);

    if (FAILED(hr) || !windowHandle) {
        return ret;
    }

    if (!ShouldShowWindow(windowHandle)) {
        Wh_Log(L"Hiding window %p", windowHandle);
        *isVisible = FALSE;
    }

    return ret;
}

using AltTabViewHost_DismissView_t = void(WINAPI*)(void* pThis);
AltTabViewHost_DismissView_t XamlAltTabViewHost_DismissView_Original;
AltTabViewHost_DismissView_t CAltTabViewHost_DismissView_Original;

void WINAPI XamlAltTabViewHost_DismissView_Hook(void* pThis) {
    Wh_Log(L">");
    g_altTabOpen = false;
    XamlAltTabViewHost_DismissView_Original(pThis);
}

void WINAPI CAltTabViewHost_DismissView_Hook(void* pThis) {
    Wh_Log(L">");
    g_altTabOpen = false;
    CAltTabViewHost_DismissView_Original(pThis);
}

using XamlAltTabViewHost_Show_t = HRESULT(WINAPI*)(void* pThis,
                                                   void* param1,
                                                   int param2,
                                                   void* param3);
XamlAltTabViewHost_Show_t XamlAltTabViewHost_Show_Original;

HRESULT WINAPI XamlAltTabViewHost_Show_Hook(void* pThis,
                                            void* param1,
                                            int param2,
                                            void* param3) {
    Wh_Log(L">");
    g_threadIdForAltTabShowWindow = GetCurrentThreadId();

    HRESULT ret =
        XamlAltTabViewHost_Show_Original(pThis, param1, param2, param3);
    g_threadIdForAltTabShowWindow = 0;

    return ret;
}

using CAltTabViewHost_Show_t = HRESULT(WINAPI*)(void* pThis,
                                                void* param1,
                                                int param2,
                                                void* param3);
CAltTabViewHost_Show_t CAltTabViewHost_Show_Original;

HRESULT WINAPI CAltTabViewHost_Show_Hook(void* pThis,
                                         void* param1,
                                         int param2,
                                         void* param3) {
    Wh_Log(L">");
    g_threadIdForAltTabShowWindow = GetCurrentThreadId();

    HRESULT ret = CAltTabViewHost_Show_Original(pThis, param1, param2, param3);
    g_threadIdForAltTabShowWindow = 0;

    return ret;
}

using ITaskGroupWindowInformation_Position_t =
    HRESULT(WINAPI*)(void* pThis, winrt::Windows::Foundation::Rect* rect);
ITaskGroupWindowInformation_Position_t
    ITaskGroupWindowInformation_Position_Original;

HRESULT WINAPI ITaskGroupWindowInformation_Position_Hook(
    void* pThis,
    winrt::Windows::Foundation::Rect* rect) {
    if (g_threadIdForAltTabShowWindow != GetCurrentThreadId()) {
        return ITaskGroupWindowInformation_Position_Original(pThis, rect);
    }

    g_threadIdForAltTabShowWindow = 0;

    RECT newRectNative;

    if (!HandleAltTabWindow(&newRectNative)) {
        return ITaskGroupWindowInformation_Position_Original(pThis, rect);
    }

    winrt::Windows::Foundation::Rect newRect{
        static_cast<float>(newRectNative.left),
        static_cast<float>(newRectNative.top),
        static_cast<float>(newRectNative.right - newRectNative.left),
        static_cast<float>(newRectNative.bottom - newRectNative.top),
    };

    HRESULT ret =
        ITaskGroupWindowInformation_Position_Original(pThis, &newRect);

    return ret;
}

using CMultitaskingViewFrame_CreateFrame_t = HRESULT(WINAPI*)(void* pThis,
                                                              RECT* rect,
                                                              void* param2);
CMultitaskingViewFrame_CreateFrame_t
    CMultitaskingViewFrame_CreateFrame_Original;

HRESULT WINAPI CMultitaskingViewFrame_CreateFrame_Hook(void* pThis,
                                                       RECT* rect,
                                                       void* param2) {
    if (g_threadIdForAltTabShowWindow != GetCurrentThreadId()) {
        return CMultitaskingViewFrame_CreateFrame_Original(pThis, rect, param2);
    }

    g_threadIdForAltTabShowWindow = 0;

    RECT newRect;

    if (!HandleAltTabWindow(&newRect)) {
        return CMultitaskingViewFrame_CreateFrame_Original(pThis, rect, param2);
    }

    HRESULT ret =
        CMultitaskingViewFrame_CreateFrame_Original(pThis, &newRect, param2);

    return ret;
}

HRESULT CreateInstanceHook(std::function<HRESULT()> original) {
    auto windows = IsWinKeyPressed() ? g_settings.winAltTabWindows
                                     : g_settings.altTabWindows;

    HMONITOR monitor = GetCurrentMonitor();
    g_altTabMonitor = monitor;

    bool filter = windows != WindowsToShow::allMonitors && monitor;

    if (filter && windows == WindowsToShow::currentMonitorExceptPrimary &&
        IsPrimaryMonitor(monitor)) {
        filter = false;
    }

    if (filter && g_settings.showAllWhenMonitorEmpty &&
        !MonitorHasAltTabWindows(monitor)) {
        Wh_Log(L"No windows on the current monitor, showing all windows");
        filter = false;
    }

    if (!filter) {
        g_altTabThreadId = 0;
        g_altTabOpen = false;

        return original();
    }

    g_altTabCreatingThreadId = GetCurrentThreadId();
    g_altTabThreadId = GetCurrentThreadId();
    g_altTabCreatedTickCount = GetTickCount64();
    g_altTabOpen = g_dismissViewHooked;

    HRESULT ret = original();
    g_altTabCreatingThreadId = 0;

    return ret;
}

using XamlAltTabViewHost_CreateInstance_t = HRESULT(WINAPI*)(void* pThis,
                                                             void* param1,
                                                             void* param2,
                                                             void* param3);
XamlAltTabViewHost_CreateInstance_t XamlAltTabViewHost_CreateInstance_Original;

HRESULT WINAPI XamlAltTabViewHost_CreateInstance_Hook(void* pThis,
                                                      void* param1,
                                                      void* param2,
                                                      void* param3) {
    Wh_Log(L">");

    return CreateInstanceHook([=]() {
        return XamlAltTabViewHost_CreateInstance_Original(pThis, param1, param2,
                                                          param3);
    });
}

using CAltTabViewHost_CreateInstance_t = HRESULT(WINAPI*)(void* pThis,
                                                          void* param1,
                                                          void* param2,
                                                          void* param3,
                                                          void* param4,
                                                          void* param5,
                                                          void* param6,
                                                          void* param7,
                                                          void* param8,
                                                          void* param9,
                                                          void* param10,
                                                          void* param11);
CAltTabViewHost_CreateInstance_t CAltTabViewHost_CreateInstance_Original;

HRESULT WINAPI CAltTabViewHost_CreateInstance_Hook(void* pThis,
                                                   void* param1,
                                                   void* param2,
                                                   void* param3,
                                                   void* param4,
                                                   void* param5,
                                                   void* param6,
                                                   void* param7,
                                                   void* param8,
                                                   void* param9,
                                                   void* param10,
                                                   void* param11) {
    Wh_Log(L">");

    return CreateInstanceHook([=]() {
        return CAltTabViewHost_CreateInstance_Original(
            pThis, param1, param2, param3, param4, param5, param6, param7,
            param8, param9, param10, param11);
    });
}

using CAltTabViewHost_CreateInstance_Win11_t = HRESULT(WINAPI*)(void* pThis,
                                                                void* param1,
                                                                void* param2,
                                                                void* param3,
                                                                void* param4,
                                                                void* param5,
                                                                void* param6,
                                                                void* param7,
                                                                void* param8,
                                                                void* param9,
                                                                void* param10);
CAltTabViewHost_CreateInstance_Win11_t
    CAltTabViewHost_CreateInstance_Win11_Original;

HRESULT WINAPI CAltTabViewHost_CreateInstance_Win11_Hook(void* pThis,
                                                         void* param1,
                                                         void* param2,
                                                         void* param3,
                                                         void* param4,
                                                         void* param5,
                                                         void* param6,
                                                         void* param7,
                                                         void* param8,
                                                         void* param9,
                                                         void* param10) {
    Wh_Log(L">");

    return CreateInstanceHook([=]() {
        return CAltTabViewHost_CreateInstance_Win11_Original(
            pThis, param1, param2, param3, param4, param5, param6, param7,
            param8, param9, param10);
    });
}

void LoadSettings() {
    auto loadCurrentMonitor = [](PCWSTR name, CurrentMonitor defaultVal) {
        PCWSTR val = Wh_GetStringSetting(name);
        CurrentMonitor result = defaultVal;

        if (wcscmp(val, L"cursor") == 0) {
            result = CurrentMonitor::cursor;
        } else if (wcscmp(val, L"foregroundWindow") == 0) {
            result = CurrentMonitor::foregroundWindow;
        }

        Wh_FreeStringSetting(val);

        return result;
    };

    auto loadLocation = [](PCWSTR name, Location defaultVal) {
        PCWSTR val = Wh_GetStringSetting(name);
        Location result = defaultVal;

        if (wcscmp(val, L"primary") == 0) {
            result = Location::primary;
        } else if (wcscmp(val, L"cursor") == 0) {
            result = Location::currentMonitor;
        }

        Wh_FreeStringSetting(val);

        return result;
    };

    auto loadWindows = [](PCWSTR name, WindowsToShow defaultVal) {
        PCWSTR val = Wh_GetStringSetting(name);
        WindowsToShow result = defaultVal;

        if (wcscmp(val, L"allMonitors") == 0) {
            result = WindowsToShow::allMonitors;
        } else if (wcscmp(val, L"cursorMonitor") == 0) {
            result = WindowsToShow::currentMonitor;
        } else if (wcscmp(val, L"cursorMonitorExceptPrimary") == 0) {
            result = WindowsToShow::currentMonitorExceptPrimary;
        }

        Wh_FreeStringSetting(val);

        return result;
    };
    g_settings.currentMonitor =
        loadCurrentMonitor(L"currentMonitor", CurrentMonitor::cursor);
    g_settings.altTabLocation =
        loadLocation(L"altTabLocation", Location::currentMonitor);
    g_settings.altTabWindows =
        loadWindows(L"altTabWindows", WindowsToShow::currentMonitor);
    g_settings.winAltTabLocation =
        loadLocation(L"winAltTabLocation", Location::primary);
    g_settings.winAltTabWindows =
        loadWindows(L"winAltTabWindows", WindowsToShow::allMonitors);
    g_settings.showAllWhenMonitorEmpty =
        Wh_GetIntSetting(L"showAllWhenMonitorEmpty");

    auto alwaysShowApps = std::make_shared<std::vector<std::wstring>>();

    for (int i = 0;; i++) {
        PCWSTR app = Wh_GetStringSetting(L"alwaysShowApps[%d]", i);
        bool hasApp = *app;

        if (hasApp) {
            alwaysShowApps->emplace_back(app);
        }

        Wh_FreeStringSetting(app);

        if (!hasApp) {
            break;
        }
    }

    std::lock_guard<std::mutex> lock(g_alwaysShowAppsMutex);
    g_alwaysShowApps = std::move(alwaysShowApps);
}

BOOL Wh_ModInit() {
    Wh_Log(L">");
    LoadSettings();
    g_winVersion = GetWindowsVersion();
    HMODULE twinuiPcshellModule = LoadLibrary(L"twinui.pcshell.dll");

    if (!twinuiPcshellModule) {
        Wh_Log(L"Couldn't load twinui.pcshell.dll");

        return FALSE;
    }

    if (g_winVersion == WinVersion::Win11) {

        // twinui.pcshell.dll

        WindhawkUtils::SYMBOL_HOOK twinuiPcshellSymbolHooks[] = {
            {
                {LR"(public: virtual long __cdecl CVirtualDesktop::IsViewVisible(struct IApplicationView *,int *))"},
                &CVirtualDesktop_IsViewVisible_Original,
                CVirtualDesktop_IsViewVisible_Hook,
            },
            {
                {LR"(const CWin32ApplicationView::`vftable'{for `IApplicationView'})"},
                &CWin32ApplicationView_vtable,
            },
            {
                {LR"(private: virtual long __cdecl CWin32ApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
                &CWin32ApplicationView_v_GetNativeWindow,
            },
            {
                {LR"(const CWinRTApplicationView::`vftable'{for `IApplicationView'})"},
                &CWinRTApplicationView_vtable,
            },
            {
                {LR"(private: virtual long __cdecl CWinRTApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
                &CWinRTApplicationView_v_GetNativeWindow,
            },
            {
                {LR"(public: virtual long __cdecl XamlAltTabViewHost::Show(struct IImmersiveMonitor *,enum ALT_TAB_VIEW_FLAGS,struct IApplicationView *))"},
                &XamlAltTabViewHost_Show_Original,
                XamlAltTabViewHost_Show_Hook,
            },
            {
                {LR"(public: __cdecl winrt::impl::consume_Windows_Internal_Shell_TaskGroups_ITaskGroupWindowInformation<struct winrt::Windows::Internal::Shell::TaskGroups::ITaskGroupWindowInformation>::Position(struct winrt::Windows::Foundation::Rect const &)const )"},
                &ITaskGroupWindowInformation_Position_Original,
                ITaskGroupWindowInformation_Position_Hook,
            },
            {
                {LR"(long __cdecl XamlAltTabViewHost_CreateInstance(struct XamlViewHostInitializeArgs const &,struct _GUID const &,void * *))"},
                &XamlAltTabViewHost_CreateInstance_Original,
                XamlAltTabViewHost_CreateInstance_Hook,
            },
            {
                {LR"(protected: virtual void __cdecl XamlAltTabViewHost::v_DismissView(void))"},
                &XamlAltTabViewHost_DismissView_Original,
                XamlAltTabViewHost_DismissView_Hook,
                true,
            },

            // For the old Win10 (non-XAML) Alt+Tab (can be enabled with
            // ExplorerPatcher):

            {
                {LR"(public: virtual long __cdecl CAltTabViewHost::Show(struct IImmersiveMonitor *,enum ALT_TAB_VIEW_FLAGS,struct IApplicationView *))"},
                &CAltTabViewHost_Show_Original,
                CAltTabViewHost_Show_Hook,
                true,
            },
            {
                {LR"(public: virtual long __cdecl CMultitaskingViewFrame::CreateFrame(struct Geometry::CRect const &,enum MultitaskingViewZOrder))"},
                &CMultitaskingViewFrame_CreateFrame_Original,
                CMultitaskingViewFrame_CreateFrame_Hook,
                true,
            },
            {
                {LR"(long __cdecl CAltTabViewHost_CreateInstance(struct IMultitaskingData *,struct IMultitaskingViewManagerInternal *,struct IApplicationViewSwitcher *,struct IImmersiveAppCrusher *,struct IMultitaskingViewVisibilityServiceInternal *,struct IMultitaskingViewGestureState *,struct IApplicationViewCollection *,struct ITouchGestureSettings *,struct _GUID const &,void * *))"},
                &CAltTabViewHost_CreateInstance_Win11_Original,
                CAltTabViewHost_CreateInstance_Win11_Hook,
                true,
            },
            {
                {LR"(protected: virtual void __cdecl CAltTabViewHost::v_DismissView(void))"},
                &CAltTabViewHost_DismissView_Original,
                CAltTabViewHost_DismissView_Hook,
                true,
            },
        };

        if (!HookSymbols(twinuiPcshellModule, twinuiPcshellSymbolHooks,
                         ARRAYSIZE(twinuiPcshellSymbolHooks))) {
            Wh_Log(L"HookSymbols failed");

            return FALSE;
        }

        g_dismissViewHooked =
            XamlAltTabViewHost_DismissView_Original != nullptr &&
            CAltTabViewHost_DismissView_Original != nullptr;

    } else if (g_winVersion == WinVersion::Win10) {

        // twinui.pcshell.dll

        WindhawkUtils::SYMBOL_HOOK twinuiPcshellSymbolHooks[] = {
            {
                {LR"(public: virtual long __cdecl CVirtualDesktop::IsViewVisible(struct IApplicationView *,int *))"},
                &CVirtualDesktop_IsViewVisible_Original,
                CVirtualDesktop_IsViewVisible_Hook,
            },
            {
                {LR"(const CWin32ApplicationView::`vftable'{for `IApplicationView'})"},
                &CWin32ApplicationView_vtable,
            },
            {
                {LR"(private: virtual long __cdecl CWin32ApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
                &CWin32ApplicationView_v_GetNativeWindow,
            },
            {
                {LR"(const CWinRTApplicationView::`vftable'{for `IApplicationView'})"},
                &CWinRTApplicationView_vtable,
            },
            {
                {LR"(private: virtual long __cdecl CWinRTApplicationView::v_GetNativeWindow(struct HWND__ * *))"},
                &CWinRTApplicationView_v_GetNativeWindow,
            },
            {
                {LR"(public: virtual long __cdecl CAltTabViewHost::Show(struct IImmersiveMonitor *,enum ALT_TAB_VIEW_FLAGS,struct IApplicationView *))"},
                &CAltTabViewHost_Show_Original,
                CAltTabViewHost_Show_Hook,
            },
            {
                {LR"(public: virtual long __cdecl CMultitaskingViewFrame::CreateFrame(struct Geometry::CRect const &,enum MultitaskingViewZOrder))"},
                &CMultitaskingViewFrame_CreateFrame_Original,
                CMultitaskingViewFrame_CreateFrame_Hook,
            },
            {
                {LR"(long __cdecl CAltTabViewHost_CreateInstance(struct IMultitaskingData *,struct IMultitaskingViewManagerInternal *,struct IApplicationViewSwitcher *,struct IImmersiveAppCrusher *,struct IMultitaskingViewVisibilityServiceInternal *,struct IMultitaskingViewGestureState *,struct IApplicationViewCollection *,struct Windows::Internal::ComposableShell::Tabs::ITabController *,struct ITabViewManager *,struct _GUID const &,void * *))"},
                &CAltTabViewHost_CreateInstance_Original,
                CAltTabViewHost_CreateInstance_Hook,
            },
            {
                {LR"(protected: virtual void __cdecl CAltTabViewHost::v_DismissView(void))"},
                &CAltTabViewHost_DismissView_Original,
                CAltTabViewHost_DismissView_Hook,
                true,
            },
        };

        if (!HookSymbols(twinuiPcshellModule, twinuiPcshellSymbolHooks,
                         ARRAYSIZE(twinuiPcshellSymbolHooks))) {
            Wh_Log(L"HookSymbols failed");

            return FALSE;
        }

        g_dismissViewHooked = CAltTabViewHost_DismissView_Original != nullptr;

    } else {
        Wh_Log(L"Unsupported Windows version");

        return FALSE;
    }

    return TRUE;
}

void Wh_ModSettingsChanged() {
    Wh_Log(L">");
    LoadSettings();
}
