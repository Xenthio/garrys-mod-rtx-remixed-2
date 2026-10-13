#!/bin/bash
# Runs the scripted play session (lua/autorun/openremix_bench.lua) in the
# running dev game and prints its frame-time statistics, with OpenRemix's own
# pacing figures for the same interval when rtx.native.gpuPassTimers is on.
# Usage: bench.sh [label]   (results are also kept as bench-<label>.txt)
DEV="$(cd "$(dirname "$0")" && pwd)"; G="$DEV/game"; D="$G/garrysmod/data"; O="$G/openremix"
LABEL="${1:-$(date +%H%M%S)}"
rm -f "$D/openremix_bench_state.txt" "$D/openremix_bench_result.txt"
# A timings dump resets OpenRemix's pacing statistics: one before, one after.
rm -f "$O/timings.txt"; : > "$O/timings.request"; sleep 1.5
# With OPENREMIX_PROXY_STALLS=<ms> the proxy samples the frames longer than
# that: counted from here (the reset is seen within 2 s).
B="$G/bin/win64"; : > "$B/shaderapi_proxy_stalls.reset"; sleep 2.5
"$DEV/con.sh" "openremix_bench" || exit 1
for i in $(seq 1 120); do grep -q done "$D/openremix_bench_state.txt" 2>/dev/null && break; sleep 1; done
grep -q done "$D/openremix_bench_state.txt" 2>/dev/null || { echo "bench.sh: the session did not finish" >&2; exit 1; }
rm -f "$O/timings.txt"; : > "$O/timings.request"
for i in $(seq 1 20); do [ -s "$O/timings.txt" ] && break; sleep 0.25; done; sleep 0.5
{ cat "$D/openremix_bench_result.txt"; grep -E "^GPU frame|^# pacing|^# Present call|^# longest|^# asynchronous|^# low latency|^# slowest raster|^# the frame that|^# texture images|^# slowest surface|^# tracer memory|^# slowest acceleration" "$O/timings.txt" 2>/dev/null; } | tee "$DEV/bench-$LABEL.txt"
cp "$D/openremix_bench_frames.txt" "$DEV/bench-$LABEL-frames.txt"
# The whole dump: its "# slow" lines list every frame over 40 ms.
cp "$O/timings.txt" "$DEV/bench-$LABEL-timings.txt" 2>/dev/null
sleep 2.5; cp "$B/shaderapi_proxy_stalls.txt" "$DEV/bench-$LABEL-stalls.txt" 2>/dev/null
cp "$B/shaderapi_proxy_opens.txt" "$DEV/bench-$LABEL-opens.txt" 2>/dev/null
