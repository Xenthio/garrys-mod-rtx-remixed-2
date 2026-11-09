#include "host/input.h"

#include <windows.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <array>
#include <string>

namespace bridge {
namespace {

// The runtime's SDL3.dll, loaded by remix.dll into this process.
template <class Fn> Fn sdl(const char *name) {
  static HMODULE module = GetModuleHandleW(L"SDL3.dll");
  return module ? reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(module, name))) : nullptr;
}

SDL_Scancode scancode(uint32_t vk) {
  if (vk >= 'A' && vk <= 'Z')
    return SDL_Scancode(SDL_SCANCODE_A + (vk - 'A'));
  if (vk >= '1' && vk <= '9')
    return SDL_Scancode(SDL_SCANCODE_1 + (vk - '1'));
  if (vk >= VK_F1 && vk <= VK_F12)
    return SDL_Scancode(SDL_SCANCODE_F1 + (vk - VK_F1));
  switch (vk) {
  case '0': return SDL_SCANCODE_0;
  case VK_RETURN: return SDL_SCANCODE_RETURN;
  case VK_ESCAPE: return SDL_SCANCODE_ESCAPE;
  case VK_BACK: return SDL_SCANCODE_BACKSPACE;
  case VK_TAB: return SDL_SCANCODE_TAB;
  case VK_SPACE: return SDL_SCANCODE_SPACE;
  case VK_LEFT: return SDL_SCANCODE_LEFT;
  case VK_RIGHT: return SDL_SCANCODE_RIGHT;
  case VK_UP: return SDL_SCANCODE_UP;
  case VK_DOWN: return SDL_SCANCODE_DOWN;
  case VK_HOME: return SDL_SCANCODE_HOME;
  case VK_END: return SDL_SCANCODE_END;
  case VK_PRIOR: return SDL_SCANCODE_PAGEUP;
  case VK_NEXT: return SDL_SCANCODE_PAGEDOWN;
  case VK_INSERT: return SDL_SCANCODE_INSERT;
  case VK_DELETE: return SDL_SCANCODE_DELETE;
  case VK_SHIFT: case VK_LSHIFT: return SDL_SCANCODE_LSHIFT;
  case VK_RSHIFT: return SDL_SCANCODE_RSHIFT;
  case VK_CONTROL: case VK_LCONTROL: return SDL_SCANCODE_LCTRL;
  case VK_RCONTROL: return SDL_SCANCODE_RCTRL;
  case VK_MENU: case VK_LMENU: return SDL_SCANCODE_LALT;
  case VK_RMENU: return SDL_SCANCODE_RALT;
  case VK_OEM_MINUS: return SDL_SCANCODE_MINUS;
  case VK_OEM_PLUS: return SDL_SCANCODE_EQUALS;
  case VK_OEM_PERIOD: return SDL_SCANCODE_PERIOD;
  case VK_OEM_COMMA: return SDL_SCANCODE_COMMA;
  default: return SDL_SCANCODE_UNKNOWN;
  }
}

SDL_Keymod modifier(SDL_Scancode code) {
  switch (code) {
  case SDL_SCANCODE_LSHIFT: return SDL_KMOD_LSHIFT;
  case SDL_SCANCODE_RSHIFT: return SDL_KMOD_RSHIFT;
  case SDL_SCANCODE_LCTRL: return SDL_KMOD_LCTRL;
  case SDL_SCANCODE_RCTRL: return SDL_KMOD_RCTRL;
  case SDL_SCANCODE_LALT: return SDL_KMOD_LALT;
  case SDL_SCANCODE_RALT: return SDL_KMOD_RALT;
  default: return SDL_KMOD_NONE;
  }
}

std::string utf8(uint32_t c) {
  std::string out;
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xc0 | (c >> 6));
    out += char(0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    out += char(0xe0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3f));
    out += char(0x80 | (c & 0x3f));
  } else {
    out += char(0xf0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 0x3f));
    out += char(0x80 | ((c >> 6) & 0x3f));
    out += char(0x80 | (c & 0x3f));
  }
  return out;
}

float g_x = 0, g_y = 0;
SDL_Keymod g_mods = SDL_KMOD_NONE;
SDL_MouseButtonFlags g_buttons = 0;
bool g_pending = false;
// SDL text events point at their text until the event is processed.
std::array<std::string, 64> g_text;
size_t g_textIndex = 0;

} // namespace

void injectInput(const proto::ArgsInput &input) {
  using PushEvent = bool (*)(SDL_Event *);
  using GetWindows = SDL_Window **(*)(int *);
  using GetWindowID = SDL_WindowID (*)(SDL_Window *);
  using Free = void (*)(void *);
  using GetTicksNS = Uint64 (*)();
  using KeyFromScancode = SDL_Keycode (*)(SDL_Scancode, SDL_Keymod, bool);
  static auto push = sdl<PushEvent>("SDL_PushEvent");
  static auto windows = sdl<GetWindows>("SDL_GetWindows");
  static auto windowId = sdl<GetWindowID>("SDL_GetWindowID");
  static auto sdlFree = sdl<Free>("SDL_free");
  static auto ticks = sdl<GetTicksNS>("SDL_GetTicksNS");
  static auto keyFromScancode = sdl<KeyFromScancode>("SDL_GetKeyFromScancode");
  if (!push || !windows || !windowId || !sdlFree || !ticks || !keyFromScancode)
    return;
  int count = 0;
  SDL_Window **list = windows(&count);
  const SDL_WindowID id = list && count > 0 ? windowId(list[0]) : 0;
  sdlFree(list);
  if (!id)
    return;

  SDL_Event e = {};
  e.common.timestamp = ticks();
  switch (input.type) {
  case 0: // mouse move
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = id;
    e.motion.state = g_buttons;
    e.motion.x = float(input.x);
    e.motion.y = float(input.y);
    e.motion.xrel = e.motion.x - g_x;
    e.motion.yrel = e.motion.y - g_y;
    g_x = e.motion.x;
    g_y = e.motion.y;
    break;
  case 1: { // mouse button
    if (input.code < 1 || input.code > 3)
      return;
    const Uint8 button = input.code == 1 ? SDL_BUTTON_LEFT : input.code == 2 ? SDL_BUTTON_MIDDLE : SDL_BUTTON_RIGHT;
    e.type = input.down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = id;
    e.button.button = button;
    e.button.down = input.down != 0;
    e.button.clicks = 1;
    e.button.x = float(input.x);
    e.button.y = float(input.y);
    if (input.down)
      g_buttons |= SDL_BUTTON_MASK(button);
    else
      g_buttons &= ~SDL_BUTTON_MASK(button);
    break;
  }
  case 2: // wheel
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = id;
    e.wheel.y = input.wheel;
    e.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    e.wheel.mouse_x = g_x;
    e.wheel.mouse_y = g_y;
    break;
  case 3: { // key
    const SDL_Scancode code = scancode(input.code);
    if (code == SDL_SCANCODE_UNKNOWN)
      return;
    if (const SDL_Keymod mod = modifier(code))
      g_mods = input.down ? SDL_Keymod(g_mods | mod) : SDL_Keymod(g_mods & ~mod);
    e.type = input.down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = id;
    e.key.scancode = code;
    e.key.key = keyFromScancode(code, g_mods, false);
    e.key.mod = g_mods;
    e.key.down = input.down != 0;
    break;
  }
  case 4: { // text
    if (!input.code)
      return;
    auto &text = g_text[g_textIndex++ % g_text.size()];
    text = utf8(input.code);
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = id;
    e.text.text = text.c_str();
    break;
  }
  case 5: // focus
    e.type = input.down ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = id;
    if (!input.down) {
      g_mods = SDL_KMOD_NONE;
      g_buttons = 0;
    }
    break;
  default:
    return;
  }
  push(&e);
  g_pending = true;
}

bool takeInputPending() {
  const bool pending = g_pending;
  g_pending = false;
  return pending;
}

} // namespace bridge
