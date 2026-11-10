> This project uses AI-generated code

<p align="left">
  <img src="https://github.com/user-attachments/assets/4dc256a6-8795-4c1b-885c-addf341d66eb" width="150" />
</p>


This is currently a proxy `shaderapidx9.dll` for a few Source Engine games that wraps the
Source shader API (and later D3D9) to submit the game's rendering to
[OpenRemix](https://github.com/OpenRenderCollective/OpenRemix) through the Remix C API. 

This removes the need to force the engine to run in the fixed function pipeline, so all shader-based systems are supported and are translated when applicable. OpenRemix has its own rasterization pass for sprites and other effects that would not work well in path-tracing and can be integrated seamlessly.

RTX Remix mods from the old version of the project will be supported, and also any lua stools like the Remix API lights, etc for Garry's Mod.

Multiplayer works out of the box since everything lives in the wrapper.

### Games the wrapper works with
* Garry's Mod **(CI only builds for this one)**
* Team Fortress 2 (64-bit, `tf_win64.exe`)
* Entropy: Zero 2
* Left 4 Dead 2
* Half-Life 2 RTX
* Portal with RTX
* Portal 2

## Garry's Mod Installation Instructions
- Ensure you're on the `main` branch of Garry's Mod on Steam (not `x86-64`)
- Run the vanilla game at least once, especially if you switched branches.
- Download [RTXLauncher](https://github.com/Xenthio/RTXLauncher/releases) (Use the newer nightly if available).
- **Windows**
   - Run the launcher (**as an Administrator**)
- **Linux**
   - Ensure Steam Play is forced for Garry's Mod with either Proton Experimental or another recent Proton build.
   - Run [GModPatchTool](https://github.com/solsticegamestudios/GModPatchTool)
   - Make sure it's marked as executable `chmod +x ./RTXLauncher.Avalonia.Linux`, and run it.
- Select `Start Quick Install` on the main screen and follow the prompts when asked.
- Once it's finished, press `Launch Game` at the bottom of the launcher.

## Garry's Mod | Workshop RTX content (Map remasters)
RTX Remix mods could be packed as workshop addons, refer to [GMod native loader content package](https://github.com/SheepyLord/Gmod-Workshop-RTX-Remix/blob/main/docs/PACKAGE_FORMAT.md):
A few example PBR maps could be downloaded from: 
[RTX-Remix Collection](https://steamcommunity.com/workshop/filedetails/?id=3834474695). 

## Credits
* Jane for helping me make the logo.
* Xenthio for ToPBR, Foward+, and general guidance
