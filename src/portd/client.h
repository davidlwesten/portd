#pragma once

#include <stdint.h>
#include <sys/types.h>

#include <systemd/sd-event.h>

#include "gateway.h"
#include "nat.h"

// Requested from the gateway whenever there's one
typedef struct mapping {
  // Never reused
  uint64_t id;
  // CLOCK_MONOTONIC milliseconds
  uint64_t expires;
  uid_t owner;
  nat_protocol_t protocol;
  uint16_t port;
  // 0 until the gateway grants it
  uint16_t external_port;
} mapping_t;

typedef struct client_callbacks {
  // When the external port changed
  void (*changed)(void *userdata, mapping_t const *mapping);
  // Just before the mapping goes away, with the reason if the gateway gave up
  // on it
  void (*removed)(void *userdata,
                  mapping_t const *mapping,
                  nat_result_t const *failure);
  void *userdata;
} client_callbacks_t;

typedef struct client {
  sd_event *event;
  // Owns the socket connected to the gateway
  sd_event_source *socket;
  // Owns the socket for the gateway's announcements, if it could be opened
  sd_event_source *announcements;
  // For the session and the mappings' expiry
  sd_event_source *timer;
  // Null while there's no gateway
  nat_session_t *nat;
  uint16_t capacity;
  uint16_t count;
  mapping_t *mappings;
  uint64_t next_id;
  client_callbacks_t callbacks;
  bool stopping;
} client_t;

[[nodiscard]] int client_begin(client_t *client,
                               sd_event *event,
                               uint16_t capacity,
                               client_callbacks_t callbacks);

void client_end(client_t *client);

// Keeps the port mapped for `lifetime` seconds, on whichever gateway there is.
// The port must not be mapped already. Fails with ENOBUFS when the table is
// full.
[[nodiscard]] int client_map(client_t *client,
                             uid_t owner,
                             nat_protocol_t protocol,
                             uint16_t port,
                             uint32_t lifetime,
                             uint64_t *id);

// Keeps the mapping for `lifetime` seconds from now instead
void client_extend(client_t *client, uint64_t id, uint32_t lifetime);

void client_release(client_t *client, uint64_t id);

// Null if there's no such mapping. Only valid until the table changes.
mapping_t const *client_find(client_t const *client, uint64_t id);

mapping_t const *client_find_port(client_t const *client,
                                  nat_protocol_t protocol,
                                  uint16_t port);

// Talks to `gateway` from now on, or to nothing if it has no ifindex
[[nodiscard]] int client_connect(client_t *client, gateway_t const *gateway);

// Releases the mappings and exits the event loop once that's done, or right
// away when called again
void client_stop(client_t *client);
