#pragma once

#include "controller.h"
#include "icons.h"

#include <windows.h>
#include <string>

// Hidden window owning the notification-area icon and its menu.
class Tray {
public:
    Tray() = default;
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    bool Create(HINSTANCE instance, Controller* controller);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void AddIcon();
    void RemoveIcon();
    void UpdateIcon();
    void ShowMenu(POINT pt);
    void ShowNotice(bool isError, const std::wstring& text);
    HICON IconFor(IconState state);
    void ClearIcons();

    HWND hwnd_ = nullptr;
    Controller* controller_ = nullptr;
    UINT taskbarCreated_ = 0;
    HPOWERNOTIFY powerNotify_ = nullptr;
    bool added_ = false;
    int iconSize_ = 0;
    HICON icons_[4] = {};
};
