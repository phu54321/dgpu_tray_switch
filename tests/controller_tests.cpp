// Controller regression tests use fake hardware/services; no GPU is touched.
#include <windows.h>
#include <string>
#include <cstdint>
#include <optional>
#include <vector>
#include <functional>
#include <iostream>
#include <stdexcept>
#include "controller.h"
#include "settings.h"
#include "nv_services.h"

// Befriended by Controller; exposes the worker steps without changing access
// specifiers (which would alter MSVC name mangling and break linking).
struct ControllerTestAccess {
    static DgpuInfo& gpu(Controller& c) { return c.gpu_; }
    static bool& applied(Controller& c) { return c.applied_; }
    static bool& requested(Controller& c) { return c.requested_; }
    static auto ApplyOnce(Controller& c, bool enable) { return c.ApplyOnce(enable); }
    static void RunBoot(Controller& c) { c.RunBoot(); }
    static void Drain(Controller& c) { c.Drain(); }
};
using T = ControllerTestAccess;

namespace {
GpuState state;
bool transitionOk, waitOk, reboot;
int sets, waits, starts, restarts;
std::function<void()> duringSet;
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void Reset(Controller& c, bool enabled) {
    state = enabled ? GpuState::Enabled : GpuState::Disabled;
    transitionOk = waitOk = true;
    reboot = false;
    sets = waits = starts = restarts = 0;
    duringSet = {};
    T::gpu(c).instanceId = L"fake";
    T::applied(c) = T::requested(c) = enabled;
}
}
bool FindDgpu(const std::wstring&, DgpuInfo& gpu) { gpu.instanceId = L"fake"; return true; }
GpuState QueryGpuState(const DgpuInfo&, ULONG*) { return state; }
bool SetGpuEnabled(const DgpuInfo&, bool enable, bool& needReboot, std::wstring& error) {
    ++sets;
    needReboot = reboot;
    auto callback = std::move(duringSet);
    duringSet = {};
    if (callback) callback();
    if (!transitionOk) { error = L"fake failure"; return false; }
    if (waitOk && !reboot) state = enable ? GpuState::Enabled : GpuState::Disabled;
    return true;
}
bool WaitForGpuState(const DgpuInfo&, bool, DWORD) { ++waits; return waitOk; }
bool StopServices(const std::vector<std::wstring>&, std::vector<std::wstring>&, std::wstring&) { return true; }
bool StartServices(const std::vector<std::wstring>&, const std::vector<std::wstring>&, std::wstring&) { ++starts; return true; }
bool RestartServices(const std::vector<std::wstring>&, std::wstring&) { ++restarts; return true; }
namespace settings {
std::optional<bool> DesiredEnabled() { return std::nullopt; }
void SetDesiredEnabled(bool) {}
std::wstring DeviceInstanceId() { return L"fake"; }
std::vector<std::wstring> Services() { return {}; }
DWORD NvDelayMs() { return 0; }
}
int wmain() {
    try {
        {
            Controller c; Reset(c, true);
            waitOk = false;
            const auto r = T::ApplyOnce(c, false);
            Check(!r.ok && !r.error.empty(), "disable timeout must fail");
            Check(starts == 1, "services must be restored after timeout");
        }
        {
            Controller c; Reset(c, false);
            waitOk = false;
            const auto r = T::ApplyOnce(c, true);
            Check(!r.ok && restarts == 0, "enable timeout must not restart services");
            T::requested(c) = true;
            T::RunBoot(c);
            Check(!T::applied(c) && !T::requested(c), "boot timeout must not record enabled or retry");
        }
        {
            Controller c; Reset(c, false);
            reboot = true;
            const auto r = T::ApplyOnce(c, true);
            Check(r.ok && r.needReboot && waits == 0 && restarts == 0, "reboot must be reported without waiting for driver");
        }
        {
            Controller c; Reset(c, true);
            c.Request(false);
            transitionOk = false;
            duringSet = [&] { c.Request(false); };
            T::Drain(c);
            Check(!T::requested(c), "failed operation must preserve a newer request for the same target");
            transitionOk = true;
            T::Drain(c);
            Check(state == GpuState::Disabled && sets == 2, "newer request must still execute");
        }
        for (const auto external : {GpuState::Disabled, GpuState::Problem, GpuState::NotFound}) {
            Controller c; Reset(c, true);
            state = external;
            c.Request(true);
            T::Drain(c);
            Check(sets == 1 && state == GpuState::Enabled, "enable must reconcile external changes and problems");
        }
        {
            Controller c; Reset(c, true);
            c.Request(false);
            transitionOk = false;
            T::Drain(c);
            T::Drain(c);
            Check(sets == 1 && T::requested(c), "failed request must not retry indefinitely");
        }
        {
            Controller c; Reset(c, false);
            state = GpuState::Enabled;
            c.Request(false);
            T::Drain(c);
            Check(sets == 1 && state == GpuState::Disabled, "disable must reconcile external changes");
        }
        std::cout << "Controller regressions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
