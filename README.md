# DDS RT Benchmark — eProsima Fast DDS vs Eclipse Cyclone DDS

Entorno de benchmarking comparativo, en **C++20**, de **Fast DDS 3.6.2** y **Cyclone DDS 11.0.1**
(OMG DDS / DDSI-RTPS) para telemetría de sensores críticos sobre **Linux PREEMPT_RT**, con nodos
conectados a un switch Ethernet convencional (sin TSN) y tráfico cruzado de ruido (`iperf3`).

El mismo motor de medida (publicador/suscriptor) se compila contra los dos middlewares; sólo
cambia una capa fina de backend. Así ambos se miden con el mismo planificador, los mismos
generadores de datos, la misma instrumentación y las mismas métricas.

## Entregables

| # | Entregable | Ubicación |
|---|---|---|
| 1 | IDL sintético | [`idl/SensorsTelemetry.idl`](idl/SensorsTelemetry.idl) |
| 2 | Comandos de generación (`fastddsgen`, `idlc`) | [`scripts/generate_types.sh`](scripts/generate_types.sh) (CMake también los ejecuta al compilar) |
| 3 | Perfiles XML | [`config/fastdds_profiles.xml`](config/fastdds_profiles.xml), [`config/cyclonedds.xml`](config/cyclonedds.xml) + overlays [`cyclonedds_multicast.xml`](config/cyclonedds_multicast.xml), [`cyclonedds_shm.xml`](config/cyclonedds_shm.xml), QoS Provider [`cyclonedds_qos.xml`](config/cyclonedds_qos.xml), [`iceoryx_roudi.toml`](config/iceoryx_roudi.toml) |
| 4 | Publisher (con generador sintético) y Subscriber deterministas | [`src/fastdds/`](src/fastdds), [`src/cyclonedds/`](src/cyclonedds), motor común en [`include/bench/`](include/bench) y [`src/core/`](src/core) |
| 5 | Prueba de estrés y recolección de métricas (CSV / formato Prometheus) | [`scripts/run_benchmark.sh`](scripts/run_benchmark.sh), [`run_matrix.sh`](scripts/run_matrix.sh), [`noise.sh`](scripts/noise.sh), [`analyze_results.py`](scripts/analyze_results.py), [`run_local.sh`](scripts/run_local.sh), [`tune_host_rt.sh`](scripts/tune_host_rt.sh) |

La metodología de medida (RTT, E2E, offset de reloj, jitter, pérdidas, deadline…) está en
[`docs/METODOLOGIA.md`](docs/METODOLOGIA.md).

## Arquitectura

```
            Data Writer (PREEMPT_RT)                                 Data Reader (PREEMPT_RT)
 ┌──────────────────────────────────────────┐            ┌──────────────────────────────────────────┐
 │ bench.writer  FIFO 80  CPU app            │  datos     │ bench.reader  FIFO 80  CPU app            │
 │  planificador EDF (clock_nanosleep ABS)   │──────────▶ │  WaitSet → take() → T2                    │
 │  generadores Radar/Sonar/Meteo → T1 →write│  (RTPS)    │  eco inmediato (T3) si ECHO_REQUEST        │
 │ bench.echo    FIFO 79  CPU app            │ ◀────────── │  E2E bruta / corregida, secuencia, pérdidas│
 │  RTT (MONOTONIC), offset θ tipo NTP       │   eco      │                                           │
 │ hilos middleware FIFO 70  CPU mw          │            │ hilos middleware FIFO 70  CPU mw          │
 │ bench.stats  SCHED_OTHER CPU housekeeping │            │ bench.stats  SCHED_OTHER CPU housekeeping │
 │  ventanas 1 s → CSV / Prometheus          │            │  ventanas 1 s → CSV / Prometheus          │
 └──────────────────────────────────────────┘            └──────────────────────────────────────────┘
                      │                 switch Ethernet estándar                 │
                      └──────── Servidor de Ruido · RPI Ruido 1 · RPI Ruido 2 ───┘  (iperf3)
```

* **Núcleo común (sin DDS):** RT (`mlockall`, `sched_setaffinity`, `SCHED_FIFO`, prefault de pila
  y heap, `/dev/cpu_dma_latency`), histograma log-lineal (<0.8 % de error) más muestras crudas para
  percentiles exactos, detector de pérdidas/duplicados/desorden, estimador de offset de reloj,
  acumuladores de ventana con doble búfer lock-free (los hilos RT sólo registran; los percentiles se
  calculan fuera del camino crítico), métricas de proceso (`/proc`) y de red (`/proc/net/snmp`, NIC).
* **Backends:** [`fastdds_backend.hpp`](src/fastdds/fastdds_backend.hpp) (API DDS de Fast DDS 3.x,
  `take()` con préstamo, `loan_sample()` en tipos *plain*) y
  [`cyclone_backend.hpp`](src/cyclonedds/cyclone_backend.hpp) (cyclonedds-cxx; camino crítico con
  `dds_write`/`dds_waitset_wait` de la API C, sin excepciones).
* **Herencia de prioridades:** el hilo principal crea las entidades DDS con la afinidad y prioridad
  del middleware (`--mw-cpus`, `--mw-prio`), de modo que todos los hilos internos las heredan; además
  ambos XML fijan explícitamente la política de sus hilos. El informe `*_threads.csv` muestra la
  política y CPUs efectivas de cada hilo.

## Instalación

```bash
# Middleware desde fuentes, versiones fijadas (Fast DDS 3.6.2 + Fast-CDR 2.3.6 + fastddsgen 4.3.0,
# Cyclone DDS 11.0.1 + cyclonedds-cxx 11.0.1, iceoryx 2.0.6 opcional para SHM en Cyclone)
sudo scripts/install_middleware.sh --prefix /opt/dds --with-iceoryx
source /opt/dds/setup.bash

# Benchmark (genera el código de los tipos con fastddsgen e idlc durante la compilación)
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=/opt/dds -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build            # tests del núcleo (histograma, secuencias, offset, DSCP, CLI)
sudo cmake --install build --prefix /opt/dds_bench   # binarios + config + scripts en cada nodo
```

Binarios: `fastdds_publisher`, `fastdds_subscriber`, `cyclonedds_publisher`, `cyclonedds_subscriber`
(`--help` en cualquiera de ellos lista todas las opciones).

### Generación de código (comandos exactos)

```bash
fastddsgen -replace -cs -flat-output-dir -d generated/fastdds idl/SensorsTelemetry.idl
idlc -l cxx -Wno-implicit-extensibility -o generated/cyclonedds idl/SensorsTelemetry.idl
idlc -l c   -Wno-implicit-extensibility -o generated/cyclonedds-c idl/SensorsTelemetry.idl   # opcional (API C)
```

`scripts/generate_types.sh` los ejecuta y comprueba qué tipos son *plain*/*fixed-size* (aptos para
zero-copy). Para compilar en un nodo sin Java: `cmake -DBENCH_FASTDDS_PREGENERATED=generated/fastdds`.

## Tipos IDL

| Tipo | Clave (instancia) | Tamaño CDR | Zero-copy | Contenido |
|---|---|---|---|---|
| `RadarTrack` | `track_id` | 236 B | Fast DDS: sí · Cyclone: sí | posición/velocidad ENU, covarianzas 3×3 (triángulo superior), SNR, RCS, calidad, estado, identidad STANAG, entorno |
| `SonarPing` | `transducer_id` | 140 B + N (N ≤ 1024) | no (secuencia) | modo (pasivo/CW/LFM/batimetría), frecuencia, ancho de banda, ganancia, `sequence<octet,1024>` de muestras crudas |
| `WeatherStation` | `station_id` | 136 B | Fast DDS: sí · Cyclone: sí | presión, temperatura, punto de rocío, humedad, viento medio y ráfaga, visibilidad, precipitación, flags de calibración |
| `EchoReply` | — | 72 B | sí | cabecera devuelta + T2/T3 del suscriptor (RTT y offset) |

Todos los tipos son `@final` y ordenan sus miembros por alineación (8 → 4 → 2 → 1 bytes), de modo que
el layout C++ coincide con el CDR: `fastddsgen` los marca como *plain* (`is_plain_xcdrv1/2_impl`).
Cada muestra lleva un `BenchHeader` de 48 B (secuencia, T1 REALTIME y MONOTONIC, offset estimado,
flags), que es la base de todas las métricas.

## Uso

### En un solo host (validación, SHM / zero-copy)

```bash
scripts/run_local.sh fastdds    udp  --duration 30            # UDP unicast por loopback
scripts/run_local.sh cyclonedds udpm --duration 30            # UDP multicast
scripts/run_local.sh fastdds    zc   --duration 30 --deadline-ms 0   # SHM + Data-Sharing + loans
scripts/run_local.sh cyclonedds zc   --duration 30 --deadline-ms 0   # PSMX iceoryx (arranca RouDi)
```

### Manualmente en el testbed

```bash
# Data Reader (CPU 2 para la aplicación, 3 para el middleware, 0 para métricas)
sudo /opt/dds_bench/bin/cyclonedds_subscriber --iface enp3s0 --run-id prueba1 \
     --cpus 2 --mw-cpus 3 --hk-cpus 0 --cpu-dma-latency 0
# Data Writer
sudo /opt/dds_bench/bin/cyclonedds_publisher  --iface enp3s0 --run-id prueba1 \
     --cpus 2 --mw-cpus 3 --hk-cpus 0 --cpu-dma-latency 0 --duration 120 \
     --streams radar,sonar,weather --reliability reliable --deadline-ms radar=20
```

### Experimento con ruido orquestado y matriz completa

```bash
vi config/testbed.env                      # hosts SSH, IPs, interfaces, CPUs
sudo scripts/tune_host_rt.sh show  --iface enp3s0      # en Data Writer y Data Reader
sudo scripts/tune_host_rt.sh apply --iface enp3s0 --irq-cpus 3
sudo scripts/tune_host_rt.sh baseline --cpus 2,3        # cyclictest: suelo de jitter del SO

# Un experimento: baseline 30 s → ruido UDP desde las 3 fuentes hacia el Data Reader 60 s → recuperación 30 s
scripts/run_benchmark.sh --mw fastdds --transport udp --scenario reliable \
     --noise udp_saturate --noise-target reader -- --reliability reliable

# Toda la matriz (config/matrix.env): escenarios QoS × transportes × perfiles de ruido × middleware × repeticiones
scripts/run_matrix.sh --dry-run            # plan y duración estimada
scripts/run_matrix.sh                      # → results/matrix-FECHA/{runs,phases,comparison}.csv, report.md
```

Perfiles de ruido (`scripts/noise.sh`): `udp_saturate` (flujo UDP no reactivo a tasa de línea →
pérdidas por tail-drop en la cola del puerto), `tcp_bulk` (flujos TCP → *bufferbloat*), `udp_burst`
(microrráfagas de 200 ms/s), `mixed`. Objetivos: `reader` (incast 3:1 en el puerto del Data Reader),
`writer` (afecta al camino de eco/ACKNACK), `both`, `cross` (tráfico que no comparte puertos con DDS,
como control).

## QoS: configuración en XML y en C++

Las **variables del experimento** se fijan en C++ (`QosSpec` → API de cada middleware) a partir de
presets por sensor y de la línea de comandos; los **XML** fijan transporte, hilos, buffers,
descubrimiento, temporizadores del protocolo fiable y política de memoria. Con `--qos-profile` el
perfil XML pasa a ser la base y el CLI sólo sobrescribe lo indicado explícitamente
(Fast DDS: `<data_writer/data_reader profile_name>`; Cyclone: QoS Provider OMG DDS-XML
`bench_qos::<perfil>`). Los perfiles `radar_rt`, `radar_be`, `sonar_rt`, `weather_rt` existen en ambos
formatos con valores idénticos.

| Preset | Instancias × frecuencia | Agregado | Historia | ResourceLimits (samples/instances/per-inst.) | DEADLINE | TRANSPORT_PRIORITY → DSCP |
|---|---|---|---|---|---|---|
| radar | 16 pistas × 100 Hz | 1600 msg/s | KEEP_LAST 8 | 512 / 64 / 8 | 20 ms | 50 → EF (46) |
| sonar | 4 transductores × 50 Hz | 200 msg/s | KEEP_LAST 4 | 32 / 8 / 4 | 50 ms | 30 → AF41 (34) |
| weather | 4 estaciones × 1 Hz | 4 msg/s | KEEP_LAST 1 | 8 / 8 / 1 | 1.5 s | 10 → AF21 (18) |

Comunes: RELIABLE (`max_blocking_time` 100 ms), VOLATILE, LATENCY_BUDGET 0, LIVELINESS AUTOMATIC
(lease 1 s). Todo se puede cambiar globalmente o por flujo:

```bash
--reliability best_effort --history keep_all --depth 16 --max-samples 4096 \
--deadline-ms radar=10,sonar=40 --latency-budget-us 500 --liveliness manual_topic --lease-ms 200 \
--transport-priority radar=50 --data-rep xcdr2 --fastdds-memory-policy preallocated
```

**Preasignación estricta:** con `ResourceLimits` finitos, Fast DDS además reserva
`allocated_samples = max_samples` y la política de historia `PREALLOCATED` (perfiles `*_rt`); el
arnés bloquea la memoria (`mlockall`), desactiva la devolución de memoria de glibc y pre-toca heap y
pilas antes de medir.

### Transportes

| `--transport` | Fast DDS | Cyclone DDS |
|---|---|---|
| `udp` | UDPv4 en la interfaz `--iface`, Data-Sharing OFF | `cyclonedds.xml`: datos unicast, multicast sólo SPDP |
| `udpm` | locator multicast `--mcast-addr` (239.255.0.2) en los DataReaders de datos | overlay `cyclonedds_multicast.xml`: partición de red + coste multicast < unicast |
| `shm` | transporte SHM (con serialización) | overlay `cyclonedds_shm.xml`: PSMX iceoryx (RouDi) |
| `zc` | SHM + Data-Sharing + `loan_sample()` (tipos *plain*) | PSMX iceoryx + `dds_request_loan()` (tipos *fixed-size*) |

### TRANSPORT_PRIORITY → DSCP/TOS

Verificado en el código fuente de ambas versiones: **ninguno aplica TRANSPORT_PRIORITY a DSCP**
(Fast DDS 3.6.2 recibe el valor en `UDPTransportInterface::send()` pero lo ignora; su
`<udp_priority_mappings>` sólo existe en Fast DDS Pro. En Cyclone 11 `m_diffserv` es siempre 0 y
TRANSPORT_PRIORITY sólo decide la entrega síncrona en el hilo de recepción). El arnés lo materializa:
tras crear las entidades marca todos los sockets UDP del proceso con `IP_TOS` y `SO_PRIORITY`
según la tabla (≤0 BE · 1-9 AF11 · 10-19 AF21 · 20-29 AF31 · 30-39 AF41 · 40-49 CS5 · ≥50 EF),
o con `--dscp EF|AF41|<0-63>|off`. El marcado sólo tiene efecto en un switch gestionable que confíe en
DSCP; en el propio host, `tune_host_rt.sh apply` instala una qdisc `prio` que pone `SO_PRIORITY 6`
(EF) en la banda de máxima prioridad, por delante del ruido.

## Métricas y ficheros de salida

Cada proceso escribe en `<out-dir>/<run-id>/`:

| Fichero | Contenido |
|---|---|
| `*_timeseries.csv` | una fila por flujo y ventana de 1 s: msg/s, MB/s, pérdidas, E2E y E2E corregida (media, σ, p50/p95/p99/p99.9/máx), RTT y RTT neto, offset θ, duración de `write()`, latencia de despertar, deadline incumplidos y tasa, SAMPLE_LOST/REJECTED, liveliness, CPU, RSS, cambios de contexto, page faults, errores UDP del host y descartes de la NIC, fase |
| `*_summary.csv` | resumen de la ejecución con percentiles exactos (muestras crudas) |
| `*_phases.csv` | métricas por fase (baseline / noise / recovery) con percentiles reales por fase |
| `*_summary.prom` | el resumen en formato de texto Prometheus (exportable) |
| `*_threads.csv` | hilos del proceso con política, prioridad, CPUs y tiempo de CPU |
| `*_meta.json` | configuración, versión del middleware, kernel, estado NTP |

`analyze_results.py` une publicador y suscriptor y genera `runs.csv` (pérdida extremo a extremo),
`phases.csv` (degradación frente a baseline), `comparison.csv` (mediana y rango sobre repeticiones),
`report.md` y `aggregate.prom`; con `--plots`, gráficas de la serie temporal por fase.
Opcionalmente los binarios sirven las métricas de ventana en formato Prometheus con
`--prom-port PUERTO` o `--prom-textfile FICHERO`.

## Hallazgos de la validación

Todo el sistema se compiló y ejecutó en un host de desarrollo (VM de 4 vCPU, kernel **sin**
PREEMPT_RT, loopback). Las cifras absolutas **no** son representativas del testbed; sí lo son los
comportamientos funcionales, verificados en ambos middlewares (ver
[`docs/METODOLOGIA.md`](docs/METODOLOGIA.md#hallazgos-de-la-validación-local) para el detalle):

* Todos los transportes (`udp`, `udpm`, `shm`, `zc`) y los perfiles XML funcionan en ambos; el grupo
  multicast de datos lo une exactamente el DataReader en ambos.
* Con un 2 % de descarte inyectado (nftables), BEST_EFFORT mide 2.07 % (Fast DDS) / 1.83 % (Cyclone) de
  pérdida y RELIABLE recupera el 100 %.
* **Fast DDS RELIABLE con pérdidas:** la reparación depende de `heartbeat_period`. Con HB 20 ms la E2E
  p99 sube a 24.6 ms (1088 deadline incumplidos); con HB 5 ms baja a 6.6 ms. Cyclone, con el mismo HB,
  se mantuvo en 1.7 ms.
* **Cyclone + iceoryx** rechaza endpoints con DEADLINE finito (o liveliness no AUTOMATIC) y cae a UDP
  loopback sin error; el arnés lo avisa. Los escenarios SHM de la matriz usan `--deadline-ms 0` en
  ambos middlewares.
* Con PSMX/iceoryx activo, Cyclone redujo la CPU del suscriptor de ~15 % a ~11 % y la E2E p50 de 170 a
  118 µs frente a UDP loopback.

## Limitaciones conocidas

* La E2E bruta depende de la sincronía NTP; la E2E corregida y el RTT no (ver metodología). Si las NIC
  tienen timestamping hardware, PTP (`ptp4l`/`phc2sys`) da sincronía sub-µs.
* El marcado DSCP es por proceso (los writers de un participante comparten sockets en ambos
  middlewares).
* Fast DDS registra "Could not set next deadline in the history" al terminar: la instancia se purga al
  desaparecer el writer y su temporizador de deadline ya no la encuentra. Es benigno.
* En loopback el ruido `iperf3` no reproduce la contención de la cola de un puerto del switch: la
  degradación bajo ruido sólo es significativa en el testbed físico.
