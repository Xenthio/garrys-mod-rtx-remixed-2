#!/bin/bash
# dll-ab.sh <rounds> <labelA>=<remix.dll> <labelB>=<remix.dll>...: alternates
# OpenRemix builds at the fixed view of gpu-view.sh (A B A B ...), then prints
# each build's mean of the given passes (PASSES, a regex) over its rounds.
DEV="$(cd "$(dirname "$0")/.." && pwd)"
ROUNDS="${1:?rounds}"; shift
PASSES="${PASSES:-GPU frame|Trace opaque|Trace indirect}"
for r in $(seq 1 "$ROUNDS"); do
  for spec in "$@"; do
    label=${spec%%=*}; dll=${spec#*=}
    cp "$dll" "$DEV/game/bin/win64/openremix/remix.dll"
    "$DEV/perf/gpu-view.sh" "$label-$r" > "$DEV/perf/view-$label-$r.summary" 2>&1
    echo "round $r $label: $(grep -E "^($PASSES)" "$DEV/perf/view-$label-$r.summary" | awk -F'  +' '{printf "%s %s | ", $1, $2}')"
  done
done
for spec in "$@"; do
  label=${spec%%=*}
  echo "== $label"
  cat "$DEV"/perf/view-"$label"-*.summary | grep -E "^($PASSES)" |
    awk '{v=$NF; if ($1=="GPU") {v=$3}; name=$0; sub(/ +[0-9.]+.*$/, "", name); s[name]+=v; n[name]++} END {for (k in s) printf "  %-56s %.3f\n", k, s[k]/n[k]}'
done
