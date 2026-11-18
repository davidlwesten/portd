#pragma once

#include <stddef.h>
#include <stdint.h>

#include <byo/pcp/pcp.h>

// A port mapping session with one gateway, without any I/O like the PCP and
// NAT-PMP sessions it wraps: PCP first, and NAT-PMP if the gateway only speaks
// that. Mappings are known by ids the caller picks, which stay the same when it
// falls back. Functions that can fail return a negative errno value.

typedef struct nat_config {
  // Our address as the gateway sees it
  pcp_ip_address_t local;
  pcp_rng_t rng;
  // How many mappings the session can hold at once. With NAT-PMP, a released
  // mapping still counts until its deletion is done.
  uint16_t capacity;
} nat_config_t;

// The largest datagram either protocol sends or receives
static constexpr size_t NAT_PACKET_MAX_SIZE = PCP_PACKET_MAX_SIZE;

typedef struct nat_session nat_session_t;

nat_session_t *nat_session_begin(nat_config_t config);

// Leaves the mappings on the gateway until their lifetime runs out
void nat_session_end(nat_session_t *session);

typedef enum nat_gateway_protocol {
  NAT_GATEWAY_PROTOCOL_PCP,
  NAT_GATEWAY_PROTOCOL_NATPMP,
} nat_gateway_protocol_t;

nat_gateway_protocol_t nat_session_protocol(nat_session_t const *session);

typedef enum nat_protocol {
  NAT_PROTOCOL_TCP = 6,
  NAT_PROTOCOL_UDP = 17,
} nat_protocol_t;

// "tcp" or "udp"
char const *nat_protocol_name(nat_protocol_t protocol);

// `id` must not be in use by another mapping of the session. `external_port` is
// the one to suggest, 0 for any. With PCP, the external address suggested is
// the last one the gateway gave. Fails with -ENOBUFS when the session is full.
[[nodiscard]] int nat_session_map_request(nat_session_t *session,
                                          uint64_t id,
                                          uint32_t lifetime,
                                          nat_protocol_t protocol,
                                          uint16_t port,
                                          uint16_t external_port);

// `id` must be mapped. No more events refer to it afterwards, so it can be
// used again.
void nat_session_release(nat_session_t *session, uint64_t id);

typedef enum nat_result {
  NAT_RESULT_NOT_AUTHORIZED,
  NAT_RESULT_NO_RESOURCES,
  NAT_RESULT_NETWORK_FAILURE,
  // The gateway didn't answer, or the mapping expired without being renewed
  NAT_RESULT_UNREACHABLE,
  NAT_RESULT_OTHER,
} nat_result_t;

typedef struct nat_event {
  enum {
    NAT_EVENT_SEND,
    // The gateway only speaks NAT-PMP, so the session switched to it and
    // requests the mappings again
    NAT_EVENT_PROTOCOL,
    // The gateway's external address, from NAT-PMP
    NAT_EVENT_ADDRESS,
    // Each time the gateway grants the mapping, with NAT-PMP's external address
    // as 0.0.0.0 until it's known
    NAT_EVENT_MAPPED,
    // The mapping was lost and gets requested again
    NAT_EVENT_UNMAPPED,
    // The last event for the id
    NAT_EVENT_FAILED,
    NAT_EVENT_TIMEOUT,
  } type;
  union {
    struct {
      size_t size;
      // Only valid until the next poll
      uint8_t const *data;
    } send;
    struct {
      pcp_ip_address_t external;
    } address;
    struct {
      uint64_t id;
      pcp_socket_address_t external;
    } mapped;
    struct {
      uint64_t id;
    } unmapped;
    struct {
      uint64_t id;
      nat_result_t result;
    } failed;
    struct {
      // Empty if nothing is scheduled
      pcp_optional_time_ms_t deadline;
    } timeout;
  };
} nat_event_t;

// Call until it returns NAT_EVENT_TIMEOUT whenever something happens, with
// milliseconds from a monotonic clock
nat_event_t nat_session_poll(nat_session_t *session, pcp_time_ms_t now);

// Only datagrams from the gateway, its answers and its announcements
void nat_session_on_datagram(nat_session_t *session,
                             pcp_time_ms_t now,
                             size_t size,
                             uint8_t const data[]);

// For an ICMP Port Unreachable from the gateway
void nat_session_on_host_unreachable(nat_session_t *session);
