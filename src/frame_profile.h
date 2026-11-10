#pragma once
#include <cstdint>
#include <remix/remix_c.h>
#include <windows.h>

namespace proxy::frame_profile {
// Wall times differenced at consecutive frame boundaries. Mesh build nests
// inside draws. BSP and Present nest inside submission. World lists can
// overlap those scopes, and patch switching nests inside world lists.
struct HostStages {
  bool valid = false;
  double draws = 0, meshBuild = 0, submission = 0, bsp = 0, present = 0;
  double worldLists = 0, worldPatches = 0;
};

struct WorldListTotals {
  uint32_t lists = 0, patches = 0; // cumulative microseconds
};

bool enabled();
void recordWorldLists(uint32_t lists, uint32_t patches);
WorldListTotals worldListTotals();
// Optional CSV diagnostics. Disabled sessions perform no API calls or file I/O.
// Samples describe completed GPU frames; IDs distinguish repeated readbacks.
void sample(double hostFrameMilliseconds, remixapi_ErrorCode presentStatus,
            HWND hostWindow, PFN_remixapi_GetWindowState getWindowState, const HostStages &stages);
}
