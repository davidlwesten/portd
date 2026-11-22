#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-bus.h>

#include "bus.h"
#include "portctl.h"
#include "portd1.h"

typedef struct search {
  uint16_t port;
  char const *protocol;
  // Null until found
  char *path;
  bool out_of_memory;
} search_t;

static void
find_mapping(void *userdata, char const *path, mapping_t const *mapping) {
  search_t *search = userdata;
  if (search->path != nullptr) {
    return;
  }
  if (mapping->port != search->port ||
      strcmp(mapping->protocol, search->protocol) != 0) {
    return;
  }

  search->path = strdup(path);
  search->out_of_memory = search->path == nullptr;
}

// Leaves the path null if there's no such mapping
[[nodiscard]] static int
search_mappings(sd_bus *bus, sd_bus_error *error, search_t *search) {
  int const r = for_each_mapping(bus, error, find_mapping, search);
  if (r < 0) {
    return r;
  }
  return search->out_of_memory ? -ENOMEM : 0;
}

int release(sd_bus *bus, char const *text) {
  search_t search = {};
  if (!parse_port(text, &search.port, &search.protocol)) {
    print_usage();
    return 2;
  }

  sd_bus_error error = SD_BUS_ERROR_NULL;
  int r = search_mappings(bus, &error, &search);
  if (r >= 0 && search.path != nullptr) {
    r = sd_bus_call_method(bus, PORTD1_NAME, search.path,
                           PORTD1_MAPPING_INTERFACE, "Release", &error, nullptr,
                           "");
  }

  int result = EXIT_FAILURE;
  if (r < 0) {
    fprintf(stderr, "portctl: releasing %s: %s\n", text,
            sd_bus_error_is_set(&error) ? error.message : strerror(-r));
  } else if (search.path == nullptr) {
    fprintf(stderr, "portctl: %s isn't mapped\n", text);
  } else {
    result = EXIT_SUCCESS;
  }

  free(search.path);
  sd_bus_error_free(&error);
  return result;
}
