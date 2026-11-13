#include "tray.h"

#include "autostart.h"

#include <shellapi.h>
#include <windowsx.h>

namespace {

constexpr wchar_t kClassName[] = L"DgpuTraySwitchWindow";
constexpr wchar_t kAppTitle[] = L"dGPU Tray Switch";
constexpr UINT kIconId = 1;
constexpr UINT WM_APP_TRAY = WM_APP + 10;
constexpr UINT_PTR kRetryTimer = 1;

enum MenuId : UINT {
    ID_STATUS = 100,
    ID_ENABLE,
    ID_DISABLE,
    ID_RESTART_SERVICES,
    ID_AUTOSTART,
    ID_EXIT,
};

std::wstring StateText(const ControllerSnapshot& s) {
    if (s.busy) {
        std::wstring text;
        switch (s.op) {
        case ControllerOp::Enable:          text = L"enabling..."; break;
        case ControllerOp::Disable:         text = L"disabling..."; break;
        case ControllerOp::RestartServices: text = L"restarting services..."; break;
        case ControllerOp::None:            text = L"pending..."; break;
        }
        // Queued follow-up, e.g. the user flipped it back while disabling.
        bool opIsToggle = s.op == ControllerOp::Enable || s.op == ControllerOp::Disable;
        if (opIsToggle && (s.op == ControllerOp::Enable) != s.requested)
            text += s.requested ? L" then enable" : L" then disable";
        return text;
    }
    switch (s.state) {
    case GpuState::Enabled:  return L"On";
    case GpuState::Disabled: return L"Off (Eco)";
    case GpuState::Problem:  return L"error " + std::to_wstring(s.problemCode) + L": " + GpuProblemText(s.problemCode);
    case GpuState::NotFound: break;
    }
    return L"not found";
}

IconState IconStateFor(const ControllerSnapshot& s) {
    if (s.busy) return IconState::Busy;
    switch (s.state) {
    case GpuState::Enabled:  return IconState::On;
    case GpuState::Disabled: return IconState::Off;
    default:                 return IconState::Error;
    }
}

template <size_t N>
void CopyTruncated(wchar_t (&dst)[N], const std::wstring& src) {
    lstrcpynW(dst, src.c_str(), static_cast<int>(N));
}

} // namespace

Tray::~Tray() {
    if (hwnd_) DestroyWindow(hwnd_);
    ClearIcons();
}

bool Tray::Create(HINSTANCE instance, Controller* controller) {
    controller_ = controller;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&wc)) return false;

    // A regular (hidden) top-level window: message-only windows don't get
    // the TaskbarCreated broadcast.
    hwnd_ = CreateWindowExW(0, kClassName, kAppTitle, WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;

    // We run elevated; let Explorer (medium integrity) reach us.
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreated_, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, WM_APP_TRAY, MSGFLT_ALLOW, nullptr);

    AddIcon();
    return true;
}

LRESULT CALLBACK Tray::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* self = static_cast<Tray*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Tray::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreated_ && taskbarCreated_ != 0) {
        // Explorer restarted (or DPI changed): re-add with freshly sized icons.
        added_ = false;
        ClearIcons();
        AddIcon();
        return 0;
    }

    switch (msg) {
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            controller_->Toggle();
            return 0;
        case WM_CONTEXTMENU:
            ShowMenu({GET_X_LPARAM(wp), GET_Y_LPARAM(wp)});
            return 0;
        }
        return 0;

    case WM_APP_CONTROLLER_STATE:
        UpdateIcon();
        return 0;

    case WM_APP_CONTROLLER_NOTICE: {
        auto* text = reinterpret_cast<std::wstring*>(lp);
        ShowNotice(wp != 0, *text);
        delete text;
        return 0;
    }

    case WM_TIMER:
        if (wp == kRetryTimer) AddIcon();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ENABLE:           controller_->Request(true); break;
        case ID_DISABLE:          controller_->Request(false); break;
        case ID_RESTART_SERVICES: controller_->RequestServiceRestart(); break;
        case ID_AUTOSTART: {
            std::wstring error;
            if (!SetAutostart(!IsAutostartEnabled(), error)) ShowNotice(true, error);
            break;
        }
        case ID_EXIT:             DestroyWindow(hwnd_); break;
        }
        return 0;

    case WM_DESTROY:
        RemoveIcon();
        hwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void Tray::AddIcon() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = IconFor(IconState::Busy);
    CopyTruncated(nid.szTip, kAppTitle);

    // The taskbar may not exist yet right after logon; keep retrying.
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        SetTimer(hwnd_, kRetryTimer, 2000, nullptr);
        return;
    }
    KillTimer(hwnd_, kRetryTimer);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    added_ = true;
    UpdateIcon();
}

void Tray::RemoveIcon() {
    if (!added_) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

void Tray::UpdateIcon() {
    if (!added_) return;
    ControllerSnapshot s = controller_->Snapshot();

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = IconFor(IconStateFor(s));
    std::wstring tip = L"dGPU: " + StateText(s);
    if (!s.deviceName.empty()) tip += L"\n" + s.deviceName;
    CopyTruncated(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::ShowMenu(POINT pt) {
    ControllerSnapshot s = controller_->Snapshot();
    HMENU menu = CreatePopupMenu();

    std::wstring status = (s.deviceName.empty() ? std::wstring(L"dGPU") : s.deviceName) + L": " + StateText(s);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_STATUS, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Always enabled: requests made while busy are queued (latest wins).
    AppendMenuW(menu, MF_STRING, ID_ENABLE, L"&Enable dGPU");
    AppendMenuW(menu, MF_STRING, ID_DISABLE, L"&Disable dGPU (Eco)");
    CheckMenuRadioItem(menu, ID_ENABLE, ID_DISABLE, s.requested ? ID_ENABLE : ID_DISABLE, MF_BYCOMMAND);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // Toggles already restart services; this is only for recovering stuck ones.
    AppendMenuW(menu, MF_STRING, ID_RESTART_SERVICES, L"(Manual) &Restart NVIDIA services");
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : MF_UNCHECKED), ID_AUTOSTART,
                L"&Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"E&xit");

    // Required so the menu closes when clicking elsewhere.
    SetForegroundWindow(hwnd_);
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void Tray::ShowNotice(bool isError, const std::wstring& text) {
    if (!added_) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = (isError ? NIIF_ERROR : NIIF_INFO) | NIIF_RESPECT_QUIET_TIME;
    CopyTruncated(nid.szInfoTitle, kAppTitle);
    CopyTruncated(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

HICON Tray::IconFor(IconState state) {
    int size = GetSystemMetrics(SM_CXSMICON);
    if (size != iconSize_) {
        ClearIcons();
        iconSize_ = size;
    }
    HICON& icon = icons_[static_cast<int>(state)];
    if (!icon) icon = MakeStateIcon(state, size);
    return icon;
}

void Tray::ClearIcons() {
    for (HICON& icon : icons_) {
        if (icon) DestroyIcon(icon);
        icon = nullptr;
    }
}
