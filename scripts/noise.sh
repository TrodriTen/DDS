#!/usr/bin/env bash
# =============================================================================
# noise.sh — Generación de tráfico cruzado con iperf3 en los nodos de ruido
# (Servidor de Ruido, RPI Ruido 1, RPI Ruido 2). Lo invoca run_benchmark.sh
# por SSH, pero puede usarse a mano.
#
#   noise.sh server [--ports 5201-5204] [--cpus 0]
#   noise.sh client --target IP --profile PERFIL --duration S [--bw 950M]
#                   [--parallel N] [--port 5201] [--dscp 0] [--log FICHERO]
#   noise.sh stop
#
# Perfiles (cómo estresan el switch convencional, sin TSN):
#   udp_saturate  Flujo UDP no reactivo a --bw (por defecto tasa de línea):
#                 llena la cola de salida del puerto destino -> pérdidas
#                 por tail-drop compartidas con el tráfico DDS.
#   tcp_bulk      N flujos TCP: rellenan los buffers del switch hasta la
#                 pérdida (bufferbloat) -> aumento de latencia y jitter.
#   udp_burst     Ráfagas UDP a tasa de línea de 200 ms cada 1 s:
#                 microrráfagas que agotan el buffer del puerto.
#   mixed         tcp_bulk + udp a 30 % de --bw.
# El tráfico de ruido sale con DSCP 0 (best effort) salvo --dscp.
# =============================================================================
set -euo pipefail

CMD=${1:-}
shift || true
TARGET=""
PROFILE=udp_saturate
DURATION=60
BW=950M
PARALLEL=4
PORT=5201
PORTS=5201-5204
DSCP=0
CPUS=""
LOG=/tmp/noise_$(hostname)_$$.json

while [[ $# -gt 0 ]]; do
  case "$1" in
    --target) TARGET=$2; shift 2 ;;
    --profile) PROFILE=$2; shift 2 ;;
    --duration) DURATION=$2; shift 2 ;;
    --bw) BW=$2; shift 2 ;;
    --parallel) PARALLEL=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --ports) PORTS=$2; shift 2 ;;
    --dscp) DSCP=$2; shift 2 ;;
    --cpus) CPUS=$2; shift 2 ;;
    --log) LOG=$2; shift 2 ;;
    *) echo "opción desconocida: $1" >&2; exit 1 ;;
  esac
done

command -v iperf3 >/dev/null || { echo "iperf3 no instalado" >&2; exit 1; }

bw_to_bits() { # 950M -> 950000000
  local v=${1%[KkMmGg]} u=${1: -1}
  case "$u" in
    K|k) echo $((v * 1000)) ;;
    M|m) echo $((v * 1000000)) ;;
    G|g) echo $((v * 1000000000)) ;;
    *) echo "$1" ;;
  esac
}
TOS=$((DSCP << 2))
PIN=()
[[ -n $CPUS ]] && PIN=(taskset -c "$CPUS")

case "$CMD" in
  server)
    # Un servidor por puerto: cada fuente de ruido usa el suyo (iperf3 atiende
    # una sola prueba a la vez por servidor). En los nodos RT, fijar --cpus a
    # housekeeping para no competir con los hilos del benchmark.
    for p in $(seq "${PORTS%-*}" "${PORTS#*-}"); do
      pgrep -f "iperf3 -s -p $p" >/dev/null || "${PIN[@]}" iperf3 -s -p "$p" -D
    done
    ;;
  client)
    [[ -n $TARGET ]] || { echo "--target obligatorio" >&2; exit 1; }
    case "$PROFILE" in
      udp_saturate)
        "${PIN[@]}" iperf3 -c "$TARGET" -p "$PORT" -u -b "$BW" -l 1400 -P "$PARALLEL" -t "$DURATION" -S "$TOS" -J > "$LOG"
        ;;
      tcp_bulk)
        "${PIN[@]}" iperf3 -c "$TARGET" -p "$PORT" -P "$PARALLEL" -t "$DURATION" -S "$TOS" -J > "$LOG"
        ;;
      udp_burst)
        # iperf3 -t sólo admite segundos enteros: la ráfaga de ~200 ms se acota
        # por volumen (-n) = tasa * 0.2 s.
        burst_bytes=$(( $(bw_to_bits "$BW") / 8 / 5 ))
        end=$((SECONDS + DURATION))
        : > "$LOG"
        while (( SECONDS < end )); do
          "${PIN[@]}" iperf3 -c "$TARGET" -p "$PORT" -u -b "$BW" -l 1400 -n "$burst_bytes" -S "$TOS" -J >> "$LOG" 2>/dev/null || true
          sleep 0.8
        done
        ;;
      mixed)
        "${PIN[@]}" iperf3 -c "$TARGET" -p "$PORT" -P "$PARALLEL" -t "$DURATION" -S "$TOS" -J > "$LOG" &
        "${PIN[@]}" iperf3 -c "$TARGET" -p "$((PORT + 1))" -u -b "$(( $(bw_to_bits "$BW") * 3 / 10 ))" -l 1400 -t "$DURATION" -S "$TOS" -J > "$LOG.udp" &
        wait
        ;;
      none) sleep "$DURATION" ;;
      *) echo "perfil desconocido: $PROFILE" >&2; exit 1 ;;
    esac
    ;;
  stop)
    pkill -x iperf3 || true
    ;;
  *)
    sed -n 2,26p "$0"
    exit 1
    ;;
esac
