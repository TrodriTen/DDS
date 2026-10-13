#!/usr/bin/env bash
# =============================================================================
# run_benchmark.sh — Orquesta UN experimento en el testbed distribuido:
#
#   t=0 ........ warmup ........ baseline ........ noise ........ recovery ..| fin
#        sub + pub arrancan     fase=baseline    iperf3 activo    fase=recovery
#
#  1. Arranca el suscriptor en el Data Reader y el publicador en el Data Writer
#     (SSH, con privilegios RT) con un fichero de fase en ambos nodos.
#  2. Cambia la fase (baseline -> noise -> recovery) en ambos nodos y lanza el
#     ruido iperf3 desde Servidor de Ruido / RPI 1 / RPI 2 hacia el objetivo.
#  3. Espera, recoge los CSV de ambos nodos y ejecuta el análisis.
#
# Uso:
#   scripts/run_benchmark.sh --mw fastdds|cyclonedds [opciones] [-- args extra para pub y sub]
#
# Opciones:
#   --transport udp|udpm|shm|zc   (shm/zc: pub y sub colocalizados en el Data Writer)
#   --scenario NOMBRE             etiqueta del escenario (p.ej. reliable_default)
#   --noise PERFIL                none|udp_saturate|tcp_bulk|udp_burst|mixed  (def. none)
#   --noise-target T              reader|writer|both|cross  (def. reader: incast en el puerto del Data Reader)
#   --noise-bw BW                 tasa por fuente de ruido (def. 950M)
#   --warmup S --baseline S --noise-duration S --recovery S   (def. 5/30/60/30)
#   --rep N                       nº de repetición (etiqueta)
#   --testbed FICHERO             (def. config/testbed.env)
#   --out DIR                     resultados locales (def. ./results)
#   --no-analyze                  no ejecutar analyze_results.py al final
# =============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MW=""
TRANSPORT=udp
SCENARIO=default
NOISE=none
NOISE_TARGET=reader
NOISE_BW=950M
WARMUP=5
BASELINE=30
NOISE_DUR=60
RECOVERY=30
REP=1
TESTBED="$ROOT/config/testbed.env"
OUT="$ROOT/results"
ANALYZE=1
EXTRA=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mw) MW=$2; shift 2 ;;
    --transport) TRANSPORT=$2; shift 2 ;;
    --scenario) SCENARIO=$2; shift 2 ;;
    --noise) NOISE=$2; shift 2 ;;
    --noise-target) NOISE_TARGET=$2; shift 2 ;;
    --noise-bw) NOISE_BW=$2; shift 2 ;;
    --warmup) WARMUP=$2; shift 2 ;;
    --baseline) BASELINE=$2; shift 2 ;;
    --noise-duration) NOISE_DUR=$2; shift 2 ;;
    --recovery) RECOVERY=$2; shift 2 ;;
    --rep) REP=$2; shift 2 ;;
    --testbed) TESTBED=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --no-analyze) ANALYZE=0; shift ;;
    --) shift; EXTRA=("$@"); break ;;
    -h|--help) sed -n 2,33p "$0"; exit 0 ;;
    *) echo "opción desconocida: $1" >&2; exit 1 ;;
  esac
done
[[ $MW == fastdds || $MW == cyclonedds ]] || { echo "--mw fastdds|cyclonedds" >&2; exit 1; }

# shellcheck source=../config/testbed.env
source "$TESTBED"
[[ $NOISE == none ]] && NOISE_DUR_EFF=0 || NOISE_DUR_EFF=$NOISE_DUR
DURATION=$((BASELINE + NOISE_DUR + RECOVERY))
RUN_ID="${SCENARIO}-${MW}-${TRANSPORT}-${NOISE}-r${REP}-$(date +%Y%m%d-%H%M%S)"
LOCAL_DIR="$OUT/$RUN_ID"
mkdir -p "$LOCAL_DIR"
PHASE_FILE="$REMOTE_PHASE_DIR/$RUN_ID.phase"

# Colocalizado (SHM / zero-copy): ambos procesos en el Data Writer
SUB_HOST=$READER_HOST
SUB_IFACE=$READER_IFACE
if [[ $TRANSPORT == shm || $TRANSPORT == zc ]]; then
  SUB_HOST=$WRITER_HOST
  SUB_IFACE=$WRITER_IFACE
fi

log() { printf '[%(%H:%M:%S)T] %s\n' -1 "$*" | tee -a "$LOCAL_DIR/orchestrator.log"; }

# Ejecuta un comando en un host ("local" = sin SSH)
rexec() {
  local host=$1; shift
  if [[ $host == local ]]; then bash -c "$*"; else ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" "$*"; fi
}
# Copia un directorio remoto al local
rfetch() {
  local host=$1 src=$2 dst=$3
  if [[ $host == local ]]; then cp -r "$src"/. "$dst"/ 2>/dev/null || true
  else scp -q -r -o BatchMode=yes "$host:$src/." "$dst"/ 2>/dev/null || true; fi
}
# Copia ficheros remotos que casan con un patrón (glob en el host remoto)
rfetch_glob() {
  local host=$1 pattern=$2 dst=$3
  if [[ $host == local ]]; then cp $pattern "$dst"/ 2>/dev/null || true
  else scp -q -o BatchMode=yes "$host:$pattern" "$dst"/ 2>/dev/null || true; fi
}
set_phase() {
  local p=$1
  log "fase -> $p"
  rexec "$WRITER_HOST" "echo $p > $PHASE_FILE" &
  [[ $SUB_HOST != "$WRITER_HOST" ]] && rexec "$SUB_HOST" "echo $p > $PHASE_FILE" &
  wait
}

BIN="$REMOTE_BENCH_PREFIX/bin"
ENVV="LD_LIBRARY_PATH=$REMOTE_DDS_PREFIX/lib DDS_BENCH_CONFIG_DIR=$REMOTE_BENCH_PREFIX/share/dds_bench/config"
COMMON="--transport $TRANSPORT --run-id $RUN_ID --scenario $SCENARIO --out-dir $REMOTE_RESULTS_DIR \
 --domain $DOMAIN_ID --mcast-addr $MCAST_ADDR --phase-file $PHASE_FILE \
 --cpus $APP_CPUS --mw-cpus $MW_CPUS --hk-cpus $HK_CPUS --prio $APP_PRIO --mw-prio $MW_PRIO \
 --cpu-dma-latency $CPU_DMA_LATENCY -q ${EXTRA[*]:-}"
LOANS=""
[[ $TRANSPORT == zc ]] && LOANS="--loans"

cat > "$LOCAL_DIR/run_info.json" <<EOF
{"run_id": "$RUN_ID", "middleware": "$MW", "transport": "$TRANSPORT", "scenario": "$SCENARIO",
 "noise": "$NOISE", "noise_target": "$NOISE_TARGET", "noise_bw": "$NOISE_BW", "rep": $REP,
 "warmup_s": $WARMUP, "baseline_s": $BASELINE, "noise_s": $NOISE_DUR_EFF, "recovery_s": $RECOVERY,
 "extra_args": "${EXTRA[*]:-}", "started": "$(date -Is)"}
EOF

log "RUN $RUN_ID"
for h in "$WRITER_HOST" "$SUB_HOST"; do
  # El publicador etiqueta sus propias ventanas de warmup; el fichero arranca en baseline
  rexec "$h" "mkdir -p $REMOTE_PHASE_DIR $REMOTE_RESULTS_DIR && echo baseline > $PHASE_FILE"
done

# RouDi para Cyclone + SHM
ROUDI_STARTED=0
if [[ $MW == cyclonedds && ( $TRANSPORT == shm || $TRANSPORT == zc ) ]]; then
  rexec "$WRITER_HOST" "pgrep -x iox-roudi >/dev/null || ($SUDO nohup $REMOTE_DDS_PREFIX/bin/iox-roudi -c $REMOTE_BENCH_PREFIX/share/dds_bench/config/iceoryx_roudi.toml > /tmp/roudi_$RUN_ID.log 2>&1 &) ; sleep 1"
  ROUDI_STARTED=1
fi

# Suscriptor (Data Reader) y publicador (Data Writer) en segundo plano remoto
SUB_CMD="$SUDO env $ENVV $BIN/${MW}_subscriber $COMMON --iface $SUB_IFACE --match-timeout 120 --duration $DURATION --warmup $WARMUP"
PUB_CMD="$SUDO env $ENVV $BIN/${MW}_publisher $COMMON $LOANS --iface $WRITER_IFACE --match-timeout 60 --duration $DURATION --warmup $WARMUP"
SUB_PID=$(rexec "$SUB_HOST" "nohup $SUB_CMD > $REMOTE_RESULTS_DIR/$RUN_ID.sub.log 2>&1 & echo \$!")
log "suscriptor PID $SUB_PID en $SUB_HOST"
sleep 2
PUB_PID=$(rexec "$WRITER_HOST" "nohup $PUB_CMD > $REMOTE_RESULTS_DIR/$RUN_ID.pub.log 2>&1 & echo \$!")
log "publicador PID $PUB_PID en $WRITER_HOST"

cleanup() {
  rexec "$WRITER_HOST" "$SUDO kill $PUB_PID 2>/dev/null" || true
  rexec "$SUB_HOST" "$SUDO kill $SUB_PID 2>/dev/null" || true
  for h in "$NOISE_SERVER_HOST" "$NOISE_RPI1_HOST" "$NOISE_RPI2_HOST"; do rexec "$h" "pkill -x iperf3" 2>/dev/null || true; done
}
trap 'log "interrumpido"; cleanup; exit 130' INT TERM

# Espera emparejamiento + warmup (el publicador espera a los suscriptores)
sleep $((WARMUP + 3))
set_phase baseline
sleep "$BASELINE"

# ------------------------------------------------------------------ ruido ---
if [[ $NOISE != none ]]; then
  NOISE_SCRIPT="$REMOTE_BENCH_PREFIX/share/dds_bench/scripts/noise.sh"
  declare -a SOURCES=("$NOISE_SERVER_HOST" "$NOISE_RPI1_HOST" "$NOISE_RPI2_HOST")
  # Objetivo de cada fuente
  target_for() {
    case "$NOISE_TARGET" in
      reader) echo "$READER_HOST $READER_IP" ;;
      writer) echo "$WRITER_HOST $WRITER_IP" ;;
      both) if (( $1 % 2 == 0 )); then echo "$READER_HOST $READER_IP"; else echo "$WRITER_HOST $WRITER_IP"; fi ;;
      cross) echo "$NOISE_SERVER_HOST $NOISE_SERVER_IP" ;;   # tráfico que NO comparte puertos con DDS
    esac
  }
  # Servidores iperf3 en los destinos (en CPUs de housekeeping)
  for i in "${!SOURCES[@]}"; do
    read -r thost _ <<< "$(target_for "$i")"
    rexec "$thost" "$NOISE_SCRIPT server --ports 5201-5204 --cpus ${HK_CPUS%%,*}" || true
  done
  set_phase noise
  for i in "${!SOURCES[@]}"; do
    src=${SOURCES[$i]}
    read -r _ tip <<< "$(target_for "$i")"
    [[ $NOISE_TARGET == cross && $src == "$NOISE_SERVER_HOST" ]] && continue  # el servidor recibe
    rexec "$src" "$NOISE_SCRIPT client --target $tip --port $((5201 + i)) --profile $NOISE --bw $NOISE_BW \
      --duration $NOISE_DUR --log /tmp/noise_${RUN_ID}_$i.json" > "$LOCAL_DIR/noise_$i.out" 2>&1 &
  done
  sleep "$NOISE_DUR"
  wait || true
  set_phase recovery
  for i in "${!SOURCES[@]}"; do
    rfetch_glob "${SOURCES[$i]}" "/tmp/noise_${RUN_ID}_$i.json*" "$LOCAL_DIR"
  done
else
  # Sin ruido toda la ejecución es baseline (referencia para la degradación)
  sleep "$NOISE_DUR"
fi

# ------------------------------------------------------------- final ---------
log "esperando fin del publicador..."
for _ in $(seq 600); do
  rexec "$WRITER_HOST" "kill -0 $PUB_PID 2>/dev/null" || break
  sleep 1
done
for _ in $(seq 60); do
  rexec "$SUB_HOST" "kill -0 $SUB_PID 2>/dev/null" || break
  sleep 1
done
[[ $ROUDI_STARTED == 1 ]] && rexec "$WRITER_HOST" "$SUDO pkill -x iox-roudi" || true

log "recogiendo resultados"
rfetch "$WRITER_HOST" "$REMOTE_RESULTS_DIR/$RUN_ID" "$LOCAL_DIR"
[[ $SUB_HOST != "$WRITER_HOST" ]] && rfetch "$SUB_HOST" "$REMOTE_RESULTS_DIR/$RUN_ID" "$LOCAL_DIR"
for h in "$WRITER_HOST" "$SUB_HOST"; do
  rexec "$h" "cat $REMOTE_RESULTS_DIR/$RUN_ID.*.log 2>/dev/null" >> "$LOCAL_DIR/remote_logs.txt" || true
done

if [[ $ANALYZE == 1 ]]; then
  python3 "$ROOT/scripts/analyze_results.py" "$LOCAL_DIR" --quiet || log "análisis falló"
fi
log "resultados en $LOCAL_DIR"
