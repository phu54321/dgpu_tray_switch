#pragma once

#include <string>

// Autostart is a Task Scheduler logon task with highest privileges, so the
// elevated app starts at logon without a UAC prompt. Requires COM on the
// calling thread.
bool IsAutostartEnabled();
bool SetAutostart(bool enable, std::wstring& error);
