#!/usr/bin/env bash
# =============================================================================
# tune_host_rt.sh — Diagnóstico y ajuste de un nodo Linux PREEMPT_RT para el
# benchmark DDS (Data Writer / Data Reader).
#
# Uso (como root):
#   tune_host_rt.sh show                       # checklist del entorno (no modifica)
#   tune_host_rt.sh apply  --iface IF [opts]   # aplica ajustes (no persistentes)
#   tune_host_rt.sh revert --iface IF          # deshace qdisc/coalescing/governor
#   tune_host_rt.sh baseline [--cpus 2,3] [--duration 60]   # cyclictest de referencia
#
# Opciones de apply:
#   --iface IF          interfaz de la red de pruebas (obligatoria)
#   --irq-cpus LISTA    CPUs para las IRQ de la NIC (p.ej. 3; por defecto = mw-cpus)
#   --irq-prio N        prioridad SCHED_FIFO de los hilos IRQ de la NIC (def. 85)
#   --no-qdisc          no instalar la qdisc 'prio' (SO_PRIORITY 6 -> banda 0)
#   --no-coalesce       no desactivar interrupt coalescing
#   --sock-buf BYTES    net.core.{r,w}mem_max (def. 16777216)
#
# Parámetros de arranque recomendados (no se aplican aquí; editar GRUB):
#   isolcpus=managed_irq,domain,2-3 nohz_full=2-3 rcu_nocbs=2-3 irqaffinity=0-1
#   intel_pstate=disable processor.max_cstate=1 intel_idle.max_cstate=0
#   (en Raspberry Pi: añadir a cmdline.txt; cpufreq.default_governor=performance)
# =============================================================================
set -uo pipefail

CMD=${1:-show}
shift || true
IFACE=""
IRQ_CPUS=""
IRQ_PRIO=85
QDISC=1
COALESCE=1
SOCK_BUF=16777216
BL_CPUS=""
BL_DURATION=60

while [[ $# -gt 0 ]]; do
  case "$1" in
    --iface) IFACE=$2; shift 2 ;;
    --irq-cpus) IRQ_CPUS=$2; shift 2 ;;
    --irq-prio) IRQ_PRIO=$2; shift 2 ;;
    --no-qdisc) QDISC=0; shift ;;
    --no-coalesce) COALESCE=0; shift ;;
    --sock-buf) SOCK_BUF=$2; shift 2 ;;
    --cpus) BL_CPUS=$2; shift 2 ;;
    --duration) BL_DURATION=$2; shift 2 ;;
    *) echo "opción desconocida: $1" >&2; exit 1 ;;
  esac
done

ok()   { printf '  \033[32m[OK]\033[0m   %s\n' "$*"; }
warn() { printf '  \033[33m[WARN]\033[0m %s\n' "$*"; }
info() { printf '  \033[36m[..]\033[0m   %s\n' "$*"; }
need_root() { [[ $EUID -eq 0 ]] || { echo "requiere root" >&2; exit 1; }; }

nic_irqs() { # IRQs de la interfaz (MSI-X: <iface>-TxRx-N, <iface>-rx-N, ...)
  grep -E "\b${IFACE}(-|$|:)" /proc/interrupts | awk -F: '{gsub(/ /,"",$1); print $1}'
}

show() {
  echo "== Kernel"
  local v; v=$(uname -v)
  if [[ $(cat /sys/kernel/realtime 2>/dev/null) == 1 || $v == *PREEMPT_RT* ]]; then ok "PREEMPT_RT: $(uname -r)"; else warn "kernel sin PREEMPT_RT: $(uname -r) ($v)"; fi
  local iso; iso=$(cat /sys/devices/system/cpu/isolated 2>/dev/null)
  [[ -n $iso ]] && ok "isolcpus: $iso" || warn "sin CPUs aisladas (isolcpus)"
  local nohz; nohz=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null)
  [[ -n $nohz && $nohz != "(null)" ]] && ok "nohz_full: $nohz" || info "nohz_full no configurado"
  local rt; rt=$(cat /proc/sys/kernel/sched_rt_runtime_us)
  [[ $rt == -1 ]] && warn "RT throttling desactivado (sched_rt_runtime_us=-1): un bucle FIFO puede colgar la CPU" || ok "RT throttling: $rt us/s"
  echo "== CPU"
  local gov; gov=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)
  [[ $gov == performance || $gov == n/a ]] && ok "governor: $gov" || warn "governor: $gov (recomendado performance)"
  ok "clocksource: $(cat /sys/devices/system/clocksource/clocksource0/current_clocksource)"
  systemctl is-active --quiet irqbalance 2>/dev/null && warn "irqbalance activo (mueve IRQs a CPUs aisladas)" || ok "irqbalance inactivo"
  echo "== Límites del proceso (usuario actual)"
  [[ $(ulimit -r) == unlimited || $(ulimit -r) -ge 90 ]] && ok "rtprio: $(ulimit -r)" || warn "rtprio: $(ulimit -r) (sin root no habrá SCHED_FIFO)"
  [[ $(ulimit -l) == unlimited ]] && ok "memlock: unlimited" || warn "memlock: $(ulimit -l) KiB (mlockall fallará sin root)"
  echo "== Red"
  ok "net.core.rmem_max=$(sysctl -n net.core.rmem_max) wmem_max=$(sysctl -n net.core.wmem_max)"
  if [[ -n $IFACE ]]; then
    info "qdisc: $(tc qdisc show dev "$IFACE" 2>/dev/null | head -1)"
    command -v ethtool >/dev/null && info "coalescing: $(ethtool -c "$IFACE" 2>/dev/null | grep -E '^(rx-usecs|adaptive-rx)' | tr '\n' ' ')"
    for irq in $(nic_irqs); do
      local pid; pid=$(pgrep -f "irq/${irq}-" | head -1)
      info "IRQ $irq afinidad=$(cat /proc/irq/$irq/smp_affinity_list) hilo=${pid:-n/a} $( [[ -n $pid ]] && chrt -p "$pid" | tail -1 | awk -F: '{print $2}')"
    done
  fi
  echo "== Reloj"
  if command -v chronyc >/dev/null; then
    chronyc tracking 2>/dev/null | grep -E "Reference|System time|RMS offset|Leap" | sed 's/^/  /'
  elif command -v timedatectl >/dev/null; then
    timedatectl show -p NTPSynchronized 2>/dev/null | sed 's/^/  /'
  fi
  command -v ethtool >/dev/null && [[ -n $IFACE ]] && ethtool -T "$IFACE" 2>/dev/null | grep -q hardware-transmit \
    && info "NIC con timestamping hardware: PTP (linuxptp ptp4l/phc2sys) daría sincronía sub-µs" || true
}

apply() {
  need_root
  [[ -n $IFACE ]] || { echo "--iface obligatoria" >&2; exit 1; }
  echo "== Governor performance"
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null || true; done
  echo "== irqbalance"
  systemctl stop irqbalance 2>/dev/null || true
  echo "== Buffers de socket (absorben ráfagas durante el ruido)"
  sysctl -qw net.core.rmem_max="$SOCK_BUF" net.core.wmem_max="$SOCK_BUF" \
             net.core.rmem_default=4194304 net.core.wmem_default=4194304 \
             net.core.netdev_max_backlog=5000
  if [[ $COALESCE == 1 ]] && command -v ethtool >/dev/null; then
    echo "== Interrupt coalescing OFF en $IFACE (latencia frente a CPU)"
    ethtool -C "$IFACE" adaptive-rx off adaptive-tx off rx-usecs 0 tx-usecs 0 2>/dev/null || warn "coalescing no ajustable"
    ethtool -K "$IFACE" gro off lro off 2>/dev/null || true
  fi
  if [[ -n $IRQ_CPUS ]]; then
    echo "== IRQs de $IFACE -> CPUs $IRQ_CPUS, hilos IRQ SCHED_FIFO $IRQ_PRIO"
    for irq in $(nic_irqs); do
      echo "$IRQ_CPUS" > /proc/irq/$irq/smp_affinity_list 2>/dev/null || warn "no se pudo fijar IRQ $irq"
      for pid in $(pgrep -f "irq/${irq}-"); do chrt -f -p "$IRQ_PRIO" "$pid"; done
    done
    # softirq de red en PREEMPT_RT
    for c in $(echo "$IRQ_CPUS" | tr ',' ' '); do
      pid=$(pgrep -x "ksoftirqd/$c") && chrt -f -p "$((IRQ_PRIO - 1))" "$pid" || true
    done
  fi
  if [[ $QDISC == 1 ]]; then
    # 3 bandas estrictas; priomap: SO_PRIORITY 6/7 -> banda 0 (DDS EF del arnés),
    # 4/5 -> banda 1, resto -> banda 2 (incluye iperf3/tráfico best-effort).
    echo "== qdisc 'prio' en $IFACE: SO_PRIORITY 6 (EF) en banda 0"
    tc qdisc replace dev "$IFACE" root handle 1: prio bands 3 priomap 2 2 2 2 1 1 0 0 2 2 2 2 2 2 2 2
    tc qdisc replace dev "$IFACE" parent 1:1 pfifo limit 1000
    tc qdisc replace dev "$IFACE" parent 1:2 pfifo limit 1000
    tc qdisc replace dev "$IFACE" parent 1:3 fq_codel
  fi
  echo "== Límites RT persistentes para el usuario del benchmark"
  cat > /etc/security/limits.d/99-dds-bench.conf <<'EOF'
# DDS RT benchmark: SCHED_FIFO y mlockall sin root
*    -    rtprio    95
*    -    memlock   unlimited
EOF
  ok "ajustes aplicados (no persistentes salvo limits.d)"
}

revert() {
  need_root
  [[ -n $IFACE ]] && tc qdisc del dev "$IFACE" root 2>/dev/null || true
  [[ -n $IFACE ]] && command -v ethtool >/dev/null && ethtool -C "$IFACE" adaptive-rx on adaptive-tx on 2>/dev/null || true
  for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo schedutil > "$g" 2>/dev/null || true; done
  systemctl start irqbalance 2>/dev/null || true
  ok "revertido"
}

baseline() {
  need_root
  command -v cyclictest >/dev/null || { echo "instalar rt-tests (cyclictest)" >&2; exit 1; }
  local aff=()
  [[ -n $BL_CPUS ]] && aff=(-a "$BL_CPUS" -t "$(echo "$BL_CPUS" | tr ',' '\n' | wc -l)")
  echo "== cyclictest ${BL_DURATION}s (latencia de planificación del SO, referencia del suelo de jitter)"
  cyclictest -m -p 80 -i 1000 -h 400 -q -D "$BL_DURATION" "${aff[@]}" --mainaffinity=0 > /tmp/cyclictest.hist
  grep -E "Min Latencies|Avg Latencies|Max Latencies" /tmp/cyclictest.hist
}

case "$CMD" in
  show) show ;;
  apply) apply ;;
  revert) revert ;;
  baseline) baseline ;;
  *) sed -n 2,25p "$0"; exit 1 ;;
esac
