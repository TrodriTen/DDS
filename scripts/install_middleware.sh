#!/usr/bin/env bash
# =============================================================================
# install_middleware.sh — Compila e instala desde fuentes las versiones exactas
# de los middlewares usadas por el benchmark, en un prefijo aislado.
#
#   eProsima Fast DDS 3.6.2  (+ Fast-CDR 2.3.6, foonathan_memory_vendor 1.4.1)
#   eProsima Fast-DDS-Gen 4.3.0 (fastddsgen, requiere JDK >= 11)
#   Eclipse Cyclone DDS 11.0.1 (C) + cyclonedds-cxx 11.0.1 (C++ ISO/IEC PSM)
#   Eclipse iceoryx 2.0.6 (opcional, SHM zero-copy para Cyclone vía PSMX)
#
# Uso:
#   sudo ./scripts/install_middleware.sh [--prefix /opt/dds] [--with-iceoryx]
#                                         [--jobs N] [--only fastdds|cyclone]
#
# Tras instalar:
#   source /opt/dds/setup.bash
# =============================================================================
set -euo pipefail

PREFIX=/opt/dds
JOBS=$(nproc)
WITH_ICEORYX=0
ONLY=all
SRC_DIR=${SRC_DIR:-/tmp/dds-src}
BUILD_TYPE=Release

FOONATHAN_VER=v1.4.1
FASTCDR_VER=v2.3.6
FASTDDS_VER=v3.6.2
FASTDDSGEN_VER=v4.3.0
CYCLONE_VER=11.0.1
CYCLONE_CXX_VER=11.0.1
ICEORYX_VER=v2.0.6

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --with-iceoryx) WITH_ICEORYX=1; shift ;;
    --only) ONLY="$2"; shift 2 ;;
    --src) SRC_DIR="$2"; shift 2 ;;
    -h|--help) sed -n 2,20p "$0"; exit 0 ;;
    *) echo "Opción desconocida: $1" >&2; exit 1 ;;
  esac
done

log() { printf '\n\033[1;34m[install]\033[0m %s\n' "$*"; }

mkdir -p "$SRC_DIR" "$PREFIX"
export CMAKE_PREFIX_PATH="$PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"

clone() { # clone <url> <tag> <dir>
  local url=$1 tag=$2 dir=$3
  if [[ ! -d "$SRC_DIR/$dir" ]]; then
    git clone --depth 1 --recurse-submodules --shallow-submodules -b "$tag" "$url" "$SRC_DIR/$dir"
  fi
}

cmake_install() { # cmake_install <dir> [cmake args...]
  local dir=$1; shift
  cmake -S "$SRC_DIR/$dir" -B "$SRC_DIR/$dir/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_PREFIX_PATH="$PREFIX" "$@"
  cmake --build "$SRC_DIR/$dir/build" -j "$JOBS"
  cmake --install "$SRC_DIR/$dir/build"
}

if command -v apt-get >/dev/null && [[ ${SKIP_APT:-0} != 1 ]]; then
  log "Dependencias del sistema (apt)"
  DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git libasio-dev libtinyxml2-dev libssl-dev \
    libacl1-dev libncurses-dev openjdk-17-jdk-headless python3 iperf3 rt-tests \
    ethtool nftables >/dev/null || true
fi

# ----------------------------------------------------------------- Fast DDS --
if [[ $ONLY == all || $ONLY == fastdds ]]; then
  log "foonathan_memory_vendor $FOONATHAN_VER"
  clone https://github.com/eProsima/foonathan_memory_vendor.git $FOONATHAN_VER foonathan_memory_vendor
  cmake_install foonathan_memory_vendor -DBUILD_SHARED_LIBS=ON

  log "Fast-CDR $FASTCDR_VER"
  clone https://github.com/eProsima/Fast-CDR.git $FASTCDR_VER Fast-CDR
  cmake_install Fast-CDR

  log "Fast DDS $FASTDDS_VER (SHM habilitado, sin DDS-Security)"
  clone https://github.com/eProsima/Fast-DDS.git $FASTDDS_VER Fast-DDS
  cmake_install Fast-DDS -DSECURITY=OFF -DSHM_TRANSPORT_DEFAULT=ON \
                -DCOMPILE_TOOLS=ON -DCOMPILE_EXAMPLES=OFF -DBUILD_TESTING=OFF

  log "Fast-DDS-Gen $FASTDDSGEN_VER"
  clone https://github.com/eProsima/Fast-DDS-Gen.git $FASTDDSGEN_VER Fast-DDS-Gen
  # Si Maven Central responde 429 (rate limit), definir un mirror en
  # ~/.gradle/init.d/ (p.ej. maven-central.storage-download.googleapis.com).
  ( cd "$SRC_DIR/Fast-DDS-Gen" && ./gradlew --no-daemon -q assemble )
  install -d "$PREFIX/share/fastddsgen/java" "$PREFIX/bin"
  install -m 0644 "$SRC_DIR/Fast-DDS-Gen/share/fastddsgen/java/fastddsgen.jar" "$PREFIX/share/fastddsgen/java/"
  cat > "$PREFIX/bin/fastddsgen" <<EOF
#!/bin/sh
exec java -jar "$PREFIX/share/fastddsgen/java/fastddsgen.jar" "\$@"
EOF
  chmod +x "$PREFIX/bin/fastddsgen"
fi

# ------------------------------------------------------------- Cyclone DDS --
if [[ $ONLY == all || $ONLY == cyclone ]]; then
  ICEORYX_FLAG=-DENABLE_ICEORYX=OFF
  if [[ $WITH_ICEORYX == 1 ]]; then
    log "iceoryx $ICEORYX_VER (RouDi + posh) para PSMX de Cyclone"
    clone https://github.com/eclipse-iceoryx/iceoryx.git $ICEORYX_VER iceoryx
    cmake -S "$SRC_DIR/iceoryx/iceoryx_meta" -B "$SRC_DIR/iceoryx/build" -G Ninja \
          -DCMAKE_BUILD_TYPE=$BUILD_TYPE -DCMAKE_INSTALL_PREFIX="$PREFIX" \
          -DBUILD_SHARED_LIBS=ON -DROUDI_ENVIRONMENT=OFF -DTOML_CONFIG=ON
    cmake --build "$SRC_DIR/iceoryx/build" -j "$JOBS"
    cmake --install "$SRC_DIR/iceoryx/build"
    ICEORYX_FLAG=-DENABLE_ICEORYX=ON
  fi

  log "Cyclone DDS $CYCLONE_VER"
  clone https://github.com/eclipse-cyclonedds/cyclonedds.git $CYCLONE_VER cyclonedds
  cmake_install cyclonedds -DENABLE_SECURITY=OFF -DENABLE_ICEORYX2=OFF $ICEORYX_FLAG \
                -DBUILD_DDSPERF=ON -DBUILD_IDLC=ON -DENABLE_LTO=ON

  log "cyclonedds-cxx $CYCLONE_CXX_VER"
  clone https://github.com/eclipse-cyclonedds/cyclonedds-cxx.git $CYCLONE_CXX_VER cyclonedds-cxx
  cmake_install cyclonedds-cxx -DENABLE_ICEORYX=$([[ $WITH_ICEORYX == 1 ]] && echo ON || echo OFF) \
                -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF
fi

# ------------------------------------------------------------- setup.bash ---
cat > "$PREFIX/setup.bash" <<EOF
# Entorno del benchmark DDS (generado por install_middleware.sh)
export DDS_PREFIX="$PREFIX"
export PATH="$PREFIX/bin:\$PATH"
export LD_LIBRARY_PATH="$PREFIX/lib:$PREFIX/lib64\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export CMAKE_PREFIX_PATH="$PREFIX\${CMAKE_PREFIX_PATH:+:\$CMAKE_PREFIX_PATH}"
EOF

log "Instalación completada en $PREFIX — ejecuta: source $PREFIX/setup.bash"
