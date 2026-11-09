#!/bin/bash
# gpu-view.sh <label> [VAR=value...]: GPU pass cost at a fixed view.
# Launches the dev game (gm_construct) with those environment variables,
# stands at the map spawn looking along VIEW_ANGLES (default "8 60 0"), waits
# for pipelines, turns on rtx.native.gpuPassTimers, takes DUMPS (default 8)
# timings dumps 1.5 s apart and a screenshot into perf/view-<label>/, closes
# the game and prints the mean of each pass over the dumps.
DEV="$(cd "$(dirname "$0")/.." && pwd)"; O="$DEV/game/openremix"
LABEL="${1:?label}"; shift
OUT="$DEV/perf/view-$LABEL"; mkdir -p "$OUT"
cd "$DEV" || exit 1
if pgrep -x gmod_win64.exe >/dev/null; then echo "gpu-view.sh: a game is running" >&2; exit 1; fi
rm -f /home/cr/proj/gmod-dev/game/bin/win64/shaderapi_proxy_diag.log
(env "$@" nohup ./run.sh +map gm_construct > run.log 2>&1 &)
sleep 10
timeout 200 bash -c "until grep -aq 'world loaded 1' game/bin/win64/shaderapi_proxy_diag.log 2>/dev/null; do sleep 5; done" ||
  { echo "gpu-view.sh: map not loaded" >&2; ./stop.sh; exit 1; }
sleep 8
./con.sh "setang ${VIEW_ANGLES:-8 60 0}"
echo "rtx.native.gpuPassTimers = 1" > "$O/config.request"
# Pipelines for the view (and any variant the environment asks for) compile in the background.
sleep "${SETTLE:-60}"
for n in $(seq 1 "${DUMPS:-8}"); do
  rm -f "$O/timings.txt"; : > "$O/timings.request"
  for i in $(seq 1 40); do [ -s "$O/timings.txt" ] && break; sleep 0.1; done
  cp "$O/timings.txt" "$OUT/timings-$n.txt" 2>/dev/null
  sleep 1.5
done
rm -f "$O/screenshot.png"; : > "$O/screenshot.request"
for i in $(seq 1 40); do [ -s "$O/screenshot.png" ] && break; sleep 0.25; done
sleep 1; cp "$O/screenshot.png" "$OUT/screenshot.png" 2>/dev/null
./stop.sh > /dev/null 2>&1
timeout 40 bash -c 'while pgrep -x gmod_win64.exe >/dev/null; do sleep 1; done'
python3 - "$OUT" <<'EOF'
import collections, glob, re, sys
passes = collections.defaultdict(list); frames = []
for f in sorted(glob.glob(sys.argv[1] + "/timings-*.txt")):
    for line in open(f):
        m = re.match(r"GPU frame ([\d.]+)", line)
        if m: frames.append(float(m.group(1)))
        m = re.match(r"(.{56}) +([\d.]+) +([\d.]+)$", line.rstrip())
        if m and not line.startswith("#"): passes[m.group(1).strip()].append(float(m.group(2)))
if not frames:
    sys.exit("no GPU pass timings (were the timers on?)")
print(f"{'GPU frame':56} {sum(frames)/len(frames):8.3f}  ({len(frames)} dumps, min {min(frames):.3f} max {max(frames):.3f})")
for name, v in sorted(passes.items(), key=lambda kv: -sum(kv[1]) / len(kv[1]))[:14]:
    print(f"{name:56} {sum(v)/len(v):8.3f}")
EOF
