#pragma once

#include <windows.h>
#include <cstdint>
#include <vector>

enum class IconState { On, Off, Busy, Error };

// Renders the icon into a top-down BGRA buffer (straight alpha), size*size pixels.
void RenderStateIcon(IconState state, int size, std::vector<uint32_t>& pixels);

// Creates an HICON for the given state. Caller owns the handle (DestroyIcon).
HICON MakeStateIcon(IconState state, int size);
