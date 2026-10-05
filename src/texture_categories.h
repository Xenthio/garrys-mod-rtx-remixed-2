#pragma once
#include <cstdint>
#include <string>

// dxvk-remix's per-texture instance categories (rtx.ignoreTextures,
// rtx.decalTextures, ...), read from the game's rtx.conf and user.conf and
// keyed by the dxvk texture hash (dxvk_hash.h).
namespace proxy::categories {

// Reads `<directory>\rtx.conf` then `<directory>\user.conf` (later lists replace
// earlier ones, as dxvk-remix layers its configs).
void load(const std::wstring &directory);

// Replaces one option's list at runtime ("0x1234, -0x5678, ABCD"); returns
// false if `option` is not a texture category list.
bool set(const char *option, const char *value);

// remixapi_InstanceCategoryBit flags for a texture hash (0: none).
uint32_t lookup(uint64_t textureHash);

// Number of hashes with categories, for statistics.
size_t size();

} // namespace proxy::categories
