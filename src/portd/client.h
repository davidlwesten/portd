#pragma once

#include <stdint.h>

#include <byo/pcp/pcp.h>
#include <systemd/sd-event.h>

#include "gateway.h"

typedef struct client {
  sd_event *event;
  // Owns the socket connected to the gateway
  sd_event_source *socket;
  sd_event_source *timer;
  // Null while there's no gateway
  pcp_session_t *session;
  uint8_t buffer[PCP_PACKET_MAX_SIZE];
} client_t;

// Talks to `gateway` from now on, or to nothing if it has no ifindex
[[nodiscard]] int client_connect(client_t *client, gateway_t const *gateway);

void client_disconnect(client_t *client);
