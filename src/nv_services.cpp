#include "nv_services.h"

#include "win_error.h"

#include <windows.h>

#include <algorithm>
#include <vector>

namespace {

// NVDisplay.Container can take well over 20 s to stop while it is still
// initializing a GPU that was just enabled (seen after resume).
constexpr DWORD kServiceTimeoutMs = 60000;

struct ScHandle {
    SC_HANDLE h = nullptr;
    explicit ScHandle(SC_HANDLE handle) : h(handle) {}
    ~ScHandle() { if (h) CloseServiceHandle(h); }
    ScHandle(ScHandle&& other) noexcept : h(other.h) { other.h = nullptr; }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

DWORD CurrentState(SC_HANDLE svc) {
    SERVICE_STATUS_PROCESS ss = {};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&ss), sizeof(ss), &needed)) return 0;
    return ss.dwCurrentState;
}

bool WaitForState(SC_HANDLE svc, DWORD wanted) {
    const DWORD start = GetTickCount();
    for (;;) {
        DWORD state = CurrentState(svc);
        if (state == wanted) return true;
        if (state == 0 || GetTickCount() - start >= kServiceTimeoutMs) return false;
        Sleep(250);
    }
}

bool IsDisabled(SC_HANDLE svc) {
    DWORD needed = 0;
    QueryServiceConfigW(svc, nullptr, 0, &needed);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return false;
    std::vector<BYTE> buf(needed);
    auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
    return QueryServiceConfigW(svc, cfg, needed, &needed) && cfg->dwStartType == SERVICE_DISABLED;
}

bool StopOne(SC_HANDLE scm, const std::wstring& name, std::vector<std::wstring>& stoppedDependents,
             std::wstring& error) {
    ScHandle svc(OpenServiceW(scm, name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS));
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        error = name + L": " + Win32ErrorText(GetLastError());
        return false;
    }
    if (CurrentState(svc.h) == SERVICE_STOPPED) return true;

    // Running dependents must be stopped first or the stop request fails.
    DWORD needed = 0, count = 0;
    if (!EnumDependentServicesW(svc.h, SERVICE_ACTIVE, nullptr, 0, &needed, &count) && GetLastError() == ERROR_MORE_DATA) {
        std::vector<BYTE> buf(needed);
        auto* deps = reinterpret_cast<ENUM_SERVICE_STATUSW*>(buf.data());
        if (EnumDependentServicesW(svc.h, SERVICE_ACTIVE, deps, needed, &needed, &count)) {
            for (DWORD i = 0; i < count; ++i) {
                std::wstring dep = deps[i].lpServiceName;
                if (!StopOne(scm, dep, stoppedDependents, error)) return false;
                stoppedDependents.push_back(dep);
            }
        }
    }

    SERVICE_STATUS ss = {};
    if (!ControlService(svc.h, SERVICE_CONTROL_STOP, &ss) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        error = name + L": stop failed: " + Win32ErrorText(GetLastError());
        return false;
    }
    if (!WaitForState(svc.h, SERVICE_STOPPED)) {
        error = name + L": timed out waiting for stop";
        return false;
    }
    return true;
}

bool StartOne(SC_HANDLE scm, const std::wstring& name, std::wstring& error) {
    ScHandle svc(OpenServiceW(scm, name.c_str(), SERVICE_START | SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG));
    if (!svc) {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        error = name + L": " + Win32ErrorText(GetLastError());
        return false;
    }
    if (IsDisabled(svc.h) || CurrentState(svc.h) == SERVICE_RUNNING) return true;

    if (!StartServiceW(svc.h, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        error = name + L": start failed: " + Win32ErrorText(GetLastError());
        return false;
    }
    if (!WaitForState(svc.h, SERVICE_RUNNING)) {
        error = name + L": timed out waiting for start";
        return false;
    }
    return true;
}

ScHandle OpenScm(std::wstring& error) {
    ScHandle scm(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) error = L"Service manager: " + Win32ErrorText(GetLastError());
    return scm;
}

} // namespace

std::vector<std::wstring> DefaultNvServices() {
    return {L"NVDisplay.ContainerLocalSystem", L"NvContainerLocalSystem"};
}

bool StopServices(const std::vector<std::wstring>& services, std::vector<std::wstring>& stoppedDependents,
                  std::wstring& error) {
    ScHandle scm = OpenScm(error);
    if (!scm) return false;
    bool ok = true;
    for (const auto& name : services) ok = StopOne(scm.h, name, stoppedDependents, error) && ok;
    return ok;
}

bool StartServices(const std::vector<std::wstring>& services, const std::vector<std::wstring>& dependents,
                   std::wstring& error) {
    ScHandle scm = OpenScm(error);
    if (!scm) return false;
    bool ok = true;
    for (const auto& name : services) ok = StartOne(scm.h, name, error) && ok;
    // Dependents were recorded innermost-first; start them in reverse.
    for (auto it = dependents.rbegin(); it != dependents.rend(); ++it) {
        if (std::find(services.begin(), services.end(), *it) == services.end())
            ok = StartOne(scm.h, *it, error) && ok;
    }
    return ok;
}

bool RestartServices(const std::vector<std::wstring>& services, std::wstring& error) {
    std::vector<std::wstring> dependents;
    bool stopped = StopServices(services, dependents, error);
    // Always try to bring services back, even if a stop failed.
    std::wstring startError;
    bool started = StartServices(services, dependents, startError);
    if (error.empty()) error = startError;
    return stopped && started;
}
