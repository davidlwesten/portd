#include "nat.h"

#include <assert.h>
#include <errno.h>
#include <stdlib.h>

#include <byo/natpmp/natpmp.h>
#include <byo/pcp/pcp.h>

static_assert(NAT_PACKET_MAX_SIZE == PCP_PACKET_MAX_SIZE);

typedef struct nat_mapping {
  // 0 for a free slot
  uint32_t lifetime;
  nat_protocol_t protocol;
  uint16_t port;
  uint16_t external_port;
  // In whichever session is in use
  uint16_t handle;
} nat_mapping_t;

struct nat_session {
  uint16_t capacity;
  // Null once the session fell back to NAT-PMP
  pcp_session_t *pcp;
  // Created along with the PCP session, so falling back can't fail
  natpmp_session_t *natpmp;
  // From NAT-PMP, 0.0.0.0 until known
  pcp_ip_address_t external;
  uint8_t buffer[PCP_PACKET_MAX_SIZE];
  nat_mapping_t mappings[];
};

nat_session_t *nat_session_begin(nat_config_t config) {
  assert(config.capacity > 0);
  nat_session_t *session =
      calloc(1, sizeof(*session) +
                    (size_t)config.capacity * sizeof(session->mappings[0]));
  if (session == nullptr) {
    return nullptr;
  }
  session->capacity = config.capacity;
  session->external = pcp_ip_address_any_v4;

  session->pcp = pcp_session_begin((pcp_config_t){
      .local = config.local,
      .rng = {.fill = config.rng.fill, .ctx = config.rng.ctx},
      .buffer = {.size = sizeof(session->buffer), .data = session->buffer},
  });
  session->natpmp = natpmp_session_begin((natpmp_config_t){
      .rng = {.fill = config.rng.fill, .ctx = config.rng.ctx},
      .capacity = config.capacity,
  });
  if (session->pcp == nullptr || session->natpmp == nullptr) {
    nat_session_end(session);
    return nullptr;
  }
  return session;
}

void nat_session_end(nat_session_t *session) {
  if (session->pcp != nullptr) {
    pcp_session_end(session->pcp);
  }
  if (session->natpmp != nullptr) {
    natpmp_session_end(session->natpmp);
  }
  free(session);
}

nat_gateway_protocol_t nat_session_protocol(nat_session_t const *session) {
  return session->pcp != nullptr ? NAT_GATEWAY_PROTOCOL_PCP
                                 : NAT_GATEWAY_PROTOCOL_NATPMP;
}

static nat_handle_t handle_of(nat_session_t const *session,
                              nat_mapping_t const *mapping) {
  return (nat_handle_t)(mapping - session->mappings + 1);
}

static nat_mapping_t *find_handle(nat_session_t *session, nat_handle_t handle) {
  if (handle == 0 || handle > session->capacity) {
    return nullptr;
  }
  nat_mapping_t *mapping = &session->mappings[handle - 1];
  return mapping->lifetime != 0 ? mapping : nullptr;
}

// The mapping with `handle` in the session in use
static nat_mapping_t *find_inner(nat_session_t *session, uint16_t handle) {
  for (size_t i = 0; i < session->capacity; i++) {
    nat_mapping_t *mapping = &session->mappings[i];
    if (mapping->lifetime != 0 && mapping->handle == handle) {
      return mapping;
    }
  }
  return nullptr;
}

[[nodiscard]] static int request(nat_session_t *session,
                                 nat_mapping_t *mapping) {
  if (session->pcp != nullptr) {
    pcp_handle_or_error_t const handle = pcp_session_map_request(
        session->pcp, mapping->lifetime, (pcp_protocol_t)mapping->protocol,
        mapping->port,
        (pcp_socket_address_t){.ip = pcp_ip_address_any_v4,
                               .port = mapping->external_port},
        0, nullptr);
    if (!handle.has_value) {
      return -handle.error;
    }
    mapping->handle = handle.value;
    return 0;
  }

  natpmp_handle_or_error_t const handle = natpmp_session_map_request(
      session->natpmp, mapping->lifetime, (natpmp_protocol_t)mapping->protocol,
      mapping->port, mapping->external_port);
  if (!handle.has_value) {
    return -handle.error;
  }
  mapping->handle = handle.value;
  return 0;
}

nat_handle_or_error_t nat_session_map_request(nat_session_t *session,
                                              uint32_t lifetime,
                                              nat_protocol_t protocol,
                                              uint16_t port,
                                              uint16_t external_port) {
  assert(lifetime > 0);
  for (size_t i = 0; i < session->capacity; i++) {
    nat_mapping_t *mapping = &session->mappings[i];
    if (mapping->lifetime != 0) {
      continue;
    }

    *mapping = (nat_mapping_t){
        .lifetime = lifetime,
        .protocol = protocol,
        .port = port,
        .external_port = external_port,
    };
    int const r = request(session, mapping);
    if (r < 0) {
      mapping->lifetime = 0;
      return (nat_handle_or_error_t){.error = -r};
    }
    return (nat_handle_or_error_t){.has_value = true,
                                   .value = handle_of(session, mapping)};
  }
  return (nat_handle_or_error_t){.error = ENOBUFS};
}

void nat_session_release(nat_session_t *session, nat_handle_t handle) {
  nat_mapping_t *mapping = find_handle(session, handle);
  if (mapping == nullptr) {
    return;
  }
  if (session->pcp != nullptr) {
    pcp_session_release(session->pcp, mapping->handle);
  } else {
    natpmp_session_release(session->natpmp, mapping->handle);
  }
  mapping->lifetime = 0;
}

static void fall_back(nat_session_t *session) {
  pcp_session_end(session->pcp);
  session->pcp = nullptr;

  natpmp_session_address_request(session->natpmp);
  for (size_t i = 0; i < session->capacity; i++) {
    nat_mapping_t *mapping = &session->mappings[i];
    if (mapping->lifetime != 0) {
      // The NAT-PMP session is as large as this one and holds nothing yet
      [[maybe_unused]] int const r = request(session, mapping);
      assert(r == 0);
    }
  }
}

// The session retries the short-lived failures (e.g. NO_RESOURCES) itself
static nat_result_t from_pcp(pcp_result_t result) {
  return result == PCP_RESULT_NOT_AUTHORIZED ? NAT_RESULT_NOT_AUTHORIZED
                                             : NAT_RESULT_OTHER;
}

static nat_result_t from_natpmp(natpmp_result_t result) {
  switch (result) {
  case NATPMP_RESULT_SUCCESS:
    return NAT_RESULT_UNREACHABLE;
  case NATPMP_RESULT_NOT_AUTHORIZED:
    return NAT_RESULT_NOT_AUTHORIZED;
  case NATPMP_RESULT_NETWORK_FAILURE:
    return NAT_RESULT_NETWORK_FAILURE;
  case NATPMP_RESULT_OUT_OF_RESOURCES:
    return NAT_RESULT_NO_RESOURCES;
  default:
    return NAT_RESULT_OTHER;
  }
}

static nat_event_t poll_pcp(nat_session_t *session, uint64_t now) {
  for (;;) {
    pcp_event_t const event = pcp_session_poll(session->pcp, now);
    nat_mapping_t *mapping;
    switch (event.type) {
    case PCP_EVENT_SEND:
      return (nat_event_t){
          .type = NAT_EVENT_SEND,
          .send = {.size = event.send.size, .data = event.send.data}};
    case PCP_EVENT_MAPPED:
      mapping = find_inner(session, event.mapped.handle);
      if (mapping != nullptr) {
        return (nat_event_t){.type = NAT_EVENT_MAPPED,
                             .mapped = {.handle = handle_of(session, mapping),
                                        .external = event.mapped.external}};
      }
      break;
    case PCP_EVENT_UNMAPPED:
      mapping = find_inner(session, event.unmapped.handle);
      if (mapping != nullptr) {
        return (nat_event_t){
            .type = NAT_EVENT_UNMAPPED,
            .unmapped = {.handle = handle_of(session, mapping)}};
      }
      break;
    case PCP_EVENT_FAILED:
      mapping = find_inner(session, event.failed.handle);
      if (mapping == nullptr) {
        break;
      }
      if (event.failed.result == PCP_RESULT_UNSUPP_VERSION) {
        fall_back(session);
        return (nat_event_t){.type = NAT_EVENT_PROTOCOL};
      }
      mapping->lifetime = 0;
      return (nat_event_t){.type = NAT_EVENT_FAILED,
                           .failed = {.handle = handle_of(session, mapping),
                                      .result = from_pcp(event.failed.result)}};
    case PCP_EVENT_TIMEOUT:
      return (nat_event_t){
          .type = NAT_EVENT_TIMEOUT,
          .timeout.deadline = {.has_value = event.timeout.deadline.has_value,
                               .value = event.timeout.deadline.value}};
    default:
      break;
    }
  }
}

static nat_event_t poll_natpmp(nat_session_t *session, uint64_t now) {
  for (;;) {
    natpmp_event_t const event = natpmp_session_poll(session->natpmp, now);
    nat_mapping_t *mapping;
    switch (event.type) {
    case NATPMP_EVENT_SEND:
      return (nat_event_t){
          .type = NAT_EVENT_SEND,
          .send = {.size = event.send.size, .data = event.send.data}};
    case NATPMP_EVENT_ADDRESS: {
      uint8_t const *octets = event.address.external_ip.octets;
      session->external = pcp_ip_address_from_octets(octets[0], octets[1],
                                                     octets[2], octets[3]);
      return (nat_event_t){.type = NAT_EVENT_ADDRESS,
                           .address = {.external = session->external}};
    }
    case NATPMP_EVENT_MAPPED:
      mapping = find_inner(session, event.mapped.handle);
      if (mapping != nullptr) {
        return (nat_event_t){
            .type = NAT_EVENT_MAPPED,
            .mapped = {.handle = handle_of(session, mapping),
                       .external = {.ip = session->external,
                                    .port = event.mapped.external_port}}};
      }
      break;
    case NATPMP_EVENT_MAP_FAILED:
      mapping = find_inner(session, event.map_failed.handle);
      if (mapping != nullptr) {
        mapping->lifetime = 0;
        return (nat_event_t){
            .type = NAT_EVENT_FAILED,
            .failed = {.handle = handle_of(session, mapping),
                       .result = from_natpmp(event.map_failed.result)}};
      }
      break;
    case NATPMP_EVENT_TIMEOUT:
      return (nat_event_t){
          .type = NAT_EVENT_TIMEOUT,
          .timeout.deadline = {.has_value = event.timeout.deadline.has_value,
                               .value = event.timeout.deadline.value}};
    default:
      break;
    }
  }
}

nat_event_t nat_session_poll(nat_session_t *session, uint64_t now) {
  return session->pcp != nullptr ? poll_pcp(session, now)
                                 : poll_natpmp(session, now);
}

void nat_session_on_datagram(nat_session_t *session,
                             uint64_t now,
                             size_t size,
                             uint8_t const data[]) {
  if (session->pcp != nullptr) {
    pcp_session_on_datagram(session->pcp, now, size, data);
  } else {
    natpmp_session_on_datagram(session->natpmp, now, size, data);
  }
}

void nat_session_on_host_unreachable(nat_session_t *session) {
  if (session->pcp == nullptr) {
    natpmp_session_on_host_unreachable(session->natpmp);
  }
}
