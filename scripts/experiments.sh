#!/usr/bin/env bash
# Runs the evaluation matrix from the README: every workload x GC policy x
# wear-leveling mode, printed as a markdown table.
#
#   ./scripts/experiments.sh [writes]
set -euo pipefail

BIN=${BIN:-./build/ftlsim}
WRITES=${1:-200000}
BLOCKS=${BLOCKS:-128}
PAGES_PER_BLOCK=${PAGES_PER_BLOCK:-64}
SEED=${SEED:-42}
# PREFILL=1 fills the device before measuring, so cold data exists and static
# wear leveling has something to migrate.
PREFILL_ARG=""
[[ "${PREFILL:-0}" == "1" ]] && PREFILL_ARG="--prefill"

if [[ ! -x "$BIN" ]]; then
  echo "error: $BIN not found -- build first (cmake --build build, or make)" >&2
  exit 1
fi

# Pull one metric out of a run by matching its summary line.
field() { grep -m1 "$2" <<<"$1" | sed -E "s/.*$3//; s/[^0-9.].*$//"; }

echo "ftlsim evaluation -- ${WRITES} host writes, ${BLOCKS}x${PAGES_PER_BLOCK} pages, seed ${SEED}${PREFILL_ARG:+, prefilled}"
echo
echo "| Workload | GC policy | Wear leveling | WAF | Erases | Erase min-max | Erase sigma |"
echo "|---|---|---|---|---|---|---|"

for workload in sequential random hotspot; do
  for policy in greedy cost-benefit; do
    for wl in none dynamic static; do
      out=$("$BIN" --workload "$workload" --pages "$WRITES" --blocks "$BLOCKS" \
            --pages-per-block "$PAGES_PER_BLOCK" --gc-policy "$policy" \
            --wear-leveling "$wl" --seed "$SEED" $PREFILL_ARG)

      waf=$(grep -m1 'Write Amplification' <<<"$out" | awk '{print $NF}')
      erases=$(grep -m1 'Total erases' <<<"$out" | awk '{print $NF}')
      emax=$(grep -m1 'Max erase count' <<<"$out" | awk '{print $4}')
      emin=$(grep -m1 'Max erase count' <<<"$out" | awk '{print $9}')
      sigma=$(grep -m1 'Erase spread' <<<"$out" | sed -E 's/.*stddev //; s/\)//')

      echo "| $workload | $policy | $wl | $waf | $erases | ${emin}-${emax} | $sigma |"
    done
  done
done
