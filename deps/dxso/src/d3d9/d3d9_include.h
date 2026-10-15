#pragma once
// Standalone dxso (the GMod OpenRemix proxy): the D3D9 types the compiler
// reads, in place of DXVK's d3d9_include.h.
#include <stdint.h>
#include <d3d9.h>

#include "../util/com/com_guid.h"
#include "../util/com/com_object.h"
#include "../util/com/com_pointer.h"
#include "../util/log/log.h"
#include "../util/log/log_debug.h"
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"
#include "../util/util_enum.h"
#include "../util/util_error.h"
#include "../util/util_flags.h"
#include "../util/util_likely.h"
#include "../util/util_math.h"
#include "../util/util_string.h"
