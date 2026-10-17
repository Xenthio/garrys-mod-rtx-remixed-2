#pragma once
#include <windows.h>

namespace proxy::remix {
// D3D can retry creation with presentation parameters we already redirected.
// Keep that private target out of the Vulkan window and input-hook ownership.
inline HWND gameWindowForDevice(HWND current, HWND hidden, HWND focus, HWND requested) {
  if (!focus && current)
    return current; // Reset keeps the existing game window.
  const auto candidate = requested && requested != hidden ? requested : focus;
  return candidate && candidate != hidden ? candidate : current;
}
} // namespace proxy::remix
