# Metodología de medida

## 1. Sellos de tiempo y latencias

Cada muestra lleva un `BenchHeader` (48 B) que el publicador rellena justo antes de `write()`:

```
 Publicador (reloj P)                              Suscriptor (reloj S)
 T1' = CLOCK_MONOTONIC ┐
 T1  = CLOCK_REALTIME  ┴─ write() ─────── red ─────▶ take() → T2 = CLOCK_REALTIME
                                                       eco (si ECHO_REQUEST) → T3 = CLOCK_REALTIME → write()
 T4' = CLOCK_MONOTONIC ┐
 T4  = CLOCK_REALTIME  ┴─◀──────────────── red ──────  EchoReply { cabecera original, T2, T3 }
```

| Métrica | Fórmula | Reloj | Depende de la sincronía NTP |
|---|---|---|---|
| **E2E bruta** (`e2e_*`) | T2 − T1 | S − P | **sí** (incluye el offset θ) |
| **RTT** (`rtt_*`) | T4' − T1' | un único reloj MONOTONIC local | no |
| **RTT neto** (`rtt_net_*`) | (T4' − T1') − (T3 − T2) | diferencias en cada reloj | no (excluye el tiempo de proceso del suscriptor) |
| **Retardo de un sentido estimado** (`one_way_est_*`) | RTT neto / 2 | — | no (asume caminos simétricos) |
| **Offset de reloj** θ (`offset_us`) | ((T2 − T1) + (T3 − T4)) / 2 | — | es la medida del desfase |
| **E2E corregida** (`e2e_corr_*`) | T2 − T1 − θ | — | no (si θ se estimó bien) |

**Estimación de θ (como NTP, RFC 5905 §8):** el error de θ está acotado por ±δ/2, con
δ = RTT neto, y es nulo con caminos simétricos. El publicador conserva las últimas 64 respuestas de
eco y toma θ de la de **menor δ** (filtro de reloj de NTP): las respuestas retrasadas por colas — y por
tanto más asimétricas, típicamente durante el ruido — se descartan. El θ vigente viaja en cada
muestra (`clock_offset_est_ns` + flag `OFFSET_VALID`), así el suscriptor calcula la E2E corregida en
línea. La E2E corregida es fiable mientras la asimetría ida/vuelta sea pequeña frente a la latencia
medida; el RTT es la referencia libre de cualquier supuesto de sincronía.

El eco se solicita cada `--echo-every N` muestras (por defecto todas). El topic de eco usa la misma
fiabilidad y TRANSPORT_PRIORITY que el flujo, sin DEADLINE. El eco de las muestras de calentamiento
sí se responde (alimenta θ) pero no se contabiliza.

## 2. Resto de métricas

| Métrica | Definición |
|---|---|
| **Percentiles** p50/p95/p99/p99.9/p99.99 | Resumen final: **exactos** (rango más cercano) sobre todas las muestras crudas preasignadas (`--raw-capacity`, automático = tasa × duración × 1.2); `*_exact=1` lo confirma. Ventanas y fases: histograma log-lineal de 128 sub-buckets por octava (error relativo < 0.8 %). |
| **Jitter** | Desviación estándar del retardo (Welford, exacta) — `e2e_corr_std_us`, `rtt_std_us`. |
| **Throughput** | msg/s y MB/s (10⁶ B) de payload CDR serializado (incluye 4 B de encapsulación; sin cabeceras RTPS/UDP/IP). El tráfico real en la NIC está en `nic_rx_mbps`/`nic_tx_mbps`. |
| **Pérdidas (secuencia)** | Número de secuencia por flujo y publicador; `lost = (máx. visto − primero + 1) − únicas recibidas`, con ventana de reordenación de 4096 muestras (un `take()` agrupa por instancia, por lo que el desorden entre instancias no es pérdida). Detecta también duplicados y desorden. |
| **Pérdidas extremo a extremo** | `analyze_results.py`: (enviadas − recibidas) / enviadas, incluye las pérdidas de cola. |
| **SAMPLE_LOST / SAMPLE_REJECTED** | Estados DDS del DataReader (listener). Cyclone no reporta SAMPLE_LOST para huecos BEST_EFFORT; Fast DDS sí: la pérdida por secuencia es la métrica comparable. |
| **Deadline Miss Rate** | Fallos de `on_requested_deadline_missed` (suscriptor) / `on_offered_deadline_missed` (publicador) divididos por los periodos esperados: `fallos / (instancias × tiempo / DEADLINE)`. DEADLINE se evalúa **por instancia**. |
| **Ventanas activas** | Los contadores de estado del resumen sólo suman ventanas con flujo de datos: así no cuentan los deadline incumplidos tras el final de la publicación (el publicador además espera ACKs y elimina sus writers al terminar). |
| **Duración de `write()`** | T(después de write) − T1': coste síncrono del envío; crece si RELIABLE bloquea por historia llena (KEEP_ALL). |
| **Latencia de despertar** (`wakeup_*`) | Instante real de despertar − instante planificado: jitter del planificador/SO en el publicador. `overruns` cuenta ranuras saltadas (retraso > 1 periodo; no se publica en ráfaga para recuperar). |
| **CPU** | Δ(utime+stime) / Δt del proceso completo (incluye hilos del middleware), en % de un núcleo. Por hilo: `*_threads.csv`. |
| **Memoria** | VmRSS y VmHWM (pico); fallos de página mayores/menores (deben ser ~0 con `mlockall`). |
| **Red del host** | Δ de `RcvbufErrors`, `SndbufErrors`, `InErrors` (`/proc/net/snmp`, todo el host) y `rx_dropped`/`tx_dropped` de la NIC: distinguen pérdidas en el socket/host de pérdidas en el switch. |

## 3. Garantías de tiempo real del arnés

* `mlockall(MCL_CURRENT | MCL_FUTURE)`, `mallopt(M_TRIM_THRESHOLD=-1, M_MMAP_MAX=0)` y prefault de
  64 MiB de heap y 512 KiB de pila por hilo RT: sin page faults durante la medida.
* Afinidad (`pthread_setaffinity_np` / `sched_setaffinity`) y `SCHED_FIFO` por grupos: aplicación
  (`--cpus`, prio 80 writer/reader, 79 eco), middleware (`--mw-cpus`, prio 70, heredada al crear las
  entidades desde el hilo principal y fijada también en los XML) y housekeeping (`--hk-cpus`,
  `SCHED_OTHER`, hilo de métricas).
* Planificación periódica con instantes absolutos (`clock_nanosleep(TIMER_ABSTIME)`), sin deriva;
  varios flujos se intercalan en un único hilo por orden de vencimiento (EDF) y con desfase inicial.
* Camino crítico sin asignaciones ni locks: muestras reutilizadas, secuencia sonar con capacidad
  reservada, `take()` con préstamo (Fast DDS) o sobre búfer preasignado (Cyclone), acumuladores de
  ventana con doble búfer (protocolo tipo Dekker con orden secuencial). Los percentiles, CSV y la
  lectura de `/proc` se hacen en el hilo de métricas.
* `/dev/cpu_dma_latency` (`--cpu-dma-latency 0`) evita C-states profundos; `PR_SET_TIMERSLACK=1`.
* Al arrancar se registra el entorno: PREEMPT_RT, `isolcpus`, `nohz_full`, throttling RT, governor,
  clocksource, límites y estado NTP del kernel (`adjtimex`: sincronizado, error estimado/máximo).

## 4. Diseño experimental

* **Fases:** el orquestador escribe la fase (`baseline` → `noise` → `recovery`) en un fichero que
  ambos procesos leen en cada ventana (`--phase-file`). Los histogramas de cada ventana se fusionan
  en los de su fase, de modo que `*_phases.csv` da percentiles reales por fase (no promedios de
  percentiles de ventana). `analyze_results.py` calcula la degradación (×baseline) de cada ejecución.
* **Calentamiento:** las muestras de los primeros `--warmup` segundos se marcan `WARMUP` y no
  computan (descubrimiento, cachés, estimador de θ).
* **Repeticiones y orden:** `run_matrix.sh` ejecuta cada combinación `REPETITIONS` veces e intercala
  los middlewares en orden aleatorio reproducible (hash de semilla + bloque), para no confundir la
  deriva del testbed con el efecto del middleware. El informe usa la mediana y el rango entre
  repeticiones.
* **Equidad de configuración:** mismos tipos, presets, QoS, buffers de socket (4 MiB), afinidades y
  prioridades; temporizadores del protocolo fiable alineados (heartbeat 20 ms, respuestas a 1 ms) en
  ambos XML. `participant_defaults` (Fast DDS) permite medir la configuración de fábrica.
* **Ruido:** tres fuentes `iperf3` hacia el puerto del Data Reader (incast 3:1 sobre 1 GbE) llenan la
  cola de salida del switch, la misma que atraviesan los datos DDS. Perfiles `udp_saturate`
  (pérdidas por tail-drop), `tcp_bulk` (bufferbloat), `udp_burst` (microrráfagas), `mixed`. El
  objetivo `cross` sirve de control: tráfico que no comparte puertos con DDS. Los JSON de `iperf3`
  se recogen con cada ejecución para documentar la carga real inyectada.
* **Recomendado antes de medir:** `tune_host_rt.sh show/apply` en los nodos RT y
  `tune_host_rt.sh baseline` (cyclictest) para conocer el suelo de jitter del SO.

## Hallazgos de la validación local

Entorno: VM de 4 vCPU, kernel 6.18 **sin** PREEMPT_RT, comunicación por loopback, ejecutando los
procesos como root (SCHED_FIFO y `mlockall` efectivos). Sirve para validar la funcionalidad, no para
comparar rendimiento.

1. **Contabilidad de pérdidas** — descarte aleatorio del 2 % en los puertos UDP del dominio con
   nftables (`numgen random mod 100 < 2 drop`), flujo radar 1600 msg/s, 5 s:

   | Middleware | Fiabilidad | Enviadas | Recibidas | Pérdida medida | E2E p99 | Deadline incumplidos (sub) |
   |---|---|---|---|---|---|---|
   | Fast DDS | BEST_EFFORT | 7917 | 7753 | 2.07 % | 1.56 ms | 189 |
   | Fast DDS | RELIABLE (HB 20 ms) | 7934 | 7934 | 0 % | 24.6 ms | 1088 |
   | Fast DDS | RELIABLE (HB 5 ms) | 7935 | 7935 | 0 % | 6.6 ms | 32 |
   | Cyclone | BEST_EFFORT | 7919 | 7774 | 1.83 % | 1.46 ms | 124 |
   | Cyclone | RELIABLE (HB 20 ms) | 7957 | 7957 | 0 % | 1.72 ms | 16 |

   En RELIABLE la latencia de cola bajo pérdidas está dominada por cuándo se detecta y repara el
   hueco (bloqueo de cabeza de línea hasta la retransmisión). En Fast DDS escala con
   `heartbeat_period`; con el mismo periodo, Cyclone reparó mucho antes, lo que es coherente con que
   adjunte heartbeats a los propios datos cuando hay datos sin confirmar. Es un parámetro a vigilar en
   el escenario de ruido `udp_saturate`.

2. **Cyclone + iceoryx (PSMX)** — `psmx_iox` (`iox_type_qos_supported`) rechaza endpoints con DEADLINE
   finito, LIVELINESS distinta de AUTOMATIC, durabilidad persistente o particiones con comodines;
   Cyclone entonces usa UDP loopback sin error. Con `--deadline-ms 0`: loans activos en `RadarTrack` y
   `WeatherStation`, CPU del suscriptor 15 % → 11 % y E2E p50 170 → 118 µs frente al mismo caso por
   UDP loopback. Los loans se limitan a tipos auto-contenidos: construir un `std::vector` (SonarPing)
   dentro de un chunk prestado fugaría memoria.

3. **Fast DDS** — sólo expande `${VAR}` en el texto de los elementos XML, no en atributos (la
   allowlist de interfaz se fija desde el código con `--iface`); exige
   `announcement_period < lease_duration` en liveliness (por defecto 3 s), y loans sólo en tipos
   *plain* (`RadarTrack`, `WeatherStation`).

4. **Multicast** — con un solo lector Cyclone envía por unicast aunque `AllowMulticast=true`
   (`AddrsetCosts`: uc=2 < mc=3); el overlay `cyclonedds_multicast.xml` mapea los topics de datos a
   una partición de red multicast y abarata el coste multicast. Verificado en `/proc/net/igmp`: el grupo
   de datos 239.255.0.2 lo une exactamente el DataReader en ambos middlewares.

5. **TRANSPORT_PRIORITY** no se traduce a DSCP en ninguna de las dos versiones (comprobado en el
   código fuente); el arnés marca los sockets (ver README).

6. **Ruido en loopback** — `iperf3` a 3.6 Gbit/s sobre loopback no degradó la latencia DDS (incluso la
   redujo, al mantener la CPU fuera de estados de reposo): sin la cola compartida del puerto de un
   switch, el ruido de red no se manifiesta. La degradación debe medirse en el testbed físico.
