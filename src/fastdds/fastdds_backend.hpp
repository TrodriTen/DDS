// SPDX-License-Identifier: Apache-2.0
// Backend eProsima Fast DDS 3.x para el motor genérico del benchmark.
//
// Fuentes de configuración (por orden de aplicación):
//   1. fastdds_profiles.xml: participante (transportes, hilos, buffers,
//      descubrimiento) y, opcionalmente, perfiles data_writer/data_reader.
//   2. QosSpec (presets del arnés + CLI): variables del experimento.
//   3. --transport: fuerza data-sharing OFF (udp/udpm/shm) o AUTOMATIC (zc) y
//      añade el locator multicast a los DataReaders de datos (udpm).
#pragma once

#include <fastdds/dds/core/LoanableSequence.hpp>
#include <fastdds/dds/core/condition/WaitSet.hpp>
#include <fastdds/dds/core/status/StatusMask.hpp>
#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/domain/qos/DomainParticipantQos.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/DataWriterListener.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/DataReaderListener.hpp>
#include <fastdds/dds/subscriber/ReadCondition.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#include <fastdds/dds/subscriber/qos/DataReaderQos.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>
#include <fastdds/rtps/common/Locator.hpp>
#include <fastdds/rtps/transport/UDPv4TransportDescriptor.hpp>
#include <fastdds/rtps/transport/network/AllowedNetworkInterface.hpp>
#include <fastdds/utils/IPLocator.hpp>
#include <fastdds/config.hpp>

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "SensorsTelemetryPubSubTypes.hpp"
#include "bench/config.hpp"
#include "bench/harness_common.hpp"
#include "bench/window.hpp"

namespace bench {

namespace fdds = eprosima::fastdds::dds;
namespace frtps = eprosima::fastdds::rtps;

template <class T>
struct FastPubSubType;
template <>
struct FastPubSubType<SensorsTelemetry::RadarTrack> {
  using type = SensorsTelemetry::RadarTrackPubSubType;
};
template <>
struct FastPubSubType<SensorsTelemetry::SonarPing> {
  using type = SensorsTelemetry::SonarPingPubSubType;
};
template <>
struct FastPubSubType<SensorsTelemetry::WeatherStation> {
  using type = SensorsTelemetry::WeatherStationPubSubType;
};
template <>
struct FastPubSubType<SensorsTelemetry::EchoReply> {
  using type = SensorsTelemetry::EchoReplyPubSubType;
};

class FastDdsBackend {
 public:
  // ------------------------------------------------------------------ util
  static fdds::Duration_t duration(int64_t ns, bool zero_is_infinite = true) {
    if (ns <= 0) return zero_is_infinite ? fdds::c_TimeInfinite : fdds::Duration_t(0, 0);
    return fdds::Duration_t(static_cast<int32_t>(ns / 1'000'000'000), static_cast<uint32_t>(ns % 1'000'000'000));
  }

  // Aplica las políticas comunes a DataWriterQos y DataReaderQos.
  template <class Q>
  static void apply_common(Q& q, const QosSpec& s) {
    if (s.reliability) {
      q.reliability().kind = *s.reliability == Reliability::Reliable ? fdds::RELIABLE_RELIABILITY_QOS
                                                                      : fdds::BEST_EFFORT_RELIABILITY_QOS;
    }
    if (s.max_blocking_ns) q.reliability().max_blocking_time = duration(*s.max_blocking_ns, false);
    if (s.history) {
      q.history().kind = *s.history == HistoryKind::KeepLast ? fdds::KEEP_LAST_HISTORY_QOS : fdds::KEEP_ALL_HISTORY_QOS;
    }
    if (s.depth) q.history().depth = *s.depth;
    if (s.max_samples) q.resource_limits().max_samples = *s.max_samples;
    if (s.max_instances) q.resource_limits().max_instances = *s.max_instances;
    if (s.max_samples_per_instance) q.resource_limits().max_samples_per_instance = *s.max_samples_per_instance;
    // Preasignación estricta: reservar desde el inicio todo lo que permiten los límites
    if (s.max_samples && *s.max_samples > 0) q.resource_limits().allocated_samples = *s.max_samples;
    if (s.deadline_ns) q.deadline().period = duration(*s.deadline_ns);
    if (s.latency_budget_ns) q.latency_budget().duration = duration(*s.latency_budget_ns, false);
    if (s.liveliness) {
      switch (*s.liveliness) {
        case LivelinessKind::Automatic: q.liveliness().kind = fdds::AUTOMATIC_LIVELINESS_QOS; break;
        case LivelinessKind::ManualByParticipant: q.liveliness().kind = fdds::MANUAL_BY_PARTICIPANT_LIVELINESS_QOS; break;
        case LivelinessKind::ManualByTopic: q.liveliness().kind = fdds::MANUAL_BY_TOPIC_LIVELINESS_QOS; break;
      }
    }
    if (s.lease_ns) {
      q.liveliness().lease_duration = duration(*s.lease_ns);
      if (*s.lease_ns > 0) q.liveliness().announcement_period = duration(*s.lease_ns / 2);
    }
    q.durability().kind = fdds::VOLATILE_DURABILITY_QOS;
    if (s.data_rep && *s.data_rep != DataRep::Default) {
      q.representation().m_value.clear();
      q.representation().m_value.push_back(*s.data_rep == DataRep::Xcdr2 ? fdds::XCDR2_DATA_REPRESENTATION
                                                                          : fdds::XCDR_DATA_REPRESENTATION);
    }
  }

  template <class Q>
  void apply_deployment(Q& q) const {
    // Data-sharing (zero-copy entre procesos del mismo host) sólo en 'zc'
    if (cfg_.transport == "zc") {
      q.data_sharing().automatic();
    } else {
      q.data_sharing().off();
    }
    const std::string& mp = cfg_.fastdds_memory_policy;
    if (mp == "preallocated") q.endpoint().history_memory_policy = frtps::PREALLOCATED_MEMORY_MODE;
    else if (mp == "preallocated_realloc") q.endpoint().history_memory_policy = frtps::PREALLOCATED_WITH_REALLOC_MEMORY_MODE;
    else if (mp == "dynamic") q.endpoint().history_memory_policy = frtps::DYNAMIC_RESERVE_MEMORY_MODE;
    else if (mp == "dynamic_reusable") q.endpoint().history_memory_policy = frtps::DYNAMIC_REUSABLE_MEMORY_MODE;
  }

  // -------------------------------------------------------------- listeners
  class WriterListener final : public fdds::DataWriterListener {
   public:
    explicit WriterListener(WriterStatusCounters* c) : c_(c) {}
    void on_publication_matched(fdds::DataWriter*, const fdds::PublicationMatchedStatus& s) override {
      c_->matched.store(s.current_count);
    }
    void on_offered_deadline_missed(fdds::DataWriter*, const fdds::OfferedDeadlineMissedStatus& s) override {
      c_->offered_deadline_missed.store(static_cast<uint64_t>(s.total_count));
    }
    void on_offered_incompatible_qos(fdds::DataWriter*, const fdds::OfferedIncompatibleQosStatus& s) override {
      c_->offered_incompatible_qos.store(static_cast<uint64_t>(s.total_count));
    }
    void on_liveliness_lost(fdds::DataWriter*, const fdds::LivelinessLostStatus& s) override {
      c_->liveliness_lost.store(static_cast<uint64_t>(s.total_count));
    }

   private:
    WriterStatusCounters* c_;
  };

  class ReaderListener final : public fdds::DataReaderListener {
   public:
    explicit ReaderListener(ReaderStatusCounters* c) : c_(c) {}
    void on_subscription_matched(fdds::DataReader*, const fdds::SubscriptionMatchedStatus& s) override {
      c_->matched.store(s.current_count);
      int32_t peak = c_->matched_peak.load();
      while (s.current_count > peak && !c_->matched_peak.compare_exchange_weak(peak, s.current_count)) {
      }
    }
    void on_requested_deadline_missed(fdds::DataReader*, const fdds::RequestedDeadlineMissedStatus& s) override {
      c_->requested_deadline_missed.store(static_cast<uint64_t>(s.total_count));
    }
    void on_liveliness_changed(fdds::DataReader*, const fdds::LivelinessChangedStatus& s) override {
      if (s.alive_count_change > 0) c_->liveliness_alive_changes.fetch_add(static_cast<uint64_t>(s.alive_count_change));
      if (s.not_alive_count_change > 0) {
        c_->liveliness_not_alive_changes.fetch_add(static_cast<uint64_t>(s.not_alive_count_change));
      }
      c_->liveliness_alive.store(s.alive_count);
    }
    void on_sample_rejected(fdds::DataReader*, const fdds::SampleRejectedStatus& s) override {
      c_->sample_rejected.store(static_cast<uint64_t>(s.total_count));
    }
    void on_requested_incompatible_qos(fdds::DataReader*, const fdds::RequestedIncompatibleQosStatus& s) override {
      c_->requested_incompatible_qos.store(static_cast<uint64_t>(s.total_count));
    }
    void on_sample_lost(fdds::DataReader*, const fdds::SampleLostStatus& s) override {
      c_->sample_lost.store(static_cast<uint64_t>(s.total_count));
    }

   private:
    ReaderStatusCounters* c_;
  };

  // ---------------------------------------------------------------- Writer
  template <class T>
  class Writer {
   public:
    Writer(fdds::Publisher* pub, fdds::DataWriter* w, std::unique_ptr<WriterListener> l, bool plain)
        : pub_(pub), w_(w), listener_(std::move(l)), plain_(plain) {}
    ~Writer() {
      if (w_ != nullptr) pub_->delete_datawriter(w_);
    }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    bool write(const T& s) noexcept { return w_->write(&s) == fdds::RETCODE_OK; }
    T* loan() noexcept {
      void* p = nullptr;
      return w_->loan_sample(p, fdds::DataWriter::LoanInitializationKind::NO_LOAN_INITIALIZATION) == fdds::RETCODE_OK
                 ? static_cast<T*>(p)
                 : nullptr;
    }
    bool write_loaned(T* s) noexcept {
      if (w_->write(s) == fdds::RETCODE_OK) return true;
      void* p = s;
      w_->discard_loan(p);
      return false;
    }
    [[nodiscard]] bool loans_supported() const noexcept { return plain_; }
    [[nodiscard]] int32_t matched() const {
      fdds::PublicationMatchedStatus st;
      w_->get_publication_matched_status(st);
      return st.current_count;
    }
    void flush(int64_t timeout_ns) { w_->wait_for_acknowledgments(duration(timeout_ns, false)); }
    [[nodiscard]] fdds::DataWriter* native() const noexcept { return w_; }

   private:
    fdds::Publisher* pub_;
    fdds::DataWriter* w_;
    std::unique_ptr<WriterListener> listener_;
    bool plain_;
  };

  // ---------------------------------------------------------------- Reader
  template <class T>
  class Reader {
   public:
    Reader(fdds::Subscriber* sub, fdds::DataReader* r, std::unique_ptr<ReaderListener> l)
        : sub_(sub), r_(r), listener_(std::move(l)) {}
    ~Reader() {
      if (r_ != nullptr) {
        r_->delete_contained_entities();
        sub_->delete_datareader(r_);
      }
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    // Drena el lector: take() con préstamo (loan) del pool interno, sin copias
    // adicionales ni asignaciones en régimen permanente.
    template <class F>
    size_t take(F&& f, int32_t max_batch) noexcept {
      size_t n = 0;
      while (r_->take(data_, infos_, max_batch) == fdds::RETCODE_OK) {
        for (fdds::LoanableCollection::size_type i = 0; i < infos_.length(); ++i) {
          if (infos_[i].valid_data) {
            f(data_[i]);
            ++n;
          }
        }
        r_->return_loan(data_, infos_);
      }
      return n;
    }
    [[nodiscard]] int32_t matched() const {
      fdds::SubscriptionMatchedStatus st;
      r_->get_subscription_matched_status(st);
      return st.current_count;
    }
    [[nodiscard]] fdds::DataReader* native() const noexcept { return r_; }

   private:
    fdds::Subscriber* sub_;
    fdds::DataReader* r_;
    std::unique_ptr<ReaderListener> listener_;
    fdds::LoanableSequence<T> data_;
    fdds::SampleInfoSeq infos_;
  };

  // --------------------------------------------------------------- WaitSet
  class WaitSet {
   public:
    ~WaitSet() {
      for (auto& [reader, cond] : conds_) {
        ws_.detach_condition(*cond);
        reader->delete_readcondition(cond);
      }
    }
    template <class T>
    void attach(Reader<T>& r) {
      fdds::ReadCondition* c =
          r.native()->create_readcondition(fdds::ANY_SAMPLE_STATE, fdds::ANY_VIEW_STATE, fdds::ANY_INSTANCE_STATE);
      if (c == nullptr) throw std::runtime_error("create_readcondition falló");
      ws_.attach_condition(*c);
      conds_.emplace_back(r.native(), c);
    }
    bool wait(int64_t timeout_ns) noexcept { return ws_.wait(active_, duration(timeout_ns, false)) == fdds::RETCODE_OK; }

   private:
    fdds::WaitSet ws_;
    fdds::ConditionSeq active_;
    std::vector<std::pair<fdds::DataReader*, fdds::ReadCondition*>> conds_;
  };

  // ---------------------------------------------------------- participante
  explicit FastDdsBackend(const BenchConfig& cfg) : cfg_(cfg) {
    auto* factory = fdds::DomainParticipantFactory::get_instance();
    bool xml_loaded = false;
    if (!cfg.fastdds_xml.empty()) {
      if (factory->load_XML_profiles_file(cfg.fastdds_xml) != fdds::RETCODE_OK) {
        throw std::runtime_error("no se pudo cargar " + cfg.fastdds_xml);
      }
      xml_loaded = true;
    }
    fdds::DomainParticipantQos pqos = factory->get_default_participant_qos();
    if (xml_loaded) {
      if (factory->get_participant_qos_from_profile(cfg.participant_profile, pqos) != fdds::RETCODE_OK) {
        throw std::runtime_error("perfil de participante '" + cfg.participant_profile + "' no encontrado en " + cfg.fastdds_xml);
      }
    }
    // Fast DDS sólo expande ${VAR} en el texto de los elementos, no en atributos
    // (<interface name="..."/>): la allowlist de la interfaz de pruebas se fija aquí.
    if (!cfg.iface.empty() && cfg.iface != "any") {
      for (auto& t : pqos.transport().user_transports) {
        if (auto udp = std::dynamic_pointer_cast<frtps::UDPv4TransportDescriptor>(t)) {
          udp->interface_allowlist.clear();
          udp->interface_allowlist.emplace_back(cfg.iface);
        }
      }
    }
    pqos.name("bench_" + cfg.role + "_n" + std::to_string(cfg.node_id));
    participant_ = factory->create_participant(static_cast<fdds::DomainId_t>(cfg.domain_id), pqos);
    if (participant_ == nullptr) throw std::runtime_error("create_participant falló");
    publisher_ = participant_->create_publisher(fdds::PUBLISHER_QOS_DEFAULT);
    subscriber_ = participant_->create_subscriber(fdds::SUBSCRIBER_QOS_DEFAULT);
    if (publisher_ == nullptr || subscriber_ == nullptr) throw std::runtime_error("create_publisher/subscriber falló");
  }

  ~FastDdsBackend() {
    if (participant_ != nullptr) {
      participant_->delete_contained_entities();
      fdds::DomainParticipantFactory::get_instance()->delete_participant(participant_);
    }
  }

  [[nodiscard]] std::string version() const {
    return std::string("Fast DDS ") + FASTDDS_VERSION_STR + " (" + cfg_.participant_profile + ")";
  }

  template <class T>
  std::unique_ptr<Writer<T>> create_writer(const StreamSpec& spec, const std::string& topic_name, const QosSpec& qos,
                                           const std::string& profile, WriterStatusCounters* counters) {
    (void)spec;
    fdds::Topic* topic = get_topic<T>(topic_name);
    fdds::DataWriterQos wq = publisher_->get_default_datawriter_qos();
    if (!profile.empty() && publisher_->get_datawriter_qos_from_profile(profile, wq) != fdds::RETCODE_OK) {
      throw std::runtime_error("perfil data_writer '" + profile + "' no encontrado");
    }
    apply_common(wq, qos);
    if (qos.transport_priority) wq.transport_priority().value = *qos.transport_priority;
    wq.publish_mode().kind = fdds::SYNCHRONOUS_PUBLISH_MODE;
    apply_deployment(wq);
    auto listener = std::make_unique<WriterListener>(counters);
    const fdds::StatusMask mask = fdds::StatusMask::publication_matched() << fdds::StatusMask::offered_deadline_missed()
                                  << fdds::StatusMask::offered_incompatible_qos() << fdds::StatusMask::liveliness_lost();
    fdds::DataWriter* w = publisher_->create_datawriter(topic, wq, listener.get(), mask);
    if (w == nullptr) throw std::runtime_error("create_datawriter falló en " + topic_name + " (QoS inconsistente?)");
    const auto rep = wq.representation().m_value.empty() ? fdds::DEFAULT_DATA_REPRESENTATION : wq.representation().m_value[0];
    const bool plain = type_support<T>()->is_plain(rep);
    return std::make_unique<Writer<T>>(publisher_, w, std::move(listener), plain);
  }

  template <class T>
  std::unique_ptr<Reader<T>> create_reader(const StreamSpec& spec, const std::string& topic_name, const QosSpec& qos,
                                           const std::string& profile, ReaderStatusCounters* counters) {
    fdds::Topic* topic = get_topic<T>(topic_name);
    fdds::DataReaderQos rq = subscriber_->get_default_datareader_qos();
    if (!profile.empty() && subscriber_->get_datareader_qos_from_profile(profile, rq) != fdds::RETCODE_OK) {
      throw std::runtime_error("perfil data_reader '" + profile + "' no encontrado");
    }
    apply_common(rq, qos);
    apply_deployment(rq);
    const bool is_echo = topic_name == spec.echo_topic;
    if (cfg_.transport == "udpm" && !is_echo) {
      // Datos por multicast: el DataReader anuncia el grupo y el DataWriter envía
      // un único datagrama al grupo (puerto de usuario multicast del dominio).
      frtps::Locator_t loc;
      loc.kind = LOCATOR_KIND_UDPv4;
      frtps::IPLocator::setIPv4(loc, cfg_.mcast_addr);
      loc.port = static_cast<uint32_t>(7400 + 250 * cfg_.domain_id + 1);
      rq.endpoint().multicast_locator_list.push_back(loc);
    }
    auto listener = std::make_unique<ReaderListener>(counters);
    const fdds::StatusMask mask = fdds::StatusMask::subscription_matched() << fdds::StatusMask::requested_deadline_missed()
                                  << fdds::StatusMask::liveliness_changed() << fdds::StatusMask::sample_rejected()
                                  << fdds::StatusMask::requested_incompatible_qos() << fdds::StatusMask::sample_lost();
    fdds::DataReader* r = subscriber_->create_datareader(topic, rq, listener.get(), mask);
    if (r == nullptr) throw std::runtime_error("create_datareader falló en " + topic_name + " (QoS inconsistente?)");
    return std::make_unique<Reader<T>>(subscriber_, r, std::move(listener));
  }

  std::unique_ptr<WaitSet> create_waitset() { return std::make_unique<WaitSet>(); }

 private:
  template <class T>
  fdds::TypeSupport& type_support() {
    using PST = typename FastPubSubType<T>::type;
    const std::string key = typeid(T).name();
    auto it = types_.find(key);
    if (it == types_.end()) {
      fdds::TypeSupport ts(new PST());
      if (ts.register_type(participant_) != fdds::RETCODE_OK) throw std::runtime_error("register_type falló");
      it = types_.emplace(key, ts).first;
    }
    return it->second;
  }

  template <class T>
  fdds::Topic* get_topic(const std::string& name) {
    auto it = topics_.find(name);
    if (it != topics_.end()) return it->second;
    fdds::TypeSupport& ts = type_support<T>();
    fdds::Topic* t = participant_->create_topic(name, ts.get_type_name(), fdds::TOPIC_QOS_DEFAULT);
    if (t == nullptr) throw std::runtime_error("create_topic falló: " + name);
    topics_.emplace(name, t);
    return t;
  }

  const BenchConfig& cfg_;
  fdds::DomainParticipant* participant_ = nullptr;
  fdds::Publisher* publisher_ = nullptr;
  fdds::Subscriber* subscriber_ = nullptr;
  std::map<std::string, fdds::TypeSupport> types_;
  std::map<std::string, fdds::Topic*> topics_;
};

}  // namespace bench
