"""Prepare a disposable GMod host without changing the installed game.

Engine binaries and base content are copied, never linked. Configuration and
normal write paths belong to the stage. Installed addons are not copied.
This tool prepares files only. It does not start the game or install graphics hooks.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


# Deliberately exclude unrelated injectors, addons and developer tools from an
# existing, potentially modified installation. Missing required files fail early.
ENGINE_DLLS = """
bass bz2 datacache engine filesystem_stdio GameUI gmod_audio inputsystem
launcher materialsystem mdllib scenefilecache ServerBrowser shaderapiempty
soundemittersystem soundsystem steam_api64 stdshader_dbg stdshader_dx6
stdshader_dx7 stdshader_dx8 stdshader_dx9 studiorender tier0 unicode
vaudio_speex vgui2 vguimatsurface vphysics vstdlib zip zlib1
html_chromium html_stub chrome_elf libcef libEGL libGLESv2 libcurl
jsoncpp nanomsg openvr_api GameAnalytics
""".split()
CLIENT_DLLS = """
analytics client game_shader_generic_garrysmod lua_shared menusystem resources server
""".split()


def prepare(source, destination, proxy, runtime, sdl, *, launcher_only=False):
    source = source.resolve(strict=True)
    destination = destination.resolve()
    if destination == source or source in destination.parents:
        raise ValueError("Stage must be outside the installed game")
    if destination.exists() and not (launcher_only and all(
            entry.is_file() and entry.name.startswith("RTXLauncher.Avalonia.exe")
            for entry in destination.iterdir())):
        raise ValueError("Stage must be a new directory; existing files are never replaced")
    engine = source / "bin/win64"
    client = source / "garrysmod/bin/win64"
    copies = [(source / "gmod_win64.exe", Path("gmod_win64.exe"))]
    copies += [(engine / (name + ".dll"), Path("bin/win64") / (name + ".dll"))
               for name in ENGINE_DLLS]
    copies += [(client / (name + ".dll"), Path("garrysmod/bin/win64") / (name + ".dll"))
               for name in CLIENT_DLLS]
    copies += [(engine / "gmod.exe", Path("bin/win64/gmod.exe")),
               (engine / "shaderapidx9_orig.dll", Path("bin/win64/shaderapidx9_orig.dll")),
               (proxy.resolve(strict=True), Path("bin/win64/shaderapidx9.dll")),
               (runtime.resolve(strict=True), Path("bin/win64/openremix/remix.dll")),
               (sdl.resolve(strict=True), Path("bin/win64/openremix/SDL3_shared.dll"))]
    for name in ("icudtl.dat", "snapshot_blob.bin", "v8_context_snapshot.bin"):
        copies.append((engine / name, Path("bin/win64") / name))
    # Lua and Chromium dependencies are supplied by GMod alongside its launcher.
    for name in ("libcrypto-3-x64.dll", "libssl-3-x64.dll"):
        if (source / name).is_file():
            copies.append((source / name, Path(name)))
    # GMod resets some search paths during startup. Supply base packages in its
    # expected local layout rather than depending solely on gameinfo mounts.
    for directory in ("garrysmod", "sourceengine", "platform"):
        for package in sorted((source / directory).glob("*.vpk")):
            copies.append((package, Path(directory) / package.name))
    for name in ("gm_construct", "gm_flatgrass"):
        relative = Path("garrysmod/maps") / (name + ".bsp")
        copies.append((source / relative, relative))
    loose = ["garrysmod/" + directory for directory in
             ("lua", "gamemodes/base", "gamemodes/sandbox", "resource", "scripts", "html", "backgrounds", "particles")]
    loose += ["sourceengine/resource", "sourceengine/scripts", "platform/resource", "platform/vgui", "bin/chromium"]
    for directory in loose:
        content = source / directory
        if content.is_dir():
            for asset in sorted(content.rglob("*")):
                relative = asset.relative_to(source)
                # Native Lua modules are user extensions.
                if asset.is_file() and "bin" not in asset.relative_to(content).parts:
                    copies.append((asset, relative))
    for original, _ in copies:
        if not original.is_file():
            raise FileNotFoundError(original)

    destination.mkdir(parents=True, exist_ok=launcher_only)
    for original, relative in copies:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, target)

    root = destination.as_posix()
    game = destination / "garrysmod"
    (game / "cfg").mkdir(exist_ok=True)
    for name in ("config.cfg", "autoexec.cfg", "mount.cfg"):
        (game / "cfg" / name).write_text("// Private test configuration\n", encoding="utf-8")
    (game / "cfg/valve.rc").write_text("exec autoexec.cfg\nstuffcmds\n", encoding="ascii")
    # Account player preferences may refer to Workshop models excluded above.
    # Choose a stock test actor on the server without changing those preferences.
    player_fixture = game / "lua/autorun/server/openremix_stage_player.lua"
    player_fixture.parent.mkdir(parents=True, exist_ok=True)
    player_fixture.write_text('''hook.Add("PlayerSpawn", "OpenRemixStagePlayer", function(player)
  timer.Simple(0, function()
    if not IsValid(player) then return end
    player:SetModel("models/player/kleiner.mdl")
    player:SetupHands()
  end)
end)
''', encoding="utf-8")
    # Keep all search paths local, including engine resources. GMod may replace
    # this list; the copied layout also supports its built-in mounting rules.
    (game / "gameinfo.txt").write_text(f'''"GameInfo"
{{
  game "Garry's Mod"
  type multiplayer_only
  FileSystem
  {{
    SteamAppId 4000
    ToolsAppId 211
    SearchPaths
    {{
      mod+mod_write+default_write_path "|gameinfo_path|."
      game+game_write "|gameinfo_path|."
      gamebin "|gameinfo_path|bin"
      game "{root}/garrysmod/garrysmod.vpk"
      game "{root}/sourceengine/hl2_textures.vpk"
      game "{root}/sourceengine/hl2_sound_vo_english.vpk"
      game "{root}/sourceengine/hl2_sound_misc.vpk"
      game "{root}/sourceengine/hl2_misc.vpk"
      platform "{root}/platform/platform_misc.vpk"
      game "{root}/garrysmod"
      game "{root}/sourceengine"
      platform "{root}/platform"
    }}
  }}
}}
''', encoding="utf-8")
    (destination / "steam_appid.txt").write_text("4000\n", encoding="ascii")
    (destination / "runtime-config").mkdir()
    manifest = {"sourceRoot": str(source), "contentRoot": str(destination), "files": [
        {"source": str(original), "destination": str(relative)} for original, relative in copies]}
    manifest["rendererHashes"] = {str(relative): hashlib.sha256(original.read_bytes()).hexdigest()
                                  for original, relative in copies
                                  if original in (proxy.resolve(), runtime.resolve(), sdl.resolve())}
    (destination / "stage-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "destination", "proxy", "runtime", "sdl"):
        parser.add_argument("--" + name, required=True, type=Path)
    arguments = parser.parse_args()
    prepare(arguments.source, arguments.destination, arguments.proxy, arguments.runtime, arguments.sdl)
