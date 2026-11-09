#!/bin/bash
# ab.sh <label> [VAR=value...]: launches the dev game with those environment
# variables (and REMIX_ACCEL_PROFILE), waits for gm_construct, runs
# perf/kinds.sh <label> and closes the game again. For A/B runs back to back.
DEV="$(cd "$(dirname "$0")/.." && pwd)"
LABEL="${1:?label}"; shift
cd "$DEV" || exit 1
if pgrep -x gmod_win64.exe >/dev/null; then echo "ab.sh: a game is running" >&2; exit 1; fi
rm -f /home/cr/proj/gmod-dev/game/bin/win64/shaderapi_proxy_diag.log
(env "$@" REMIX_ACCEL_PROFILE=openremix/accel.csv nohup ./run.sh +map gm_construct > run.log 2>&1 &)
sleep 10
timeout 200 bash -c "until grep -aq 'world loaded 1' game/bin/win64/shaderapi_proxy_diag.log 2>/dev/null; do sleep 5; done" ||
  { echo "ab.sh: map not loaded" >&2; ./stop.sh; exit 1; }
sleep 20
./perf/kinds.sh "$LABEL" > /dev/null 2>&1
./stop.sh > /dev/null 2>&1
timeout 40 bash -c 'while pgrep -x gmod_win64.exe >/dev/null; do sleep 1; done'
grep -E "^(whole|walk|npcs|fast)" "bench-$LABEL.txt" | cut -c1-62
