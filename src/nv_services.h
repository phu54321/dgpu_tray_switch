#pragma once

#include <string>
#include <vector>

// NVIDIA services that hold the GPU open and must be bounced around a toggle.
std::vector<std::wstring> DefaultNvServices();

// Stops the given services (and their running dependents first). Returns the
// dependents that were stopped so StartServices can bring them back.
// Missing services are skipped.
bool StopServices(const std::vector<std::wstring>& services, std::vector<std::wstring>& stoppedDependents,
                  std::wstring& error);

// Starts the given services, then the previously stopped dependents.
// Missing or disabled services are skipped.
bool StartServices(const std::vector<std::wstring>& services, const std::vector<std::wstring>& dependents,
                   std::wstring& error);

bool RestartServices(const std::vector<std::wstring>& services, std::wstring& error);
