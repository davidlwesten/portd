#pragma once

#include <stdint.h>

#include <systemd/sd-bus.h>

typedef struct mapping {
  char const *protocol;
  uint16_t port;
  uint32_t owner;
  // CLOCK_REALTIME microseconds
  uint64_t expires;
  uint16_t external_port;
} mapping_t;

// Reads the properties of a Mapping interface, leaving those that aren't there
// as they were
[[nodiscard]] int read_mapping(sd_bus_message *reply, mapping_t *mapping);

typedef void
visit_t(void *userdata, char const *path, mapping_t const *mapping);

// Calls `visit` for each mapping in a GetManagedObjects reply
[[nodiscard]] int
for_each_mapping(sd_bus_message *reply, visit_t *visit, void *userdata);

[[nodiscard]] int
get_mappings(sd_bus *bus, sd_bus_error *error, sd_bus_message **reply);

// A property of the Manager
[[nodiscard]] int get_string(sd_bus *bus,
                             char const *property,
                             sd_bus_error *error,
                             char **value);
