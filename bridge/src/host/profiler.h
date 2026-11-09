#pragma once
#include <string>

namespace bridge {

// Dev aid: while the host runs, creating `<dir>\openremix_profile.request`
// samples every host thread's stack for a few seconds (1 kHz, x64 unwind
// tables) and writes `<dir>\openremix_profile.txt`: one line per distinct
// stack, "count module+offset;module+offset;..." leaf first. Offsets are RVAs
// for llvm-symbolizer (address = image base + offset).
void startProfilerWatch(const std::wstring &directory);

} // namespace bridge
