#pragma once

#include <windows.h>
#include <optional>
#include <string>
#include <vector>

// Persisted under HKCU\Software\DgpuTraySwitch.
namespace settings {

// The state the user last chose; empty until the first toggle.
std::optional<bool> DesiredEnabled();
void SetDesiredEnabled(bool enabled);

// Optional override of the auto-detected device (value "DeviceInstanceId").
std::wstring DeviceInstanceId();

// Services to bounce (value "Services", ';'-separated); defaults to the NVIDIA ones.
std::vector<std::wstring> Services();

// Delay between enabling the GPU and restarting services (value "NvDelayMs").
DWORD NvDelayMs();

} // namespace settings
