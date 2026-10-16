#pragma once
#include <windows.h>

// Start-up and crash diagnostics: `shaderapi_proxy_diag.log` beside the proxy,
// what a user sends when the game does not start. It holds the system, the
// stages of start-up as they are reached (stage()), and for each fatal
// exception its code, address, registers, a stack trace as module+offset and
// the loaded modules; `shaderapi_proxy_crash.dmp` is a minidump of the first.
namespace proxy::diag {

// DllMain: opens the log and installs the exception handlers.
void install(HMODULE self);
// The unhandled-exception filter again: the engine installs its own after ours.
void reinstall();
// Opt-in startup diagnostics: log engine Error() messages before forwarding them.
void captureEngineErrors();
// One line, with the time since install and the calling thread.
void stage(const char *fmt, ...);

} // namespace proxy::diag
