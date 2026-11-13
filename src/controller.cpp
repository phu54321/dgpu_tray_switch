#include "controller.h"

#include "nv_services.h"
#include "settings.h"

namespace {

constexpr DWORD kDeviceSettleMs = 10000;
constexpr DWORD kShutdownWaitMs = 60000;
// Let the device stacks finish powering up after resume before touching them.
constexpr DWORD kResumeSettleMs = 3000;

class ExclusiveLock {
public:
    explicit ExclusiveLock(SRWLOCK& l) : l_(l) { AcquireSRWLockExclusive(&l_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&l_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
private:
    SRWLOCK& l_;
};

} // namespace

Controller::Controller() {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    quit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

Controller::~Controller() {
    Shutdown();
    if (wake_) CloseHandle(wake_);
    if (quit_) CloseHandle(quit_);
}

bool Controller::Start(HWND notifyWindow, bool bootMode) {
    notify_ = notifyWindow;
    {
        ExclusiveLock l(lock_);
        if (FindDgpu(settings::DeviceInstanceId(), gpu_)) {
            applied_ = QueryGpuState(gpu_) != GpuState::Disabled;
            // Converge to the last state the user chose, if any.
            requested_ = settings::DesiredEnabled().value_or(applied_);
            bootPending_ = bootMode;
        }
    }
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!thread_) return false;
    SetEvent(wake_);
    return true;
}

void Controller::Shutdown() {
    if (!thread_) return;
    {
        ExclusiveLock l(lock_);
        quitting_ = true;
    }
    SetEvent(quit_);
    SetEvent(wake_);
    WaitForSingleObject(thread_, kShutdownWaitMs);
    CloseHandle(thread_);
    thread_ = nullptr;
}

void Controller::Request(bool enable) {
    {
        ExclusiveLock l(lock_);
        requested_ = enable;
    }
    settings::SetDesiredEnabled(enable);
    PostState();
    SetEvent(wake_);
}

void Controller::RequestServiceRestart() {
    {
        ExclusiveLock l(lock_);
        restartPending_ = true;
    }
    PostState();
    SetEvent(wake_);
}

void Controller::OnSuspend() {
    // Only remember the suspend. The device is not touched here: Windows
    // gives ~2 s and doesn't wait, too short to bring the driver up safely.
    ExclusiveLock l(lock_);
    suspended_ = true;
}

void Controller::OnResume() {
    {
        ExclusiveLock l(lock_);
        // Only once per suspend: Windows can send several resume events.
        if (!suspended_) return;
        suspended_ = false;
        bootPending_ = true;
        resumeSettle_ = true;
    }
    SetEvent(wake_);
}

ControllerSnapshot Controller::Snapshot() {
    ControllerSnapshot s;
    DgpuInfo gpu;
    {
        ExclusiveLock l(lock_);
        gpu = gpu_;
        s.busy = op_ != ControllerOp::None || restartPending_ || (!gpu_.instanceId.empty() && requested_ != applied_);
        s.op = op_;
        s.requested = requested_;
    }
    s.deviceName = gpu.name;
    s.state = QueryGpuState(gpu, &s.problemCode);
    return s;
}

DWORD WINAPI Controller::ThreadProc(void* self) {
    return static_cast<Controller*>(self)->Run();
}

DWORD Controller::Run() {
    for (;;) {
        WaitForSingleObject(wake_, INFINITE);
        {
            ExclusiveLock l(lock_);
            if (quitting_) return 0;
        }
        Drain();
        PostState();
    }
}

void Controller::RunBoot() {
    bool settle;
    {
        ExclusiveLock l(lock_);
        settle = resumeSettle_;
        resumeSettle_ = false;
    }
    if (settle && !SleepUnlessQuitting(kResumeSettleMs)) return;

    DgpuInfo gpu;
    bool desired;
    {
        ExclusiveLock l(lock_);
        bootPending_ = false;
        if (quitting_ || gpu_.instanceId.empty()) return;
        gpu = gpu_;
        desired = requested_;
        op_ = ControllerOp::Enable;
    }
    PostState();

    // Bring the dGPU up first: some firmware/drivers misbehave if the GPU
    // stays disabled across boot. Then fall back to the user's last choice.
    bool changed = false, needReboot = false, ok = true;
    std::wstring error, svcError;
    if (QueryGpuState(gpu) == GpuState::Disabled) {
        ok = SetGpuEnabled(gpu, true, needReboot, error);
        if (ok) {
            changed = true;
            WaitForGpuState(gpu, true, kDeviceSettleMs);
        }
    }

    if (ok && !desired) {
        {
            ExclusiveLock l(lock_);
            op_ = ControllerOp::Disable;
        }
        PostState();
        // Stops services, disables the device and starts them again,
        // which doubles as the service restart.
        Result r = ApplyOnce(false);
        ok = r.ok;
        error = r.error;
        svcError = r.warning;
        needReboot = needReboot || r.needReboot;
    } else if (ok && changed) {
        if (SleepUnlessQuitting(settings::NvDelayMs())) {
            std::wstring e;
            if (!RestartServices(settings::Services(), e)) svcError = L"Service restart issue: " + e;
        }
    }
    // Nothing toggled: the services were never disturbed, so skip the restart.

    {
        ExclusiveLock l(lock_);
        op_ = ControllerOp::None;
        applied_ = ok ? desired : QueryGpuState(gpu_) != GpuState::Disabled;
    }
    if (!ok) PostNotice(true, L"Boot restore failed: " + error);
    else if (!svcError.empty()) PostNotice(true, svcError);
    else if (needReboot) PostNotice(false, L"Restart Windows to finish restoring the dGPU state.");
}

void Controller::Drain() {
    bool boot;
    {
        ExclusiveLock l(lock_);
        boot = bootPending_;
    }
    if (boot) RunBoot();

    bool changed = false, needReboot = false;
    std::wstring warning;

    for (;;) {
        bool target = false, restart = false;
        {
            ExclusiveLock l(lock_);
            if (quitting_) return;
            if (restartPending_) {
                restartPending_ = false;
                restart = true;
            } else if (requested_ == applied_) {
                break;
            } else if (gpu_.instanceId.empty() && !FindDgpu(settings::DeviceInstanceId(), gpu_)) {
                requested_ = applied_;
                break;  // reported below
            } else {
                target = requested_;
            }
            op_ = restart ? ControllerOp::RestartServices : target ? ControllerOp::Enable : ControllerOp::Disable;
        }
        PostState();

        if (restart) {
            std::wstring error;
            bool ok = RestartServices(settings::Services(), error);
            PostNotice(!ok, ok ? L"NVIDIA services restarted." : L"Service restart failed: " + error);
            ExclusiveLock l(lock_);
            op_ = ControllerOp::None;
            continue;
        }

        Result r = ApplyOnce(target);
        {
            ExclusiveLock l(lock_);
            op_ = ControllerOp::None;
            if (r.ok) {
                applied_ = target;
            } else {
                // Resync with reality and stop instead of retrying in a loop.
                applied_ = QueryGpuState(gpu_) != GpuState::Disabled;
                requested_ = applied_;
            }
        }
        if (!r.ok) {
            PostNotice(true, r.error);
            return;
        }
        changed = true;
        needReboot = needReboot || r.needReboot;
        if (!r.warning.empty()) warning = r.warning;
    }

    bool found;
    bool final;
    {
        ExclusiveLock l(lock_);
        found = !gpu_.instanceId.empty();
        final = applied_;
    }
    if (!found) {
        PostNotice(true, L"No NVIDIA dGPU found. Set DeviceInstanceId in HKCU\\Software\\DgpuTraySwitch to choose one.");
        return;
    }
    if (!changed) return;

    std::wstring text = final ? L"dGPU enabled." : L"dGPU disabled (Eco).";
    if (needReboot) text += L" Restart Windows to finish the change.";
    if (!warning.empty()) text += L"\n" + warning;
    PostNotice(!warning.empty(), text);
}

Controller::Result Controller::ApplyOnce(bool enable) {
    Result r;
    DgpuInfo gpu;
    {
        ExclusiveLock l(lock_);
        gpu = gpu_;
    }
    const auto services = settings::Services();
    std::wstring svcError;

    if (!enable) {
        // Stop the services first so they release the GPU (as G-Helper does),
        // then disable the device and bring the services back.
        std::vector<std::wstring> dependents;
        StopServices(services, dependents, svcError);
        r.ok = SetGpuEnabled(gpu, false, r.needReboot, r.error);
        if (r.ok && !r.needReboot) WaitForGpuState(gpu, false, kDeviceSettleMs);
        std::wstring startError;
        StartServices(services, dependents, startError);
        if (svcError.empty()) svcError = startError;
    } else {
        r.ok = SetGpuEnabled(gpu, true, r.needReboot, r.error);
        if (r.ok) {
            WaitForGpuState(gpu, true, kDeviceSettleMs);
            // Give the driver time to initialize before restarting services.
            if (!SleepUnlessQuitting(settings::NvDelayMs())) return r;
            RestartServices(services, svcError);
        }
    }
    if (!svcError.empty()) r.warning = L"Service restart issue: " + svcError;
    return r;
}

bool Controller::SleepUnlessQuitting(DWORD ms) {
    return WaitForSingleObject(quit_, ms) == WAIT_TIMEOUT;
}

void Controller::PostState() {
    if (notify_) PostMessageW(notify_, WM_APP_CONTROLLER_STATE, 0, 0);
}

void Controller::PostNotice(bool isError, const std::wstring& text) {
    if (!notify_) return;
    auto* copy = new std::wstring(text);
    if (!PostMessageW(notify_, WM_APP_CONTROLLER_NOTICE, isError ? 1 : 0, reinterpret_cast<LPARAM>(copy)))
        delete copy;
}
