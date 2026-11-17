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
  sd_event_source *timer;
  // Null while there's no gateway
  nat_session_t *nat;
  nat_handle_t mapping;
  bool stopping;
} client_t;

// Talks to `gateway` from now on, or to nothing if it has no ifindex
[[nodiscard]] int client_connect(client_t *client, gateway_t const *gateway);

// Releases the mappings and exits the event loop once that's done, or right
// away when called again
void client_stop(client_t *client);

void client_disconnect(client_t *client);
