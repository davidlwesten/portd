#pragma once

#include <stdint.h>

#include <byo/natpmp/natpmp.h>
#include <byo/pcp/pcp.h>
#include <systemd/sd-event.h>

#include "gateway.h"

typedef struct client {
  sd_event *event;
  // Owns the socket connected to the gateway
  sd_event_source *socket;
  // Owns the socket for the gateway's announcements, if it could be opened
  sd_event_source *announcements;
  sd_event_source *timer;
  // At most one of the sessions, PCP first, and neither while there's no
  // gateway
  pcp_session_t *pcp;
  natpmp_session_t *natpmp;
  union {
    pcp_handle_t pcp;
    natpmp_handle_t natpmp;
  } mapping;
  bool stopping;
  uint8_t buffer[PCP_PACKET_MAX_SIZE];
} client_t;

// Talks to `gateway` from now on, or to nothing if it has no ifindex
[[nodiscard]] int client_connect(client_t *client, gateway_t const *gateway);

// Releases the mappings and exits the event loop once that's done, or right
// away when called again
void client_stop(client_t *client);

void client_disconnect(client_t *client);
