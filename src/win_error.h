#pragma once

#include <windows.h>
#include <string>

// "Access is denied. (0x00000005)" style text for a Win32/SetupAPI error code.
std::wstring Win32ErrorText(DWORD code);
