#include <inttypes.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#include "bus.h"
#include "portctl.h"

static void print_error(char const *doing, sd_bus_error const *error, int r) {
  fprintf(stderr, "portctl: %s: %s\n", doing,
          sd_bus_error_is_set(error) ? error->message : strerror(-r));
}

// The two largest units, like "1h 59m"
static void format_duration(size_t size, char text[], uint64_t seconds) {
  struct {
    uint64_t seconds;
    char unit;
  } const units[] = {{86400, 'd'}, {3600, 'h'}, {60, 'm'}, {1, 's'}};
  size_t length = 0;
  int shown = 0;
  text[0] = '\0';
  for (size_t i = 0; i < sizeof(units) / sizeof(units[0]) && shown < 2; i++) {
    uint64_t const count = seconds / units[i].seconds;
    if (count == 0 && shown == 0) {
      continue;
    }
    seconds %= units[i].seconds;
    length += (size_t)snprintf(&text[length], size - length, "%s%" PRIu64 "%c",
                               shown > 0 ? " " : "", count, units[i].unit);
    shown++;
  }
  if (shown == 0) {
    snprintf(text, size, "0s");
  }
}

static void print_mapping(mapping_t const *mapping, char const *external) {
  char port[sizeof("65535/") + 8];
  snprintf(port, sizeof(port), "%u/%s", (unsigned)mapping->port,
           mapping->protocol);

  char external_port[64] = "waiting";
  if (mapping->external_port != 0) {
    snprintf(external_port, sizeof(external_port), "%s%s%u", external,
             external[0] != '\0' ? ":" : "", (unsigned)mapping->external_port);
  }

  char owner[32];
  struct passwd const *user = getpwuid(mapping->owner);
  if (user != nullptr) {
    snprintf(owner, sizeof(owner), "%s", user->pw_name);
  } else {
    snprintf(owner, sizeof(owner), "%" PRIu32, mapping->owner);
  }

  uint64_t const now_us = now(CLOCK_REALTIME);
  char expires[32];
  format_duration(sizeof(expires), expires,
                  mapping->expires > now_us
                      ? (mapping->expires - now_us + 999999) / 1000000
                      : 0);

  printf("%-11s %-21s %-10s in %s\n", port, external_port, owner, expires);
}

typedef struct listing {
  char const *external;
  size_t count;
} listing_t;

static void
list_mapping(void *userdata, char const * /*path*/, mapping_t const *mapping) {
  listing_t *listing = userdata;
  if (listing->count++ == 0) {
    printf("%-11s %-21s %-10s %s\n", "PORT", "EXTERNAL", "OWNER", "EXPIRES");
  }
  print_mapping(mapping, listing->external);
}

[[nodiscard]] static int
list_mappings(sd_bus *bus, sd_bus_error *error, char const *external) {
  listing_t listing = {.external = external};
  int const r = for_each_mapping(bus, error, list_mapping, &listing);
  if (r < 0) {
    return r;
  }
  if (listing.count == 0) {
    printf("No mappings\n");
  }
  return 0;
}

typedef struct manager {
  char *gateway;
  char *protocol;
  char *external;
} manager_t;

[[nodiscard]] static int
get_manager(sd_bus *bus, sd_bus_error *error, manager_t *manager) {
  int r = get_string(bus, "Gateway", error, &manager->gateway);
  if (r < 0) {
    return r;
  }
  r = get_string(bus, "GatewayProtocol", error, &manager->protocol);
  if (r < 0) {
    return r;
  }
  return get_string(bus, "ExternalAddress", error, &manager->external);
}

static void print_gateway(manager_t const *manager) {
  if (manager->gateway[0] == '\0') {
    printf("No gateway\n");
  } else if (manager->external[0] == '\0') {
    printf("Gateway %s (%s)\n", manager->gateway, manager->protocol);
  } else {
    printf("Gateway %s (%s), external address %s\n", manager->gateway,
           manager->protocol, manager->external);
  }
}

int status(sd_bus *bus) {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  manager_t manager = {};
  int r = get_manager(bus, &error, &manager);
  if (r >= 0) {
    print_gateway(&manager);
    printf("\n");
    r = list_mappings(bus, &error, manager.external);
  }
  if (r < 0) {
    print_error("asking portd", &error, r);
  }

  free(manager.external);
  free(manager.protocol);
  free(manager.gateway);
  sd_bus_error_free(&error);
  return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
