#pragma once

#include <stddef.h>
#include <stdint.h>

#include <libmnl/libmnl.h>
#include <systemd/sd-event.h>

typedef struct gateway {
  // 0 if there's no default route with a gateway
  unsigned ifindex;
  // IPv4 addresses take the first 4 bytes
  uint8_t address[16];
} gateway_t;

enum {
  GATEWAY_IPV4,
  GATEWAY_IPV6,
  GATEWAY_FAMILIES,
};

typedef void
gateway_changed_t(void *userdata, size_t family, gateway_t const *gateway);

typedef struct gateway_monitor {
  struct mnl_socket *socket;
  gateway_t gateways[GATEWAY_FAMILIES];
  gateway_changed_t *changed;
  void *userdata;
} gateway_monitor_t;

// Each family's gateway is the one on its lowest-metric default route in the
// main table. `changed` is called for the gateways found at the start too.
[[nodiscard]] int gateway_monitor_start(gateway_monitor_t *monitor,
                                        sd_event *event,
                                        gateway_changed_t *changed,
                                        void *userdata);

// Only after freeing the event loop, which watches the socket until then
void gateway_monitor_stop(gateway_monitor_t *monitor);
