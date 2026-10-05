#!/bin/bash
# Send console commands to the running dev game through data/devcmd.txt
# (polled by lua/autorun/client/devcmd.lua). "lua <code>" runs client Lua.
# Usage: con.sh "cmd1; cmd2" ["cmd3" ...]
D="$(cd "$(dirname "$0")" && pwd)/game/garrysmod/data"
printf '%s\n' "$@" >> "$D/devcmd.txt"
for i in $(seq 1 20); do [ -e "$D/devcmd.txt" ] || exit 0; sleep 0.25; done
echo "con.sh: not consumed (not in game?)" >&2; exit 1
