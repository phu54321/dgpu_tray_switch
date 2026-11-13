#include "gpu_device.h"

#include "win_error.h"

#include <initguid.h>
#include <devguid.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <vector>

namespace {

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

std::wstring DeviceDescription(HDEVINFO set, SP_DEVINFO_DATA& dev) {
    for (DWORD prop : {SPDRP_FRIENDLYNAME, SPDRP_DEVICEDESC}) {
        wchar_t buf[256];
        if (SetupDiGetDeviceRegistryPropertyW(set, &dev, prop, nullptr, reinterpret_cast<BYTE*>(buf), sizeof(buf), nullptr))
            return buf;
    }
    return L"";
}

} // namespace

bool FindDgpu(const std::wstring& overrideInstanceId, DgpuInfo& out) {
    // DIGCF_PRESENT skips phantom devnodes left behind by old hardware/images.
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    SP_DEVINFO_DATA dev = {};
    dev.cbSize = sizeof(dev);
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t id[MAX_DEVICE_ID_LEN];
        if (!SetupDiGetDeviceInstanceIdW(set, &dev, id, MAX_DEVICE_ID_LEN, nullptr)) continue;
        std::wstring instanceId = id;

        bool match = overrideInstanceId.empty()
                         ? StartsWithI(instanceId, L"PCI\\VEN_10DE")
                         : _wcsicmp(instanceId.c_str(), overrideInstanceId.c_str()) == 0;
        if (match) {
            out.instanceId = instanceId;
            out.name = DeviceDescription(set, dev);
            if (out.name.empty()) out.name = instanceId;
            found = true;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

GpuState QueryGpuState(const DgpuInfo& gpu, ULONG* problemCode) {
    if (problemCode) *problemCode = 0;
    if (gpu.instanceId.empty()) return GpuState::NotFound;

    DEVINST inst;
    if (CM_Locate_DevNodeW(&inst, const_cast<DEVINSTID_W>(gpu.instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return GpuState::NotFound;

    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, inst, 0) != CR_SUCCESS) return GpuState::NotFound;
    if (!(status & DN_HAS_PROBLEM)) return GpuState::Enabled;

    if (problemCode) *problemCode = problem;
    return problem == CM_PROB_DISABLED ? GpuState::Disabled : GpuState::Problem;
}

bool SetGpuEnabled(const DgpuInfo& gpu, bool enable, bool& needReboot, std::wstring& error) {
    needReboot = false;
    HDEVINFO set = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        error = Win32ErrorText(GetLastError());
        return false;
    }

    bool ok = false;
    SP_DEVINFO_DATA dev = {};
    dev.cbSize = sizeof(dev);
    if (!SetupDiOpenDeviceInfoW(set, gpu.instanceId.c_str(), nullptr, 0, &dev)) {
        error = L"Cannot open device: " + Win32ErrorText(GetLastError());
    } else {
        SP_PROPCHANGE_PARAMS pc = {};
        pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
        pc.StateChange = enable ? DICS_ENABLE : DICS_DISABLE;
        pc.Scope = DICS_FLAG_GLOBAL;
        pc.HwProfile = 0;

        if (!SetupDiSetClassInstallParamsW(set, &dev, &pc.ClassInstallHeader, sizeof(pc))) {
            error = L"Cannot set install params: " + Win32ErrorText(GetLastError());
        } else if (!SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, &dev)) {
            error = (enable ? L"Enable failed: " : L"Disable failed: ") + Win32ErrorText(GetLastError());
        } else {
            ok = true;
            SP_DEVINSTALL_PARAMS_W ip = {};
            ip.cbSize = sizeof(ip);
            if (SetupDiGetDeviceInstallParamsW(set, &dev, &ip))
                needReboot = (ip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

bool WaitForGpuState(const DgpuInfo& gpu, bool enabled, DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        GpuState s = QueryGpuState(gpu);
        if (enabled ? (s == GpuState::Enabled) : (s == GpuState::Disabled)) return true;
        if (GetTickCount() - start >= timeoutMs) return false;
        Sleep(250);
    }
}

std::wstring GpuProblemText(ULONG problem) {
    // Mirrors G-Helper's DeviceHelper.ProblemText.
    switch (problem) {
    case 1:  return L"Device is not configured";
    case 10: return L"Device cannot start";
    case 12: return L"Not enough free resources";
    case 14: return L"Restart required";
    case 18: return L"Drivers need to be reinstalled";
    case 21: return L"Device is being removed";
    case 22: return L"Device is disabled";
    case 28: return L"Drivers are not installed";
    case 31:
    case 39: return L"Driver failed to load";
    case 32: return L"Driver service is disabled";
    case 37: return L"Driver initialization failed";
    case 38: return L"Previous driver still loaded, restart required";
    case 43: return L"Device stopped because it reported problems";
    case 44: return L"Device stopped by an application or service";
    case 48: return L"Driver is blocked from loading";
    case 52: return L"Driver signature cannot be verified";
    case 54: return L"Device is being reset";
    default: return L"Unknown problem";
    }
}
