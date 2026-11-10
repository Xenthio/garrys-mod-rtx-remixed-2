#pragma once
#include <remix/remix_native_ext.h>
#include <algorithm>
#include <stdexcept>
#include <string>

namespace proxy::replay {
inline bool preparationReady(const remixapi_PreparationStatisticsEXT &state) {
  if (state.structSize != sizeof(state))
    throw std::runtime_error("Runtime returned an incompatible preparation statistics layout");
  if (state.failed || state.pipelinesFailed) {
    const std::string reason(state.failureReason,
        std::find(state.failureReason, state.failureReason + sizeof(state.failureReason), '\0'));
    throw std::runtime_error("Renderer preparation failed" +
        (reason.empty() ? std::string{} : ": " + reason) + "; refusing an incomplete capture");
  }
  return !state.pending && !state.loading && state.pipelinesCompleted == state.pipelinesRequested;
}
}
