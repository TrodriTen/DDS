#!/usr/bin/env bash
# =============================================================================
# generate_types.sh — Generación de código a partir de idl/SensorsTelemetry.idl
#
# CMake ya ejecuta ambos generadores en tiempo de compilación; este script deja
# los comandos EXACTOS a la vista y permite pre-generar el código (p.ej. para
# compilar en nodos sin Java: cmake -DBENCH_FASTDDS_PREGENERATED=generated/fastdds).
#
# Uso: scripts/generate_types.sh [directorio_salida]   (por defecto ./generated)
# =============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IDL="$ROOT/idl/SensorsTelemetry.idl"
OUT=${1:-$ROOT/generated}
PREFIX=${DDS_PREFIX:-/opt/dds}
export PATH="$PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}"

mkdir -p "$OUT/fastdds" "$OUT/cyclonedds" "$OUT/cyclonedds-c"

echo "== eProsima Fast DDS-Gen $(fastddsgen -version 2>/dev/null | grep -o '[0-9.]*$')"
#   -replace : sobrescribe ficheros existentes
#   -cs      : identificadores IDL sensibles a mayúsculas
#   -flat-output-dir : no replica la ruta relativa del IDL bajo -d
#   -d       : directorio de salida
# Genera: SensorsTelemetry.hpp (tipos), *PubSubTypes.{hpp,cxx} (TopicDataType:
# serialización CDR, claves, is_plain/is_bounded), *CdrAux.{hpp,ipp} y
# *TypeObjectSupport.{hpp,cxx} (XTypes TypeObject para descubrimiento de tipos).
set -x
fastddsgen -replace -cs -flat-output-dir -d "$OUT/fastdds" "$IDL"
set +x

echo "== Eclipse Cyclone DDS idlc $(idlc -v 2>/dev/null | head -1)"
#   -l cxx : backend C++ (libcycloneddsidlcxx de cyclonedds-cxx) -> ISO C++ PSM
#   -Wno-implicit-extensibility : todos los tipos llevan @final explícito
#   -o     : directorio de salida
# Genera: SensorsTelemetry.hpp/.cpp con clases, TopicTraits (isSelfContained,
# getTypeName, TypeInformation XTypes) y los ops de serialización.
set -x
idlc -l cxx -Wno-implicit-extensibility -o "$OUT/cyclonedds" "$IDL"
# (Opcional) binding C nativo de Cyclone, útil para ddsperf o pruebas en C:
idlc -l c -Wno-implicit-extensibility -o "$OUT/cyclonedds-c" "$IDL"
set +x

echo
echo "== Verificación de tipos 'plain' (aptos para zero-copy)"
# Fast DDS: is_plain_xcdrv{1,2}_impl compara el offset del último miembro con
# el tamaño CDR; si coinciden, el layout C++ == CDR y se admiten loans.
grep -A4 "static constexpr bool is_plain_xcdrv2_impl" "$OUT/fastdds/SensorsTelemetryPubSubTypes.hpp" \
  | grep -E "return|==" | sed 's/^ */   /' | head -12
# Cyclone: isSelfContained() == false sólo para tipos con secuencias/strings
grep -B1 -A3 "isSelfContained()" "$OUT/cyclonedds/SensorsTelemetry.hpp" | grep -E "TopicTraits|return" | sed 's/^ */   /'
echo "Código generado en: $OUT"
