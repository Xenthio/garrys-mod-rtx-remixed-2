#!/bin/bash
# Screenshot of the presented Remix frame of the running dev game.
# Usage: shot.sh [output.png] [console command to run first]
DEV="$(cd "$(dirname "$0")" && pwd)"; B="$DEV/game/bin/win64"; OUT="${1:-$DEV/shot_game.png}"
[ -n "$2" ] && { "$DEV/con.sh" "$2" >/dev/null; sleep 1.5; }
rm -f "$B/remix_screenshot.png"; : > "$B/remix_screenshot.request"
for i in $(seq 1 20); do sleep 0.5; [ -s "$B/remix_screenshot.png" ] && break; done
sleep 1; cp "$B/remix_screenshot.png" "$OUT"
