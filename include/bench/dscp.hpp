// SPDX-License-Identifier: Apache-2.0
// Materialización de TRANSPORT_PRIORITY en marcado DSCP/TOS y SO_PRIORITY.
//
// Verificado en el código fuente de las versiones usadas:
//   * Fast DDS 3.6.2: UDPTransportInterface::send(..., /*transport_priority*/)
//     recibe el valor pero lo ignora (el elemento XML <udp_priority_mappings>
//     se valida pero sólo está implementado en Fast DDS Pro).
//   * Cyclone DDS 11.0.1: ddsi_tran_qos::m_diffserv existe pero siempre es 0;
//     transport_priority sólo decide la entrega síncrona en el hilo "recv"
//     (Internal/SynchronousDeliveryPriorityThreshold).
// Por tanto el arnés recorre /proc/self/fd y marca todos los sockets UDP del
// proceso (IP_TOS / IPV6_TCLASS + SO_PRIORITY). Granularidad: proceso (todos
// los writers del participante comparten sockets de envío en ambos middlewares).
#pragma once

#include <cstdint>
#include <string>

namespace bench {

struct DscpMapping {
  int dscp = 0;         // 0..63 (EF = 46)
  int so_priority = 0;  // 0..6 sin CAP_NET_ADMIN; prio qdisc: 6 -> banda 0
  const char* phb = "BE";
};

// Mapeo por tramos TRANSPORT_PRIORITY -> PHB DiffServ (RFC 4594):
//   <=0: BE(0)  1-9: AF11(10)  10-19: AF21(18)  20-29: AF31(26)
//   30-39: AF41(34)  40-49: CS5(40)  >=50: EF(46)
[[nodiscard]] DscpMapping map_transport_priority(int32_t transport_priority) noexcept;

// Parsea "auto" | "off" | nombre PHB (EF, AF41, CS5...) | valor numérico 0-63.
// Devuelve -1 para "auto", -2 para "off".
[[nodiscard]] int parse_dscp(const std::string& s);

struct TagResult {
  int sockets_tagged = 0;
  int sockets_failed = 0;
  std::string last_error;
};

// Marca todos los sockets UDP (AF_INET/AF_INET6) del proceso. Idempotente: se
// reaplica periódicamente para cubrir sockets creados tras el descubrimiento.
TagResult tag_process_udp_sockets(int dscp, int so_priority);

}  // namespace bench
