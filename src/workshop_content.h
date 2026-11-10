#pragma once
#include <string>
#include <vector>

// Workshop content packages: Remix mods (and the Lua add-ons that go with
// them) that a subscribed Workshop item carries as a GMod native loader
// package, schema 1, content only (Gmod-Workshop-RTX-Remix's
// docs/PACKAGE_FORMAT.md; map PBR remasters ship this way). The item's .gma
// holds native/manifest.json and, under native/root/, the files to place:
// rtx-remix/mods/<mod>/... and garrysmod/addons/<add-on>/.... GMod refuses
// to mount such an archive and OpenRemix reads mods from a directory, so the
// proxy unpacks each file where it is read: the mods into the mods directory
// startRemix configures, the add-ons into garrysmod/addons.
//
// Only items an account on this computer subscribed to count (Steam's
// appworkshop_4000.acf names a subscriber), less the ones switched off in
// GMod's add-on list (garrysmod/cfg/addonnomount.txt). Every file is checked
// against the manifest (size and SHA-256) and the manifest against the
// archive, and a root's new files are all written and checked before any of
// them goes in place. What the proxy placed is recorded beside it
// (shaderapi_proxy_workshop.txt): a later start brings those files up to date
// with the item and takes them away once the item is unsubscribed. A folder
// with files the proxy did not place is left as it is. A placed file changed
// since (OpenRemix's Mods tab saves a mod's light scale into its mod.usda)
// stays until the package changes that file or goes; then it is moved to
// shaderapi_proxy_workshop_backup beside the record. A create-only file
// changed since stays the player's.
//
// Only a session with a runtime places or removes anything. Without one
// (OPENREMIX_PROXY=0, no runtime DLL, or a runtime that failed to start), the
// add-on folders of packages that require Remix are left out of GMod's
// searches of garrysmod/addons. OPENREMIX_PROXY_WORKSHOP=0 turns all of it
// off; with OPENREMIX_PROXY_MODS=0 the mods placed before stay as they are.
namespace proxy::workshop {

// The first CreateInterface, in the game (gmod_win64.exe beside garrysmod/).
// `runtime`: the proxy is on and its runtime is there; `modsDir`: where the
// runtime reads mods (empty: none). With a runtime it finds the packages and
// places their add-on folders before it returns, ahead of GMod mounting
// garrysmod/addons, and unpacks their mods on a thread.
void begin(bool runtime, const std::wstring &modsDir);
// startRemix, before the runtime starts (its Startup reads the mods
// directory): waits for that thread while it makes progress. A start with
// nothing new only compares sizes and times.
void finish();
// startRemix failed: the session has no runtime from now on.
void runtimeFailed();

// The same, synchronously, for given folders (offline tests).
struct Options {
  std::wstring gameRoot;  // the folder of gmod_win64.exe
  std::wstring modsDir;   // where the runtime reads mods (empty: none; the mods placed before stay)
  std::wstring statePath; // the record of what was placed
  std::wstring steamapps; // empty: the game's library, else Steam's libraryfolders.vdf
  std::wstring steamRoot; // empty: HKCU\Software\Valve\Steam SteamPath
};
void deploy(const Options &options);
// The add-on folders a session without a runtime hides, as recorded.
std::vector<std::string> rtxOnlyAddons(const Options &options);

} // namespace proxy::workshop
