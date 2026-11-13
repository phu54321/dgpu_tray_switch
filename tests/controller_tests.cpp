// Controller regression tests use fake hardware/services; no GPU is touched.
#include <windows.h>
#include <string>
#include <cstdint>
#include <optional>
#include <vector>
#include <functional>
#include <iostream>
#include <stdexcept>
#define private public
#include "controller.h"
#undef private
#include "settings.h"
#include "nv_services.h"

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
    c.gpu_.instanceId = L"fake";
    c.applied_ = c.requested_ = enabled;
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
            const auto r = c.ApplyOnce(false);
            Check(!r.ok && !r.error.empty(), "disable timeout must fail");
            Check(starts == 1, "services must be restored after timeout");
        }
        {
            Controller c; Reset(c, false);
            waitOk = false;
            const auto r = c.ApplyOnce(true);
            Check(!r.ok && restarts == 0, "enable timeout must not restart services");
            c.requested_ = true;
            c.RunBoot();
            Check(!c.applied_ && !c.requested_, "boot timeout must not record enabled or retry");
        }
        {
            Controller c; Reset(c, false);
            reboot = true;
            const auto r = c.ApplyOnce(true);
            Check(r.ok && r.needReboot && waits == 0 && restarts == 0, "reboot must be reported without waiting for driver");
        }
        {
            Controller c; Reset(c, true);
            c.Request(false);
            transitionOk = false;
            duringSet = [&] { c.Request(false); };
            c.Drain();
            Check(!c.requested_, "failed operation must preserve a newer request for the same target");
            transitionOk = true;
            c.Drain();
            Check(state == GpuState::Disabled && sets == 2, "newer request must still execute");
        }
        for (const auto external : {GpuState::Disabled, GpuState::Problem, GpuState::NotFound}) {
            Controller c; Reset(c, true);
            state = external;
            c.Request(true);
            c.Drain();
            Check(sets == 1 && state == GpuState::Enabled, "enable must reconcile external changes and problems");
        }
        {
            Controller c; Reset(c, true);
            c.Request(false);
            transitionOk = false;
            c.Drain();
            c.Drain();
            Check(sets == 1 && c.requested_, "failed request must not retry indefinitely");
        }
        {
            Controller c; Reset(c, false);
            state = GpuState::Enabled;
            c.Request(false);
            c.Drain();
            Check(sets == 1 && state == GpuState::Disabled, "disable must reconcile external changes");
        }
        std::cout << "Controller regressions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
