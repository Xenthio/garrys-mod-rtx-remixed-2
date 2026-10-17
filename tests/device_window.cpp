#include "device_window.h"
#include <cstdint>
#include <cstdio>

int main() {
  const auto game = reinterpret_cast<HWND>(uintptr_t(1));
  const auto hidden = reinterpret_cast<HWND>(uintptr_t(2));
  const auto other = reinterpret_cast<HWND>(uintptr_t(3));
  using proxy::remix::gameWindowForDevice;
  if (gameWindowForDevice(nullptr, nullptr, game, game) != game ||
      gameWindowForDevice(game, hidden, game, hidden) != game ||
      gameWindowForDevice(game, hidden, nullptr, hidden) != game ||
      gameWindowForDevice(game, hidden, hidden, hidden) != game ||
      gameWindowForDevice(nullptr, hidden, game, nullptr) != game ||
      gameWindowForDevice(game, hidden, other, other) != other)
    return 1;
  std::puts("Game window ownership survives redirected D3D creation retries and reset");
  return 0;
}
