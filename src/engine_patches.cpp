#include "engine_patches.h"

namespace proxy::engine_patches {

bool install() { return false; }
void beginWorldLists() {}
void endWorldLists() {}
void dumpStatistics(FILE *) {}

} // namespace proxy::engine_patches
