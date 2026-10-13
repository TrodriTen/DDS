#!/usr/bin/env bash
# =============================================================================
# run_local.sh — Ejecuta un par suscriptor + publicador en el MISMO host.
# Útil para escenarios colocalizados (SHM / zero-copy) y para validar la
# instalación antes de desplegar en el testbed distribuido.
#
# Uso:
#   scripts/run_local.sh <fastdds|cyclonedds> <udp|udpm|shm|zc> [opciones comunes...]
#   (las opciones se pasan a ambos binarios: --duration, --reliability, ...)
#
# Variables:
#   BUILD_DIR   directorio de los binarios (por defecto ./build)
#   OUT_DIR     resultados (por defecto ./results)
#   RUN_ID      identificador (por defecto local-<mw>-<transport>-<fecha>)
#   PUB_ARGS / SUB_ARGS  opciones adicionales sólo para pub / sub
# =============================================================================
set -euo pipefail

MW=${1:?middleware: fastdds|cyclonedds}
TRANSPORT=${2:?transporte: udp|udpm|shm|zc}
shift 2

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$ROOT/build}
OUT_DIR=${OUT_DIR:-$ROOT/results}
RUN_ID=${RUN_ID:-local-$MW-$TRANSPORT-$(date +%Y%m%d-%H%M%S)}
IFACE=${IFACE:-lo}
LOG_DIR="$OUT_DIR/$RUN_ID"
mkdir -p "$LOG_DIR"

[[ -n "${DDS_PREFIX:-}" ]] && export LD_LIBRARY_PATH="$DDS_PREFIX/lib:${LD_LIBRARY_PATH:-}"

ROUDI_PID=""
cleanup() {
  if [[ -n "$ROUDI_PID" ]]; then
    kill "$ROUDI_PID" 2>/dev/null || true
    wait "$ROUDI_PID" 2>/dev/null || true   # evita carreras con la siguiente ejecución
  fi
}
trap cleanup EXIT

# Cyclone + SHM necesita el daemon RouDi de iceoryx
if [[ $MW == cyclonedds && ( $TRANSPORT == shm || $TRANSPORT == zc ) ]]; then
  if ! pgrep -x iox-roudi >/dev/null; then
    ROUDI=$(command -v iox-roudi || echo "${DDS_PREFIX:-/opt/dds}/bin/iox-roudi")
    "$ROUDI" -c "$ROOT/config/iceoryx_roudi.toml" > "$LOG_DIR/roudi.log" 2>&1 &
    ROUDI_PID=$!
    for _ in $(seq 50); do
      grep -q "RouDi is ready" "$LOG_DIR/roudi.log" 2>/dev/null && break
      sleep 0.1
    done
  fi
fi

EXTRA=()
[[ $TRANSPORT == zc ]] && EXTRA+=(--loans)

COMMON=(--transport "$TRANSPORT" --iface "$IFACE" --run-id "$RUN_ID" --out-dir "$OUT_DIR")

"$BUILD_DIR/${MW}_subscriber" "${COMMON[@]}" "$@" ${SUB_ARGS:-} > "$LOG_DIR/sub.log" 2>&1 &
SUB_PID=$!
sleep 1
"$BUILD_DIR/${MW}_publisher" "${COMMON[@]}" "${EXTRA[@]}" "$@" ${PUB_ARGS:-} 2>&1 | tee "$LOG_DIR/pub.log" | grep -E "RESUMEN|WARN|rror|DSCP|emparej|loans" || true
wait "$SUB_PID" || true
grep -E "RESUMEN|rror" "$LOG_DIR/sub.log" || true
echo "Resultados: $LOG_DIR"
