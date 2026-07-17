// tools/phynned-ui/PhynnedTrayIcon.hpp
// PhynnedTrayIcon — Windows Shell_NotifyIcon system tray integration.
//
// Manages a notification area icon whose color reflects the agent state:
//   Green  (state 0) — Phynned active, optimisations applied.
//   Yellow (state 1) — Phynned active, Assist mode awaiting confirmation.
//   Orange (state 2) — Running without admin (degraded mode).
//   Red    (state 3) — Regression detected, policy reverted.
//   Gray   (state 4) — Agent not running / disconnected.
//
// Usage:
//   PhynnedTrayIcon tray;
//   tray.create(hWnd);              // creates the tray icon
//   tray.set_state(TrayState::Green);
//   tray.show_toast(L"Phynned", L"Cyberpunk 2077 detected — optimising");
//   tray.destroy();                 // call before WM_DESTROY
//
//   In WndProc: route WM_APP+1 to tray.on_tray_message(), and
//               route WM_COMMAND to tray.on_menu_command().
//
// Platform: Windows only. All methods are no-ops on non-Windows.
//
// §9.5, phynned-ui
#pragma once

#include <cstdint>
#include <cwchar>   // wcsncpy (en algunos MinGW solo expone ::wcsncpy, no std::wcsncpy)

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <cstring>
#endif

// ── TrayState ────────────────────────────────────────────────────────────────

enum class TrayState : uint8_t {
    Green  = 0,  ///< Phynned active, optimisations applied.
    Yellow = 1,  ///< Assist mode — awaiting user confirmation.
    Orange = 2,  ///< Running without admin (degraded).
    Red    = 3,  ///< Regression detected, reverted.
    Gray   = 4,  ///< Agent disconnected / not running.
};

// ── PhynnedTrayIcon ────────────────────────────────────────────────────────────

class PhynnedTrayIcon {
public:
    PhynnedTrayIcon() noexcept = default;
    ~PhynnedTrayIcon() noexcept { destroy(); }

    PhynnedTrayIcon(const PhynnedTrayIcon&)            = delete;
    PhynnedTrayIcon& operator=(const PhynnedTrayIcon&) = delete;

    [[nodiscard]] bool      is_active() const noexcept { return active_; }
    [[nodiscard]] TrayState state()     const noexcept { return state_; }

#ifdef _WIN32
    // ── Windows message IDs ───────────────────────────────────────────────
    static constexpr UINT kTrayMsg  = WM_APP + 1u;
    static constexpr UINT kIdIcon   = 1u;
    static constexpr UINT kMenuOpen = 100u;
    static constexpr UINT kMenuExit = 101u;

    // ── Lifecycle ─────────────────────────────────────────────────────────

    /// Register the tray icon for `hWnd` (must already be a valid HWND).
    void create(HWND hWnd) noexcept
    {
        hWnd_ = hWnd;
        if (!hWnd_) return;

        std::memset(&nid_, 0, sizeof(nid_));
        nid_.cbSize           = sizeof(nid_);
        nid_.hWnd             = hWnd_;
        nid_.uID              = kIdIcon;
        nid_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid_.uCallbackMessage = kTrayMsg;
        nid_.hIcon            = make_icon(TrayState::Gray);
        ::wcsncpy(nid_.szTip, L"Phynned — disconnected",
                     sizeof(nid_.szTip) / sizeof(wchar_t) - 1u);

        Shell_NotifyIconW(NIM_ADD, &nid_);
        active_ = true;
    }

    /// Remove the tray icon. Safe to call multiple times.
    void destroy() noexcept
    {
        if (!active_) return;
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        active_ = false;
        if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
    }

    /// Update icon color and tooltip text.
    void set_state(TrayState state,
                   const wchar_t* tooltip = nullptr) noexcept
    {
        if (!active_) return;

        if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
        icon_      = make_icon(state);
        nid_.hIcon  = icon_;
        nid_.uFlags = NIF_ICON | NIF_TIP;

        static const wchar_t* kTips[] = {
            L"Phynned — active (optimising)",
            L"Phynned — Assist mode (awaiting confirmation)",
            L"Phynned — degraded (run as admin for full features)",
            L"Phynned — regression detected, policy reverted",
            L"Phynned — agent not running",
        };
        const uint8_t idx = static_cast<uint8_t>(state);
        const wchar_t* tip = tooltip ? tooltip
                           : (idx < 5u ? kTips[idx] : kTips[4u]);
        ::wcsncpy(nid_.szTip, tip,
                     sizeof(nid_.szTip) / sizeof(wchar_t) - 1u);

        Shell_NotifyIconW(NIM_MODIFY, &nid_);
        state_ = state;
    }

    /// Show a balloon tooltip notification.
    void show_toast(const wchar_t* title, const wchar_t* body,
                    DWORD timeout_ms = 4000u) noexcept
    {
        if (!active_) return;

        NOTIFYICONDATAW nid = nid_;
        nid.uFlags         |= NIF_INFO;
        nid.dwInfoFlags     = NIIF_INFO;
        nid.uTimeout        = timeout_ms;
        ::wcsncpy(nid.szInfoTitle, title,
                     sizeof(nid.szInfoTitle) / sizeof(wchar_t) - 1u);
        ::wcsncpy(nid.szInfo, body,
                     sizeof(nid.szInfo) / sizeof(wchar_t) - 1u);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    /// Call from WndProc when uMsg == kTrayMsg.
    bool on_tray_message(WPARAM /*wParam*/, LPARAM lParam) noexcept
    {
        if (!active_) return false;
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            show_context_menu();
            return true;
        }
        if (lParam == WM_LBUTTONDBLCLK) {
            if (hWnd_) { ShowWindow(hWnd_, SW_RESTORE); SetForegroundWindow(hWnd_); }
            return true;
        }
        return false;
    }

    /// Handle WM_COMMAND from the tray context menu.
    void on_menu_command(UINT item_id) noexcept
    {
        if (item_id == kMenuOpen && hWnd_) {
            ShowWindow(hWnd_, SW_RESTORE);
            SetForegroundWindow(hWnd_);
        } else if (item_id == kMenuExit && hWnd_) {
            PostMessageW(hWnd_, WM_CLOSE, 0, 0);
        }
    }

private:
    HWND            hWnd_   {nullptr};
    NOTIFYICONDATAW nid_    {};
    HICON           icon_   {nullptr};
    bool            active_ {false};
    TrayState       state_  {TrayState::Gray};

    /// Create a 16×16 solid-color icon in software (no .ico file needed).
    static HICON make_icon(TrayState state) noexcept
    {
        static const COLORREF kColors[] = {
            RGB(40,  200,  60),  // Green
            RGB(220, 190,  40),  // Yellow
            RGB(220, 130,  30),  // Orange
            RGB(220,  50,  50),  // Red
            RGB(110, 110, 110),  // Gray
        };
        const uint8_t idx = static_cast<uint8_t>(state);
        const COLORREF c = (idx < 5u) ? kColors[idx] : kColors[4u];

        const int sz = 16;
        HDC hdc = GetDC(nullptr);

        HBITMAP hColor = CreateCompatibleBitmap(hdc, sz, sz);
        HBITMAP hMask  = CreateCompatibleBitmap(hdc, sz, sz);

        {
            HDC mdc  = CreateCompatibleDC(hdc);
            HBITMAP old = static_cast<HBITMAP>(SelectObject(mdc, hColor));
            HBRUSH br  = CreateSolidBrush(c);
            RECT rc    = {0, 0, sz, sz};
            FillRect(mdc, &rc, br);
            DeleteObject(br);
            SelectObject(mdc, old);
            DeleteDC(mdc);
        }
        {
            HDC mdc  = CreateCompatibleDC(hdc);
            HBITMAP old = static_cast<HBITMAP>(SelectObject(mdc, hMask));
            RECT rc  = {0, 0, sz, sz};
            FillRect(mdc, &rc,
                static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SelectObject(mdc, old);
            DeleteDC(mdc);
        }

        ReleaseDC(nullptr, hdc);

        ICONINFO ii{};
        ii.fIcon    = TRUE;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;
        HICON icon  = CreateIconIndirect(&ii);

        DeleteObject(hColor);
        DeleteObject(hMask);
        return icon;
    }

    void show_context_menu() noexcept
    {
        if (!hWnd_) return;
        POINT pt{};
        GetCursorPos(&pt);
        HMENU hMenu = CreatePopupMenu();
        if (!hMenu) return;
        AppendMenuW(hMenu, MF_STRING,    kMenuOpen, L"Open Phynned");
        AppendMenuW(hMenu, MF_SEPARATOR, 0,         nullptr);
        AppendMenuW(hMenu, MF_STRING,    kMenuExit, L"Exit");
        SetForegroundWindow(hWnd_);
        TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                       pt.x, pt.y, 0, hWnd_, nullptr);
        PostMessageW(hWnd_, WM_NULL, 0, 0);
        DestroyMenu(hMenu);
    }

#else // ── Non-Windows no-ops ─────────────────────────────────────────────────

    bool  active_ {false};
    TrayState state_ {TrayState::Gray};

    void create(void* /*hWnd*/)                                    noexcept {}
    void destroy()                                                 noexcept {}
    void set_state(TrayState, const void* /*tip*/ = nullptr)       noexcept {}
    void show_toast(const void*, const void*, unsigned = 0u)       noexcept {}
    bool on_tray_message(unsigned long long, long long)            noexcept { return false; }
    void on_menu_command(unsigned)                                 noexcept {}

#endif
};
// Made with my soul - Swately <3
