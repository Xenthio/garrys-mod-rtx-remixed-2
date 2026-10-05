#!/bin/bash
# Stop the dev game instance and its wineserver.
pkill -x gmod_win64.exe
WINEPREFIX="$(cd "$(dirname "$0")" && pwd)/compatdata/pfx" \
  "$HOME/.local/share/Steam/compatibilitytools.d/proton-cachyos-11.0-20260703-slr-x86_64_v3/files/bin/wineserver" -k 2>/dev/null
exit 0
