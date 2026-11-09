#!/bin/bash
# Launch the dev copy of Garry's Mod (x64) through Proton, windowed.
# Usage: run.sh [extra game args...]   (e.g. +map gm_construct)
set -e
DEV="$(cd "$(dirname "$0")" && pwd)"
PROTON="$HOME/.local/share/Steam/compatibilitytools.d/proton-cachyos-11.0-20260703-slr-x86_64_v3/proton"
export STEAM_COMPAT_DATA_PATH="$DEV/compatdata"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.local/share/Steam"
export PROTON_ENABLE_NVAPI=1
export SteamAppId=4000 SteamGameId=4000 OPENREMIX_PROXY_BACKEND=${OPENREMIX_PROXY_BACKEND:-pathtrace}
mkdir -p "$STEAM_COMPAT_DATA_PATH"
cd "$DEV/game"
exec "$PROTON" run ./gmod_win64.exe -game garrysmod -insecure -steam -novid -nod3d9ex -condebug -netconport 27998 -multirun \
  -windowed -noborder -w 1920 -h 1080 +sv_cheats 1 \
  +mat_hdr_level 0 +r_rootlod 0 +mat_reducefillrate 1 +r_waterforceexpensive 0 +r_waterforcereflectentities 0 \
  +r_shadowrendertotexture 0 +r_flashlightdepthtexture 0 +mat_colorcorrection 0 +mat_antialias 1 +mat_aaquality 0 \
  +mat_trilinear 0 +mat_forceaniso 1 +mat_vsync 0 +mat_motion_blur_enabled 0 "$@"
