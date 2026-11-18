#pragma once

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>

#include "client.h"

typedef struct bus {
  sd_bus *bus;
  client_t *client;
} bus_t;

// Serves `client`'s mappings on the system bus
[[nodiscard]] int bus_start(bus_t *bus, sd_event *event, client_t *client);

void bus_stop(bus_t *bus);

// The client's callbacks, with the bus as userdata
void bus_mapping_changed(void *userdata, mapping_t const *mapping);
void bus_mapping_removed(void *userdata,
                         mapping_t const *mapping,
                         nat_result_t const *failure);
