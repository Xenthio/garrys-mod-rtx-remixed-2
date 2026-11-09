#!/bin/bash
# kinds.sh <label>: runs bench.sh and keeps every rewrite of the proxy's
# statistics (shaderapi_proxy_slots.txt, one per 10 s interval) during it as
# perf/kinds-<label>/<wall-clock ms>.txt, then prints each interval's mesh
# build lines next to the bench phases it covered.
DEV="$(cd "$(dirname "$0")/.." && pwd)"; S="$DEV/game/bin/win64/shaderapi_proxy_slots.txt"
LABEL="${1:?label}"; OUT="$DEV/perf/kinds-$LABEL"; mkdir -p "$OUT"
"$DEV/bench.sh" "$LABEL" > "$OUT/bench.txt" 2>&1 &
BENCH=$!
last=$(stat -c %Y.%N "$S" 2>/dev/null)
while kill -0 $BENCH 2>/dev/null; do
  now=$(stat -c %Y.%N "$S" 2>/dev/null)
  if [ "$now" != "$last" ]; then
    sleep 0.3; cp "$S" "$OUT/$(date +%s%3N).txt"; last=$(stat -c %Y.%N "$S")
  fi
  sleep 0.2
done
cat "$OUT/bench.txt" | head -14 | cut -c1-120
PH="$DEV/bench-$LABEL-phases.txt"
for f in "$OUT"/[0-9]*.txt; do
  t=$(basename "$f" .txt)
  # The interval is the 10 s before this rewrite: the phases it overlaps.
  phases=$(awk -v e="$t" '{ if ($3 > e - 10000 && $2 < e) { $1 = $2 = $3 = ""; printf "%s,", substr($0, 4) } }' "$PH" 2>/dev/null)
  echo "== $(( (t - $(head -1 "$PH" | cut -d' ' -f2)) / 1000 )) s: ${phases%,}"
  grep -h "ms/frame: frame\|mesh build per frame\|per frame: texture" "$f" | sed 's/^ *//'
done
