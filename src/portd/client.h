#pragma once

#include <stdint.h>

#include <systemd/sd-event.h>

#include "gateway.h"
#include "nat.h"

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
  struct mapping *mappings;
  bool stopping;
} client_t;

[[nodiscard]] int
client_begin(client_t *client, sd_event *event, uint16_t capacity);

void client_end(client_t *client);

// Keeps the port mapped for `lifetime` seconds, on whichever gateway there is.
// Fails with ENOBUFS when the table is full.
[[nodiscard]] int client_map(client_t *client,
                             nat_protocol_t protocol,
                             uint16_t port,
                             uint32_t lifetime);

// Talks to `gateway` from now on, or to nothing if it has no ifindex
[[nodiscard]] int client_connect(client_t *client, gateway_t const *gateway);

// Releases the mappings and exits the event loop once that's done, or right
// away when called again
void client_stop(client_t *client);
