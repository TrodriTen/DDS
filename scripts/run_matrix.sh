#!/usr/bin/env bash
# =============================================================================
# run_matrix.sh — Ejecuta la matriz completa de experimentos (config/matrix.env)
# con scripts/run_benchmark.sh y genera el análisis agregado al final.
#
# Uso:
#   scripts/run_matrix.sh [--matrix config/matrix.env] [--testbed config/testbed.env]
#                         [--out results/matrix-FECHA] [--dry-run] [--resume DIR]
#                         [--only-scenario NOMBRE] [--only-mw fastdds|cyclonedds]
#
#   --dry-run   muestra el plan (orden aleatorizado) y la duración estimada
#   --resume    continúa una matriz interrumpida (salta ejecuciones ya completas)
# =============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MATRIX="$ROOT/config/matrix.env"
TESTBED="$ROOT/config/testbed.env"
OUT=""
DRY=0
ONLY_SCEN=""
ONLY_MW=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --matrix) MATRIX=$2; shift 2 ;;
    --testbed) TESTBED=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --resume) OUT=$2; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    --only-scenario) ONLY_SCEN=$2; shift 2 ;;
    --only-mw) ONLY_MW=$2; shift 2 ;;
    -h|--help) sed -n 2,14p "$0"; exit 0 ;;
    *) echo "opción desconocida: $1" >&2; exit 1 ;;
  esac
done

# shellcheck source=../config/matrix.env
source "$MATRIX"
OUT=${OUT:-$ROOT/results/matrix-$(date +%Y%m%d-%H%M%S)}
mkdir -p "$OUT"
cp "$MATRIX" "$OUT/matrix.env.used"
cp "$TESTBED" "$OUT/testbed.env.used"

# Lista de experimentos: escenario|args|transporte|ruido|rep  (+ middleware aleatorizado)
PLAN=()
add_block() { # scen args transports noises
  local scen=$1 args=$2 trs=$3 noises=$4
  [[ -n $ONLY_SCEN && $scen != "$ONLY_SCEN" ]] && return
  for rep in $(seq 1 "$REPETITIONS"); do
    for tr in $trs; do
      for noise in $noises; do
        local mws=$MIDDLEWARES
        [[ -n $ONLY_MW ]] && mws=$ONLY_MW
        # Orden de middlewares aleatorio pero reproducible: se ordena por
        # md5(semilla + bloque + middleware)
        local order
        order=$(for m in $mws; do
                  printf '%s %s\n' "$(printf '%s' "$SEED|$scen|$tr|$noise|$rep|$m" | md5sum | cut -c1-12)" "$m"
                done | sort | awk '{print $2}')
        for mw in $order; do PLAN+=("$scen|$args|$tr|$noise|$rep|$mw"); done
      done
    done
  done
}
for entry in "${SCENARIOS[@]}"; do add_block "${entry%%=*}" "${entry#*=}" "$TRANSPORTS" "$NOISE_PROFILES"; done
for entry in "${SHM_SCENARIOS[@]}"; do add_block "${entry%%=*}" "${entry#*=}" "$SHM_TRANSPORTS" "$SHM_NOISE_PROFILES"; done

per_run=$((WARMUP + BASELINE + NOISE_DURATION + RECOVERY + 25))
echo "Matriz: ${#PLAN[@]} ejecuciones, ~$((${#PLAN[@]} * per_run / 60)) min -> $OUT"
if [[ $DRY == 1 ]]; then
  printf '%s\n' "${PLAN[@]}" | awk -F'|' '{printf "  %-22s %-10s %-5s %-13s r%s  [%s]\n", $1, $6, $3, $4, $5, $2}'
  exit 0
fi

i=0
for item in "${PLAN[@]}"; do
  IFS='|' read -r scen args tr noise rep mw <<< "$item"
  i=$((i + 1))
  if compgen -G "$OUT/${scen}-${mw}-${tr}-${noise}-r${rep}-*/sub_*_summary.csv" >/dev/null; then
    echo "[$i/${#PLAN[@]}] ya existe: $scen $mw $tr $noise r$rep"
    continue
  fi
  echo "[$i/${#PLAN[@]}] $scen $mw $tr $noise r$rep"
  # shellcheck disable=SC2086
  "$ROOT/scripts/run_benchmark.sh" --mw "$mw" --transport "$tr" --scenario "$scen" --noise "$noise" \
    --noise-target "$NOISE_TARGET" --noise-bw "$NOISE_BW" --warmup "$WARMUP" --baseline "$BASELINE" \
    --noise-duration "$NOISE_DURATION" --recovery "$RECOVERY" --rep "$rep" --testbed "$TESTBED" \
    --out "$OUT" --no-analyze -- $args || echo "  !! ejecución fallida (continúa)"
  sleep 3   # deja expirar el descubrimiento de la ejecución anterior
done

source "$TESTBED"
python3 "$ROOT/scripts/analyze_results.py" "$OUT" --plots ${PUSHGATEWAY_URL:+--pushgateway "$PUSHGATEWAY_URL"}
echo "Matriz completa: $OUT/report.md"
