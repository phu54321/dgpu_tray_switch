#pragma once

#include "gpu_device.h"

#include <windows.h>
#include <string>

// Posted to the notify window whenever the snapshot changes.
constexpr UINT WM_APP_CONTROLLER_STATE = WM_APP + 1;
// Posted with a heap-allocated std::wstring* in lParam (receiver deletes it);
// wParam is nonzero for errors.
constexpr UINT WM_APP_CONTROLLER_NOTICE = WM_APP + 2;

enum class ControllerOp { None, Enable, Disable, RestartServices };

struct ControllerSnapshot {
    GpuState state = GpuState::NotFound;
    ULONG problemCode = 0;
    std::wstring deviceName;
    bool busy = false;       // an operation is running or queued
    ControllerOp op = ControllerOp::None;  // the operation running right now
    bool requested = false;  // state the user asked for last
};

// Owns the worker thread that applies dGPU state changes. Requests never
// block: they only update the requested state, and the worker converges the
// device to whatever was requested last once the running operation ends.
class Controller {
public:
    Controller();
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    // bootMode runs the boot restore first: enable the dGPU, then return it
    // to the last chosen state, restarting services only if anything toggled.
    bool Start(HWND notifyWindow, bool bootMode);
    // Joins the worker after the operation in flight and drops queued ones.
    void Shutdown();

    void Request(bool enable);
    void RequestServiceRestart();

    // Power transitions (called on the window thread from WM_POWERBROADCAST).
    // Suspend is only recorded; on the following resume the boot restore runs
    // (enable if disabled, then back to the user's choice). Resume is used
    // rather than display-on, since the screen may stay off (clamshell).
    void OnSuspend();
    void OnResume();

    ControllerSnapshot Snapshot();

private:
    struct Result {
        bool ok = false;
        bool needReboot = false;
        std::wstring error;
        std::wstring warning;
    };

    static DWORD WINAPI ThreadProc(void* self);
    DWORD Run();
    void RunBoot();
    void Drain();
    Result ApplyOnce(bool enable);
    bool SleepUnlessQuitting(DWORD ms);
    void PostState();
    void PostNotice(bool isError, const std::wstring& text);

    SRWLOCK lock_ = SRWLOCK_INIT;
    HANDLE wake_ = nullptr;   // auto-reset: new work queued
    HANDLE quit_ = nullptr;   // manual-reset: shutting down
    HANDLE thread_ = nullptr;
    HWND notify_ = nullptr;

    // Guarded by lock_.
    DgpuInfo gpu_;
    bool applied_ = false;
    bool requested_ = false;
    ControllerOp op_ = ControllerOp::None;
    bool restartPending_ = false;
    bool bootPending_ = false;
    bool resumeSettle_ = false;  // boot restore was triggered by a resume
    bool suspended_ = false;
    bool quitting_ = false;
};
