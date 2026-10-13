// SPDX-License-Identifier: Apache-2.0
// Generadores de datos sintéticos realistas para los tipos de SensorsTelemetry.idl.
//
// Son plantillas sobre el tipo generado: el mismo código rellena las clases
// producidas por fastddsgen (Fast DDS) y por idlc -l cxx (Cyclone DDS), ya que
// ambos generadores exponen accesores idénticos (x(v), header(), samples()...).
//
// Garantías de tiempo real: todo el estado se reserva en el constructor; fill()
// no asigna memoria (la secuencia sonar usa capacidad reservada de antemano).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <type_traits>
#include <vector>

namespace bench {

// xoshiro256** (determinista y reproducible con --seed)
class Rng {
 public:
  explicit Rng(uint64_t seed) noexcept {
    for (auto& w : s_) {
      seed += 0x9E3779B97F4A7C15ULL;
      uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
      w = z ^ (z >> 31);
    }
  }
  uint64_t next() noexcept {
    const uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }
  double uniform() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
  double uniform(double a, double b) noexcept { return a + (b - a) * uniform(); }
  uint32_t below(uint32_t n) noexcept { return static_cast<uint32_t>(uniform() * n) % (n ? n : 1); }
  double normal() noexcept {  // Box-Muller (sin caché, determinista)
    const double u1 = std::max(uniform(), 1e-300);
    const double u2 = uniform();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
  }

 private:
  static uint64_t rotl(uint64_t x, int k) noexcept { return (x << k) | (x >> (64 - k)); }
  std::array<uint64_t, 4> s_{};
};

template <class E>
inline void set_enum(E& field, int v) noexcept {
  field = static_cast<E>(v);
}

// Tamaños CDR (cuerpo + 4 bytes de encapsulación) para el cálculo de throughput.
inline constexpr uint32_t kRadarCdrBytes = 236 + 4;
inline constexpr uint32_t kWeatherCdrBytes = 136 + 4;
inline constexpr uint32_t kSonarCdrFixedBytes = 136 + 4 + 4;  // + N muestras
inline constexpr uint32_t kEchoCdrBytes = 72 + 4;

// ---------------------------------------------------------------- Radar ----
class RadarGenerator {
 public:
  // Índices de los enumerados del IDL
  enum Status { kTentative = 0, kConfirmed = 1, kCoasting = 2, kLost = 3 };
  enum Env { kAir = 0, kSurface = 1, kSubsurface = 2, kLand = 3, kSpace = 4 };

  RadarGenerator(uint32_t tracks, uint32_t sensor_id, uint64_t seed) : rng_(seed), sensor_id_(sensor_id) {
    tracks_.resize(tracks);
    for (uint32_t i = 0; i < tracks; ++i) init_track(tracks_[i], 1000 + i);
  }

  [[nodiscard]] uint32_t instances() const noexcept { return static_cast<uint32_t>(tracks_.size()); }

  template <class T>
  uint32_t fill(T& s, uint32_t idx, int64_t now_rt_ns) noexcept {
    Track& t = tracks_[idx % tracks_.size()];
    const double dt = t.last_ns ? std::clamp(static_cast<double>(now_rt_ns - t.last_ns) * 1e-9, 0.0, 1.0) : 0.0;
    t.last_ns = now_rt_ns;
    propagate(t, dt);

    const double range = std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
    const double sigma_p = 10.0 + 2e-4 * range;  // error de medida creciente con el alcance
    const double sigma_v = t.status == kTentative ? 8.0 : 2.0;

    s.timestamp_ns(now_rt_ns);
    s.x(t.x + sigma_p * rng_.normal());
    s.y(t.y + sigma_p * rng_.normal());
    s.z(t.z + 0.5 * sigma_p * rng_.normal());
    s.vx(t.vx + sigma_v * rng_.normal());
    s.vy(t.vy + sigma_v * rng_.normal());
    s.vz(t.vz + 0.2 * sigma_v * rng_.normal());
    // Triángulo superior [xx, xy, xz, yy, yz, zz] con correlación débil
    const double vp = sigma_p * sigma_p;
    const double vv = sigma_v * sigma_v;
    auto& pc = s.position_covariance();
    pc[0] = vp; pc[1] = 0.1 * vp; pc[2] = 0.02 * vp; pc[3] = vp; pc[4] = 0.02 * vp; pc[5] = 0.25 * vp;
    auto& vc = s.velocity_covariance();
    vc[0] = vv; vc[1] = 0.1 * vv; vc[2] = 0.0;      vc[3] = vv; vc[4] = 0.0;      vc[5] = 0.04 * vv;

    s.track_id(t.id);
    s.sensor_id(sensor_id_);
    const double snr = 28.0 - 40.0 * std::log10(std::max(range, 500.0) / 20000.0) + t.rcs_dbsm * 0.5 +
                       1.5 * rng_.normal();
    s.snr_db(static_cast<float>(std::clamp(snr, -5.0, 60.0)));
    s.rcs_dbsm(static_cast<float>(t.rcs_dbsm + 0.8 * rng_.normal()));
    const double q = static_cast<double>(t.hits) / (t.hits + 2.0 * t.misses + 3.0);
    s.track_quality(static_cast<float>(std::clamp(q, 0.0, 1.0)));
    set_enum(s.status(), t.status);
    set_enum(s.identity(), t.identity);
    set_enum(s.environment(), t.env);
    s.hit_count(t.hits);
    s.miss_count(t.misses);
    return kRadarCdrBytes;
  }

 private:
  struct Track {
    uint32_t id = 0;
    double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    double turn_rate = 0;  // rad/s (giro coordinado)
    double rcs_dbsm = 0;
    int status = kTentative, identity = 0, env = kAir, coast_left = 0;
    uint16_t hits = 0, misses = 0;
    int64_t last_ns = 0;
  };

  void init_track(Track& t, uint32_t id) {
    t.id = id;
    const double u = rng_.uniform();
    t.env = u < 0.6 ? kAir : u < 0.9 ? kSurface : u < 0.95 ? kSubsurface : kLand;
    const double bearing = rng_.uniform(0, 2 * std::numbers::pi);
    const double heading = rng_.uniform(0, 2 * std::numbers::pi);
    double range = 0, speed = 0;
    switch (t.env) {
      case kAir: range = rng_.uniform(5e3, 80e3); speed = rng_.uniform(80, 300); t.z = rng_.uniform(150, 11000);
                 t.rcs_dbsm = rng_.uniform(-10, 15); break;
      case kSurface: range = rng_.uniform(2e3, 30e3); speed = rng_.uniform(3, 15); t.z = 0;
                     t.rcs_dbsm = rng_.uniform(20, 45); break;
      case kSubsurface: range = rng_.uniform(1e3, 10e3); speed = rng_.uniform(2, 8); t.z = -rng_.uniform(30, 300);
                        t.rcs_dbsm = rng_.uniform(-20, 0); break;
      default: range = rng_.uniform(1e3, 20e3); speed = rng_.uniform(0, 20); t.z = rng_.uniform(0, 300);
               t.rcs_dbsm = rng_.uniform(0, 20); break;
    }
    t.x = range * std::sin(bearing);
    t.y = range * std::cos(bearing);
    t.vx = speed * std::sin(heading);
    t.vy = speed * std::cos(heading);
    t.vz = 0;
    // Identidades: 10% pending, 25% unknown, 5% assumed friend, 25% friend, 20% neutral, 5% suspect, 10% hostile
    const double iu = rng_.uniform();
    t.identity = iu < 0.10 ? 0 : iu < 0.35 ? 1 : iu < 0.40 ? 2 : iu < 0.65 ? 3 : iu < 0.85 ? 4 : iu < 0.90 ? 5 : 6;
  }

  void propagate(Track& t, double dt) noexcept {
    // Maniobras ocasionales (giro coordinado) para aeronaves
    if (t.env == kAir && rng_.uniform() < 0.002) t.turn_rate = rng_.uniform(-0.05, 0.05);
    if (rng_.uniform() < 0.001) t.turn_rate = 0;
    const double w = t.turn_rate * dt;
    if (w != 0.0) {
      const double c = std::cos(w), sn = std::sin(w);
      const double vx = t.vx * c - t.vy * sn;
      t.vy = t.vx * sn + t.vy * c;
      t.vx = vx;
    }
    t.x += t.vx * dt;
    t.y += t.vy * dt;
    t.z += t.vz * dt;
    // Mantener la pista en el volumen de vigilancia (rebote en 100 km)
    if (t.x * t.x + t.y * t.y > 1e10) {
      t.vx = -t.vx;
      t.vy = -t.vy;
    }
    // Ciclo de vida: tentativa -> confirmada; pérdidas de detección (coasting)
    if (t.coast_left > 0) {
      --t.coast_left;
      ++t.misses;
      t.status = kCoasting;
      if (t.coast_left == 0) t.status = kConfirmed;
    } else {
      ++t.hits;
      if (t.status == kTentative && t.hits >= 3) t.status = kConfirmed;
      if (t.status == kConfirmed && rng_.uniform() < 0.002) t.coast_left = 5;
    }
  }

  Rng rng_;
  uint32_t sensor_id_;
  std::vector<Track> tracks_;
};

// ---------------------------------------------------------------- Sonar ----
class SonarGenerator {
 public:
  enum Mode { kPassive = 0, kActiveCw = 1, kActiveLfm = 2, kBathymetric = 3 };
  static constexpr size_t kWaveforms = 8;
  static constexpr size_t kMaxSamples = 1024;

  SonarGenerator(uint32_t transducers, uint32_t min_bytes, uint32_t max_bytes, uint64_t seed)
      : rng_(seed), min_bytes_(min_bytes), max_bytes_(std::max(min_bytes, max_bytes)) {
    tx_.resize(transducers);
    for (uint32_t i = 0; i < transducers; ++i) {
      auto& t = tx_[i];
      t.id = 200 + i;
      t.mode = static_cast<int>(i % 4 == 0 ? kBathymetric : i % 4 == 1 ? kActiveLfm : i % 4 == 2 ? kActiveCw : kPassive);
      t.lat = 36.45 + 0.001 * i;
      t.lon = -6.30 - 0.001 * i;
      t.depth = rng_.uniform(4, 40);
      t.bottom_m = rng_.uniform(60, 600);
      t.azimuth = static_cast<float>(i * (360.0 / transducers));
    }
    // Banco de envolventes precalculadas: ruido + reverberación + eco de fondo + blancos
    for (size_t w = 0; w < kWaveforms; ++w) {
      auto& wf = waveforms_[w];
      const size_t bottom = 300 + 60 * w;
      for (size_t k = 0; k < kMaxSamples; ++k) {
        double a = 18.0 + 6.0 * rng_.uniform();                         // ruido de fondo
        a += 140.0 * std::exp(-static_cast<double>(k) / 40.0);          // reverberación de superficie
        a += 200.0 * std::exp(-std::pow((static_cast<double>(k) - bottom) / 6.0, 2));  // fondo
        if (k == 150 + 37 * w) a += 120.0;                              // blanco puntual
        wf[k] = static_cast<uint8_t>(std::clamp(a, 0.0, 255.0));
      }
    }
  }

  [[nodiscard]] uint32_t instances() const noexcept { return static_cast<uint32_t>(tx_.size()); }

  // Reserva la capacidad máxima de la secuencia (sin realloc en fill()).
  template <class T>
  static void prepare(T& s) {
    s.samples().reserve(kMaxSamples);
  }

  template <class T>
  uint32_t fill(T& s, uint32_t idx, int64_t now_rt_ns) noexcept {
    auto& t = tx_[idx % tx_.size()];
    ++t.ping;
    t.depth = std::clamp(t.depth + 0.05 * rng_.normal(), 2.0, 60.0);
    s.timestamp_ns(now_rt_ns);
    s.latitude_deg(t.lat + 1e-6 * rng_.normal());
    s.longitude_deg(t.lon + 1e-6 * rng_.normal());
    s.depth_m(t.depth);
    s.sound_speed_mps(1507.0 + 2.0 * rng_.normal());
    s.transducer_id(t.id);
    s.ping_number(t.ping);
    switch (t.mode) {
      case kBathymetric: s.center_frequency_hz(12000.f); s.bandwidth_hz(1500.f); s.pulse_length_s(0.002f); break;
      case kActiveLfm: s.center_frequency_hz(3500.f); s.bandwidth_hz(1000.f); s.pulse_length_s(0.25f); break;
      case kActiveCw: s.center_frequency_hz(7500.f); s.bandwidth_hz(100.f); s.pulse_length_s(0.05f); break;
      default: s.center_frequency_hz(2000.f); s.bandwidth_hz(4000.f); s.pulse_length_s(0.f); break;
    }
    s.sample_rate_hz(s.bandwidth_hz() * 2.5f);
    s.source_level_db(t.mode == kPassive ? 0.f : 215.f);
    s.receiver_gain_db(static_cast<float>(30.0 + 2.0 * rng_.normal()));
    s.beam_azimuth_deg(t.azimuth);
    s.beam_elevation_deg(-45.f);
    set_enum(s.mode(), t.mode);
    s.bits_per_sample(8);
    s.channel_count(1);
    const uint32_t n = min_bytes_ + (max_bytes_ > min_bytes_ ? rng_.below(max_bytes_ - min_bytes_ + 1) : 0);
    auto& v = s.samples();
    v.resize(n);  // dentro de la capacidad reservada: sin asignación
    std::memcpy(v.data(), waveforms_[t.ping % kWaveforms].data(), n);
    return kSonarCdrFixedBytes + n;
  }

 private:
  struct Transducer {
    uint32_t id = 0, ping = 0;
    int mode = 0;
    double lat = 0, lon = 0, depth = 0, bottom_m = 0;
    float azimuth = 0;
  };
  Rng rng_;
  uint32_t min_bytes_, max_bytes_;
  std::vector<Transducer> tx_;
  std::array<std::array<uint8_t, kMaxSamples>, kWaveforms> waveforms_{};
};

// -------------------------------------------------------------- Weather ----
class WeatherGenerator {
 public:
  WeatherGenerator(uint32_t stations, uint64_t seed) : rng_(seed) {
    st_.resize(stations);
    for (uint32_t i = 0; i < stations; ++i) {
      auto& s = st_[i];
      s.id = 300 + i;
      s.lat = 36.53 + 0.02 * i;
      s.lon = -6.29 + 0.015 * i;
      s.alt = 5.0 + 12.0 * i;
      s.pressure = 1013.0 + rng_.uniform(-6, 6);
      s.temp_offset = rng_.uniform(-1.5, 1.5);
      s.wind_dir = rng_.uniform(0, 360);
      s.wind = rng_.uniform(2, 9);
      s.cal_due = (i == 3);  // una estación con calibración caducada
    }
  }

  [[nodiscard]] uint32_t instances() const noexcept { return static_cast<uint32_t>(st_.size()); }

  template <class T>
  uint32_t fill(T& m, uint32_t idx, int64_t now_rt_ns) noexcept {
    auto& s = st_[idx % st_.size()];
    const double day_frac = std::fmod(static_cast<double>(now_rt_ns) * 1e-9, 86400.0) / 86400.0;
    const double temp = 17.0 + s.temp_offset + 6.0 * std::sin(2 * std::numbers::pi * (day_frac - 0.375)) +
                        0.2 * rng_.normal();
    s.pressure = std::clamp(s.pressure + 0.02 * rng_.normal(), 960.0, 1050.0);
    s.wind = std::clamp(s.wind + 0.4 * rng_.normal(), 0.0, 45.0);
    s.wind_dir = std::fmod(s.wind_dir + 4.0 * rng_.normal() + 360.0, 360.0);
    const double rh = std::clamp(78.0 - 2.2 * (temp - 15.0) + 3.0 * rng_.normal(), 5.0, 100.0);
    // Punto de rocío (aproximación de Magnus)
    const double a = 17.62, b = 243.12;
    const double gamma = std::log(rh / 100.0) + a * temp / (b + temp);
    const double dew = b * gamma / (a - gamma);
    const double gust = s.wind * rng_.uniform(1.25, 1.8);
    const bool rain = rh > 92.0 && rng_.uniform() < 0.3;

    m.timestamp_ns(now_rt_ns);
    m.last_calibration_ns(now_rt_ns - static_cast<int64_t>((s.cal_due ? 400.0 : 20.0) * 86400.0 * 1e9));
    m.latitude_deg(s.lat);
    m.longitude_deg(s.lon);
    m.altitude_m(s.alt);
    m.station_id(s.id);
    m.pressure_hpa(static_cast<float>(s.pressure));
    m.temperature_c(static_cast<float>(temp));
    m.dew_point_c(static_cast<float>(dew));
    m.relative_humidity_pct(static_cast<float>(rh));
    m.wind_direction_deg(static_cast<float>(s.wind_dir));
    m.wind_speed_mps(static_cast<float>(s.wind));
    m.wind_gust_mps(static_cast<float>(gust));
    m.wind_gust_direction_deg(static_cast<float>(std::fmod(s.wind_dir + 10.0 * rng_.normal() + 360.0, 360.0)));
    m.visibility_m(static_cast<float>(rh > 95.0 ? rng_.uniform(800, 4000) : 10000.0));
    m.precipitation_rate_mmph(static_cast<float>(rain ? rng_.uniform(0.2, 6.0) : 0.0));
    // Bits CAL_* del IDL
    uint32_t flags = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40;  // sensores OK + GNSS lock
    if (temp < 3.0) flags |= 0x80;                                // calefactor anemómetro
    if (s.cal_due) flags |= 0x100;
    if (rng_.uniform() < 1e-4) flags = (flags & ~0x8u) | 0x80000000u;  // fallo esporádico de anemómetro
    m.calibration_flags(flags);
    return kWeatherCdrBytes;
  }

 private:
  struct Station {
    uint32_t id = 0;
    double lat = 0, lon = 0, alt = 0, pressure = 0, temp_offset = 0, wind_dir = 0, wind = 0;
    bool cal_due = false;
  };
  Rng rng_;
  std::vector<Station> st_;
};

}  // namespace bench
