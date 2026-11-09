#pragma once
#include "common/protocol.h"

namespace bridge {

// Replays one forwarded game-window input event (remixapi_bridge_InjectInputEXT)
// as an SDL event on the runtime's window, for its own UI.
void injectInput(const proto::ArgsInput &input);

// True once after events were injected: the host then pumps the runtime's
// event queue before the next Present.
bool takeInputPending();

} // namespace bridge
