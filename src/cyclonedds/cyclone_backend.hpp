// SPDX-License-Identifier: Apache-2.0
// Backend Eclipse Cyclone DDS 11.x (cyclonedds-cxx, ISO/IEC C++ PSM).
//
// Fuentes de configuración:
//   1. cyclonedds.xml (--cyclone-xml): dominio (interfaces, multicast, hilos,
//      buffers, WHC, PSMX/iceoryx). Se pasa como config del DomainParticipant.
//   2. cyclonedds_qos.xml (--cyclone-qos-xml + --qos-profile): QoS Provider
//      OMG DDS-XML 1.0 como base de DataWriterQos/DataReaderQos.
//   3. QosSpec (presets + CLI): variables del experimento.
//
// Camino crítico: write() y waitset usan la API C (dds_write, dds_waitset_wait)
// sobre las entidades C++ (mismo sertype): sin excepciones en el hilo RT.
#pragma once

#include <dds/dds.h>
#include <dds/ddsc/dds_public_loan_api.h>
#include <dds/version.h>

#include <dds/dds.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "SensorsTelemetry.hpp"
#include "bench/config.hpp"
#include "bench/harness_common.hpp"
#include "bench/window.hpp"

namespace bench {

class CycloneBackend {
 public:
  static dds::core::Duration duration(int64_t ns, bool zero_is_infinite = true) {
    if (ns <= 0) return zero_is_infinite ? dds::core::Duration::infinite() : dds::core::Duration::zero();
    return dds::core::Duration(static_cast<int64_t>(ns / 1'000'000'000), static_cast<uint32_t>(ns % 1'000'000'000));
  }

  template <class Q>
  static void apply_common(Q& q, const QosSpec& s) {
    namespace P = dds::core::policy;
    if (s.reliability || s.max_blocking_ns) {
      P::Reliability rel = q.template policy<P::Reliability>();
      if (s.reliability) {
        rel.kind(*s.reliability == Reliability::Reliable ? P::ReliabilityKind::RELIABLE : P::ReliabilityKind::BEST_EFFORT);
      }
      if (s.max_blocking_ns) rel.max_blocking_time(duration(*s.max_blocking_ns, false));
      q << rel;
    }
    if (s.history || s.depth) {
      P::History h = q.template policy<P::History>();
      if (s.history) h.kind(*s.history == HistoryKind::KeepLast ? P::HistoryKind::KEEP_LAST : P::HistoryKind::KEEP_ALL);
      if (s.depth) h.depth(*s.depth);
      q << h;
    }
    if (s.max_samples || s.max_instances || s.max_samples_per_instance) {
      P::ResourceLimits rl = q.template policy<P::ResourceLimits>();
      if (s.max_samples) rl.max_samples(*s.max_samples);
      if (s.max_instances) rl.max_instances(*s.max_instances);
      if (s.max_samples_per_instance) rl.max_samples_per_instance(*s.max_samples_per_instance);
      q << rl;
    }
    if (s.deadline_ns) q << P::Deadline(duration(*s.deadline_ns));
    if (s.latency_budget_ns) q << P::LatencyBudget(duration(*s.latency_budget_ns, false));
    if (s.liveliness || s.lease_ns) {
      P::Liveliness l = q.template policy<P::Liveliness>();
      if (s.liveliness) {
        switch (*s.liveliness) {
          case LivelinessKind::Automatic: l.kind(P::LivelinessKind::AUTOMATIC); break;
          case LivelinessKind::ManualByParticipant: l.kind(P::LivelinessKind::MANUAL_BY_PARTICIPANT); break;
          case LivelinessKind::ManualByTopic: l.kind(P::LivelinessKind::MANUAL_BY_TOPIC); break;
        }
      }
      if (s.lease_ns) l.lease_duration(duration(*s.lease_ns));
      q << l;
    }
    q << P::Durability::Volatile();
    if (s.data_rep && *s.data_rep != DataRep::Default) {
      q << P::DataRepresentation({*s.data_rep == DataRep::Xcdr2 ? P::DataRepresentationId::XCDR2
                                                                : P::DataRepresentationId::XCDR1});
    }
  }

  // -------------------------------------------------------------- listeners
  template <class T>
  class WriterListener final : public dds::pub::NoOpDataWriterListener<T> {
   public:
    explicit WriterListener(WriterStatusCounters* c) : c_(c) {}
    void on_offered_deadline_missed(dds::pub::DataWriter<T>&, const dds::core::status::OfferedDeadlineMissedStatus& s) override {
      c_->offered_deadline_missed.store(static_cast<uint64_t>(s.total_count()));
    }
    void on_offered_incompatible_qos(dds::pub::DataWriter<T>&, const dds::core::status::OfferedIncompatibleQosStatus& s) override {
      c_->offered_incompatible_qos.store(static_cast<uint64_t>(s.total_count()));
    }
    void on_liveliness_lost(dds::pub::DataWriter<T>&, const dds::core::status::LivelinessLostStatus& s) override {
      c_->liveliness_lost.store(static_cast<uint64_t>(s.total_count()));
    }
    void on_publication_matched(dds::pub::DataWriter<T>&, const dds::core::status::PublicationMatchedStatus& s) override {
      c_->matched.store(s.current_count());
    }

   private:
    WriterStatusCounters* c_;
  };

  template <class T>
  class ReaderListener final : public dds::sub::NoOpDataReaderListener<T> {
   public:
    explicit ReaderListener(ReaderStatusCounters* c) : c_(c) {}
    void on_requested_deadline_missed(dds::sub::DataReader<T>&, const dds::core::status::RequestedDeadlineMissedStatus& s) override {
      c_->requested_deadline_missed.store(static_cast<uint64_t>(s.total_count()));
    }
    void on_requested_incompatible_qos(dds::sub::DataReader<T>&, const dds::core::status::RequestedIncompatibleQosStatus& s) override {
      c_->requested_incompatible_qos.store(static_cast<uint64_t>(s.total_count()));
    }
    void on_sample_rejected(dds::sub::DataReader<T>&, const dds::core::status::SampleRejectedStatus& s) override {
      c_->sample_rejected.store(static_cast<uint64_t>(s.total_count()));
    }
    void on_liveliness_changed(dds::sub::DataReader<T>&, const dds::core::status::LivelinessChangedStatus& s) override {
      if (s.alive_count_change() > 0) c_->liveliness_alive_changes.fetch_add(static_cast<uint64_t>(s.alive_count_change()));
      if (s.not_alive_count_change() > 0) {
        c_->liveliness_not_alive_changes.fetch_add(static_cast<uint64_t>(s.not_alive_count_change()));
      }
      c_->liveliness_alive.store(s.alive_count());
    }
    void on_subscription_matched(dds::sub::DataReader<T>&, const dds::core::status::SubscriptionMatchedStatus& s) override {
      c_->matched.store(s.current_count());
      int32_t peak = c_->matched_peak.load();
      while (s.current_count() > peak && !c_->matched_peak.compare_exchange_weak(peak, s.current_count())) {
      }
    }
    void on_sample_lost(dds::sub::DataReader<T>&, const dds::core::status::SampleLostStatus& s) override {
      c_->sample_lost.store(static_cast<uint64_t>(s.total_count()));
    }

   private:
    ReaderStatusCounters* c_;
  };

  // ---------------------------------------------------------------- Writer
  template <class T>
  class Writer {
   public:
    Writer(std::unique_ptr<WriterListener<T>> l, dds::pub::DataWriter<T> w)
        : listener_(std::move(l)), w_(std::move(w)), entity_(w_.delegate()->get_ddsc_entity()) {
      shm_ = dds_is_shared_memory_available(entity_);
      // Loans sólo para tipos auto-contenidos (sin punteros): un std::vector
      // construido en memoria prestada no se destruiría al devolver el chunk.
      loans_ = shm_ && org::eclipse::cyclonedds::topic::TopicTraits<T>::isSelfContained();
    }
    ~Writer() {
      try {
        w_.listener(nullptr, dds::core::status::StatusMask::none());
        w_.close();
      } catch (...) {
      }
    }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    bool write(const T& s) noexcept { return dds_write(entity_, &s) == DDS_RETCODE_OK; }
    // Préstamo de PSMX (iceoryx) para tipos de tamaño fijo: zero-copy.
    T* loan() noexcept {
      void* p = nullptr;
      if (dds_request_loan(entity_, &p) != DDS_RETCODE_OK || p == nullptr) return nullptr;
      return new (p) T();
    }
    bool write_loaned(T* s) noexcept {
      if (dds_write(entity_, s) == DDS_RETCODE_OK) return true;
      void* p = s;
      dds_return_loan(entity_, &p, 1);
      return false;
    }
    [[nodiscard]] bool loans_supported() const noexcept { return loans_; }
    [[nodiscard]] bool shared_memory() const noexcept { return shm_; }
    [[nodiscard]] int32_t matched() const {
      dds_publication_matched_status_t st{};
      dds_get_publication_matched_status(entity_, &st);
      return static_cast<int32_t>(st.current_count);
    }
    void flush(int64_t timeout_ns) { dds_wait_for_acks(entity_, timeout_ns); }

   private:
    std::unique_ptr<WriterListener<T>> listener_;
    dds::pub::DataWriter<T> w_;
    dds_entity_t entity_;
    bool shm_ = false;
    bool loans_ = false;
  };

  // ---------------------------------------------------------------- Reader
  template <class T>
  class Reader {
   public:
    Reader(std::unique_ptr<ReaderListener<T>> l, dds::sub::DataReader<T> r, size_t batch)
        : listener_(std::move(l)), r_(std::move(r)), entity_(r_.delegate()->get_ddsc_entity()), buf_(batch) {}
    ~Reader() {
      try {
        r_.listener(nullptr, dds::core::status::StatusMask::none());
        r_.close();
      } catch (...) {
      }
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // take() sobre un búfer de muestras preasignado (deserialización in situ).
    template <class F>
    size_t take(F&& f, int32_t max_batch) noexcept {
      size_t total = 0;
      const auto max = static_cast<uint32_t>(std::min<size_t>(buf_.size(), static_cast<size_t>(max_batch)));
      try {
        for (;;) {
          const uint32_t n = r_.take(buf_.begin(), max);
          for (uint32_t i = 0; i < n; ++i) {
            if (buf_[i].info().valid()) {
              f(buf_[i].data());
              ++total;
            }
          }
          if (n < max) break;
        }
      } catch (...) {
      }
      return total;
    }
    [[nodiscard]] int32_t matched() const {
      dds_subscription_matched_status_t st{};
      dds_get_subscription_matched_status(entity_, &st);
      return static_cast<int32_t>(st.current_count);
    }
    [[nodiscard]] dds_entity_t entity() const noexcept { return entity_; }

   private:
    std::unique_ptr<ReaderListener<T>> listener_;
    dds::sub::DataReader<T> r_;
    dds_entity_t entity_;
    std::vector<dds::sub::Sample<T>> buf_;
  };

  // --------------------------------------------------------------- WaitSet
  class WaitSet {
   public:
    explicit WaitSet(dds_entity_t participant) : ws_(dds_create_waitset(participant)) {
      if (ws_ < 0) throw std::runtime_error("dds_create_waitset falló");
    }
    ~WaitSet() {
      for (dds_entity_t c : conds_) {
        dds_waitset_detach(ws_, c);
        dds_delete(c);
      }
      dds_delete(ws_);
    }
    template <class T>
    void attach(Reader<T>& r) {
      const dds_entity_t rc = dds_create_readcondition(r.entity(), DDS_ANY_STATE);
      if (rc < 0 || dds_waitset_attach(ws_, rc, static_cast<dds_attach_t>(rc)) != DDS_RETCODE_OK) {
        throw std::runtime_error("dds_waitset_attach falló");
      }
      conds_.push_back(rc);
    }
    bool wait(int64_t timeout_ns) noexcept {
      dds_attach_t xs[8];
      return dds_waitset_wait(ws_, xs, 8, timeout_ns) > 0;
    }

   private:
    dds_entity_t ws_;
    std::vector<dds_entity_t> conds_;
  };

  // ---------------------------------------------------------- participante
  explicit CycloneBackend(const BenchConfig& cfg)
      : cfg_(cfg), participant_(make_participant(cfg)), publisher_(participant_), subscriber_(participant_) {
    entity_ = participant_.delegate()->get_ddsc_entity();
  }

  [[nodiscard]] std::string version() const {
    std::string files = cfg_.cyclone_xml.empty() ? std::string("config por defecto")
                                                  : std::filesystem::path(cfg_.cyclone_xml).filename().string();
    if (!cfg_.cyclone_xml_overlay.empty()) files += "+" + std::filesystem::path(cfg_.cyclone_xml_overlay).filename().string();
    return std::string("Cyclone DDS ") + DDS_VERSION + " (" + files + ")";
  }

  template <class T>
  std::unique_ptr<Writer<T>> create_writer(const StreamSpec& spec, const std::string& topic_name, const QosSpec& qos,
                                           const std::string& profile, WriterStatusCounters* counters) {
    (void)spec;
    dds::topic::Topic<T> topic = get_topic<T>(topic_name);
    dds::pub::qos::DataWriterQos wq = publisher_.default_datawriter_qos();
    if (!profile.empty()) wq = qos_provider(profile).datawriter_qos();
    apply_common(wq, qos);
    if (qos.transport_priority) wq << dds::core::policy::TransportPriority(*qos.transport_priority);
    auto listener = std::make_unique<WriterListener<T>>(counters);
    const auto mask = dds::core::status::StatusMask::offered_deadline_missed() |
                      dds::core::status::StatusMask::offered_incompatible_qos() |
                      dds::core::status::StatusMask::liveliness_lost() |
                      dds::core::status::StatusMask::publication_matched();
    dds::pub::DataWriter<T> w(publisher_, topic, wq, listener.get(), mask);
    auto writer = std::make_unique<Writer<T>>(std::move(listener), std::move(w));
    if ((cfg_.transport == "shm" || cfg_.transport == "zc") && !writer->shared_memory()) {
      // psmx_iox (iox_type_qos_supported) rechaza DEADLINE finito, LIVELINESS
      // no AUTOMATIC, durabilidad persistente o particiones con comodines.
      log_warn("Cyclone PSMX/iceoryx no aplicado en '" + topic_name +
               "': requiere DEADLINE infinito (--deadline-ms 0), LIVELINESS AUTOMATIC y RouDi en marcha; "
               "se usa UDP loopback");
    }
    return writer;
  }

  template <class T>
  std::unique_ptr<Reader<T>> create_reader(const StreamSpec& spec, const std::string& topic_name, const QosSpec& qos,
                                           const std::string& profile, ReaderStatusCounters* counters) {
    (void)spec;
    dds::topic::Topic<T> topic = get_topic<T>(topic_name);
    dds::sub::qos::DataReaderQos rq = subscriber_.default_datareader_qos();
    if (!profile.empty()) rq = qos_provider(profile).datareader_qos();
    apply_common(rq, qos);
    auto listener = std::make_unique<ReaderListener<T>>(counters);
    const auto mask = dds::core::status::StatusMask::requested_deadline_missed() |
                      dds::core::status::StatusMask::requested_incompatible_qos() |
                      dds::core::status::StatusMask::sample_rejected() |
                      dds::core::status::StatusMask::liveliness_changed() |
                      dds::core::status::StatusMask::subscription_matched() |
                      dds::core::status::StatusMask::sample_lost();
    dds::sub::DataReader<T> r(subscriber_, topic, rq, listener.get(), mask);
    return std::make_unique<Reader<T>>(std::move(listener), std::move(r), 64);
  }

  std::unique_ptr<WaitSet> create_waitset() { return std::make_unique<WaitSet>(entity_); }

 private:
  static dds::domain::DomainParticipant make_participant(const BenchConfig& cfg) {
    // Lista de URIs: base + overlay del transporte (Cyclone fusiona en orden).
    std::string config;
    if (!cfg.cyclone_xml.empty()) config = "file://" + std::filesystem::absolute(cfg.cyclone_xml).string();
    if (!cfg.cyclone_xml_overlay.empty()) {
      config += (config.empty() ? "" : ",") + std::string("file://") +
                std::filesystem::absolute(cfg.cyclone_xml_overlay).string();
    }
    return dds::domain::DomainParticipant(static_cast<uint32_t>(cfg.domain_id),
                                          dds::domain::DomainParticipant::default_participant_qos(), nullptr,
                                          dds::core::status::StatusMask::none(), config);
  }

  dds::core::QosProvider qos_provider(const std::string& profile) {
    if (cfg_.cyclone_qos_xml.empty()) throw std::runtime_error("--qos-profile requiere --cyclone-qos-xml");
    const std::string full = profile.find("::") == std::string::npos ? "bench_qos::" + profile : profile;
    return dds::core::QosProvider("file://" + std::filesystem::absolute(cfg_.cyclone_qos_xml).string(), full);
  }

  template <class T>
  dds::topic::Topic<T> get_topic(const std::string& name) {
    auto it = topics_.find(name);
    if (it != topics_.end()) return *std::static_pointer_cast<dds::topic::Topic<T>>(it->second);
    auto t = std::make_shared<dds::topic::Topic<T>>(participant_, name);
    topics_.emplace(name, t);
    return *t;
  }

  const BenchConfig& cfg_;
  dds::domain::DomainParticipant participant_;
  dds::pub::Publisher publisher_;
  dds::sub::Subscriber subscriber_;
  std::map<std::string, std::shared_ptr<void>> topics_;  // destruidos antes que el participante
  dds_entity_t entity_ = 0;
};

}  // namespace bench
