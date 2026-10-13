# =============================================================================
#  DDS RT Benchmark — Fast DDS vs Cyclone DDS (C++20, Linux PREEMPT_RT)
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(dds_rt_bench VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(BENCH_WITH_FASTDDS    "Compilar binarios Fast DDS"    ON)
option(BENCH_WITH_CYCLONEDDS "Compilar binarios Cyclone DDS" ON)
option(BENCH_BUILD_TESTS     "Compilar tests unitarios del núcleo" ON)
option(BENCH_NATIVE          "Optimizar para la CPU local (-march=native)" OFF)
# Si fastddsgen no está disponible en el nodo de compilación (p.ej. sin Java),
# se puede apuntar a un directorio con el código ya generado (scripts/generate_types.sh).
set(BENCH_FASTDDS_PREGENERATED "" CACHE PATH "Directorio con código fastddsgen pre-generado")

set(BENCH_IDL ${CMAKE_CURRENT_SOURCE_DIR}/idl/SensorsTelemetry.idl)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(BENCH_NATIVE)
  add_compile_options(-march=native)
endif()

find_package(Threads REQUIRED)

# ------------------------------------------------------------ núcleo común ---
add_library(bench_core STATIC
  src/core/config.cpp
  src/core/dscp.cpp
  src/core/harness_common.cpp
  src/core/metrics.cpp
  src/core/proc_stats.cpp
  src/core/rt.cpp)
target_include_directories(bench_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(bench_core PUBLIC Threads::Threads)
set_target_properties(bench_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(BENCH_TARGETS "")

# ------------------------------------------------------------------ Fast DDS --
if(BENCH_WITH_FASTDDS)
  find_package(fastcdr 2 REQUIRED)
  find_package(fastdds 3 REQUIRED)

  set(FASTDDS_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/fastdds)
  set(FASTDDS_GEN_SOURCES
    ${FASTDDS_GEN_DIR}/SensorsTelemetryPubSubTypes.cxx
    ${FASTDDS_GEN_DIR}/SensorsTelemetryTypeObjectSupport.cxx)

  if(BENCH_FASTDDS_PREGENERATED)
    file(MAKE_DIRECTORY ${FASTDDS_GEN_DIR})
    file(GLOB _pregen ${BENCH_FASTDDS_PREGENERATED}/*)
    file(COPY ${_pregen} DESTINATION ${FASTDDS_GEN_DIR})
  else()
    find_program(FASTDDSGEN_EXECUTABLE fastddsgen HINTS ${fastdds_DIR}/../../bin ENV PATH REQUIRED)
    add_custom_command(
      OUTPUT ${FASTDDS_GEN_SOURCES}
             ${FASTDDS_GEN_DIR}/SensorsTelemetry.hpp
             ${FASTDDS_GEN_DIR}/SensorsTelemetryPubSubTypes.hpp
      COMMAND ${CMAKE_COMMAND} -E make_directory ${FASTDDS_GEN_DIR}
      COMMAND ${FASTDDSGEN_EXECUTABLE} -replace -cs -flat-output-dir -d ${FASTDDS_GEN_DIR} ${BENCH_IDL}
      DEPENDS ${BENCH_IDL}
      COMMENT "fastddsgen: generando tipos Fast DDS desde SensorsTelemetry.idl"
      VERBATIM)
  endif()

  add_library(bench_types_fastdds STATIC ${FASTDDS_GEN_SOURCES})
  target_include_directories(bench_types_fastdds PUBLIC ${FASTDDS_GEN_DIR})
  target_link_libraries(bench_types_fastdds PUBLIC fastdds fastcdr)
  # El código generado no está pensado para -Wextra
  target_compile_options(bench_types_fastdds PRIVATE -w)

  foreach(role publisher subscriber)
    add_executable(fastdds_${role} src/fastdds/${role}_main.cpp)
    target_include_directories(fastdds_${role} PRIVATE src/fastdds)
    target_link_libraries(fastdds_${role} PRIVATE bench_core bench_types_fastdds)
    list(APPEND BENCH_TARGETS fastdds_${role})
  endforeach()
endif()

# --------------------------------------------------------------- Cyclone DDS --
if(BENCH_WITH_CYCLONEDDS)
  find_package(CycloneDDS REQUIRED)
  find_package(CycloneDDS-CXX REQUIRED)

  # idlc -l cxx (plugin de cyclonedds-cxx) ejecutado en tiempo de compilación
  idlcxx_generate(TARGET bench_types_cyclone
                  FILES ${BENCH_IDL}
                  WARNINGS no-implicit-extensibility
                  OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/cyclonedds)

  foreach(role publisher subscriber)
    add_executable(cyclonedds_${role} src/cyclonedds/${role}_main.cpp)
    target_include_directories(cyclonedds_${role} PRIVATE src/cyclonedds)
    target_link_libraries(cyclonedds_${role} PRIVATE bench_core bench_types_cyclone CycloneDDS-CXX::ddscxx CycloneDDS::ddsc)
    list(APPEND BENCH_TARGETS cyclonedds_${role})
  endforeach()
endif()

# -------------------------------------------------------------------- tests --
if(BENCH_BUILD_TESTS)
  enable_testing()
  add_executable(bench_core_tests tests/core_tests.cpp)
  target_link_libraries(bench_core_tests PRIVATE bench_core)
  add_test(NAME bench_core_tests COMMAND bench_core_tests)
endif()

# ------------------------------------------------------------------ install --
include(GNUInstallDirs)
install(TARGETS ${BENCH_TARGETS} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY config/ DESTINATION ${CMAKE_INSTALL_DATADIR}/dds_bench/config)
install(DIRECTORY scripts/ DESTINATION ${CMAKE_INSTALL_DATADIR}/dds_bench/scripts USE_SOURCE_PERMISSIONS)
install(FILES ${BENCH_IDL} DESTINATION ${CMAKE_INSTALL_DATADIR}/dds_bench/idl)

message(STATUS "Binarios: ${BENCH_TARGETS}")
