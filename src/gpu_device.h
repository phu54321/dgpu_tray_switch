#pragma once

#include <windows.h>
#include <string>

enum class GpuState {
    NotFound,  // device not present (or never detected)
    Enabled,   // devnode has no problem
    Disabled,  // CM_PROB_DISABLED (code 22)
    Problem,   // any other problem code, e.g. 43
};

struct DgpuInfo {
    std::wstring instanceId;  // e.g. PCI\VEN_10DE&DEV_2D19&...
    std::wstring name;        // device description
};

// Finds the dGPU among present display adapters. If overrideInstanceId is not
// empty, that exact device is used; otherwise the first NVIDIA adapter.
bool FindDgpu(const std::wstring& overrideInstanceId, DgpuInfo& out);

GpuState QueryGpuState(const DgpuInfo& gpu, ULONG* problemCode = nullptr);

// Enables or disables the devnode, the same way Device Manager does.
// needReboot is set when Windows reports the change needs a restart.
bool SetGpuEnabled(const DgpuInfo& gpu, bool enable, bool& needReboot, std::wstring& error);

// Polls until the device reaches the wanted enabled/disabled state.
bool WaitForGpuState(const DgpuInfo& gpu, bool enabled, DWORD timeoutMs);

std::wstring GpuProblemText(ULONG problemCode);
