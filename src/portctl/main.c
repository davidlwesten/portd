#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#include "bus.h"
#include "portd1.h"

static uint64_t now(clockid_t clock) {
  struct timespec time;
  clock_gettime(clock, &time);
  return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_nsec / 1000;
}

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
  sd_bus_message *reply = nullptr;
  int r = get_mappings(bus, error, &reply);
  if (r < 0) {
    return r;
  }

  listing_t listing = {.external = external};
  r = for_each_mapping(reply, list_mapping, &listing);
  sd_bus_message_unref(reply);
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

static int status(sd_bus *bus) {
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

static constexpr uint32_t DEFAULT_LIFETIME = 3600;
// How long to wait before saying the gateway hasn't answered yet
static constexpr uint64_t PATIENCE_US = 2'000'000;

static char const USAGE[] =
    "usage: portctl status\n"
    "       portctl map PORT/PROTOCOL [--lifetime DURATION]\n"
    "       portctl release PORT/PROTOCOL\n";

// Seconds, or a number with an s, m, h or d suffix
static bool parse_lifetime(char const *text, uint32_t *lifetime) {
  char *end;
  errno = 0;
  unsigned long long const count = strtoull(text, &end, 10);
  if (errno != 0 || end == text || text[0] == '-') {
    return false;
  }
  unsigned long long unit;
  switch (*end) {
  case '\0':
  case 's':
    unit = 1;
    break;
  case 'm':
    unit = 60;
    break;
  case 'h':
    unit = 3600;
    break;
  case 'd':
    unit = 86400;
    break;
  default:
    return false;
  }
  if (*end != '\0' && end[1] != '\0') {
    return false;
  }
  if (count == 0 || count > UINT32_MAX / unit) {
    return false;
  }
  *lifetime = (uint32_t)(count * unit);
  return true;
}

// Splits "12345/udp", leaving the protocol for portd to check
static bool
parse_port(char const *text, uint16_t *port, char const **protocol) {
  char *end;
  errno = 0;
  unsigned long const value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || text[0] == '-') {
    return false;
  }
  if (value == 0 || value > UINT16_MAX) {
    return false;
  }
  if (*end != '/' || end[1] == '\0') {
    return false;
  }

  *port = (uint16_t)value;
  *protocol = &end[1];
  return true;
}

static bool parse_map_arguments(int argc,
                                char *argv[],
                                uint16_t *port,
                                char const **protocol,
                                uint32_t *lifetime) {
  static struct option const options[] = {
      {"lifetime", required_argument, nullptr, 'l'},
      {},
  };
  *lifetime = DEFAULT_LIFETIME;
  int option;
  while ((option = getopt_long(argc, argv, "l:", options, nullptr)) != -1) {
    if (option != 'l' || !parse_lifetime(optarg, lifetime)) {
      return false;
    }
  }
  return optind == argc - 1 && parse_port(argv[optind], port, protocol);
}

typedef struct wait {
  // Null until Map replies
  char *path;
  bool done;
  uint16_t external_port;
  // Set if the mapping failed
  char *reason;
} wait_t;

static bool is_ours(wait_t const *wait, sd_bus_message *message) {
  return wait->path != nullptr &&
         strcmp(sd_bus_message_get_path(message), wait->path) == 0;
}

static int on_properties_changed(sd_bus_message *message,
                                 void *userdata,
                                 sd_bus_error * /*error*/) {
  wait_t *wait = userdata;
  if (!is_ours(wait, message)) {
    return 0;
  }

  char const *interface;
  int r = sd_bus_message_read(message, "s", &interface);
  if (r < 0) {
    return r;
  }
  if (strcmp(interface, PORTD1_MAPPING_INTERFACE) != 0) {
    return 0;
  }

  mapping_t changed = {};
  r = read_mapping(message, &changed);
  if (r < 0) {
    return r;
  }
  if (changed.external_port != 0) {
    wait->external_port = changed.external_port;
    wait->done = true;
  }
  return 0;
}

static int
on_failed(sd_bus_message *message, void *userdata, sd_bus_error * /*error*/) {
  wait_t *wait = userdata;
  if (!is_ours(wait, message)) {
    return 0;
  }

  char const *reason;
  int const r = sd_bus_message_read(message, "s", &reason);
  if (r < 0) {
    return r;
  }
  wait->reason = strdup(reason);
  wait->done = true;
  return wait->reason != nullptr ? 0 : -ENOMEM;
}

static int on_interfaces_removed(sd_bus_message *message,
                                 void *userdata,
                                 sd_bus_error * /*error*/) {
  wait_t *wait = userdata;
  char const *path;
  int const r = sd_bus_message_read(message, "o", &path);
  if (r < 0) {
    return r;
  }
  if (wait->path != nullptr && strcmp(path, wait->path) == 0) {
    wait->done = true;
  }
  return 0;
}

// Watches before mapping, so no answer from the gateway can be missed
[[nodiscard]] static int watch(sd_bus *bus, wait_t *wait) {
  int r = sd_bus_match_signal(bus, nullptr, PORTD1_NAME, nullptr,
                              "org.freedesktop.DBus.Properties",
                              "PropertiesChanged", on_properties_changed, wait);
  if (r < 0) {
    return r;
  }
  r = sd_bus_match_signal(bus, nullptr, PORTD1_NAME, nullptr,
                          PORTD1_MAPPING_INTERFACE, "Failed", on_failed, wait);
  if (r < 0) {
    return r;
  }
  return sd_bus_match_signal(bus, nullptr, PORTD1_NAME, PORTD1_PATH,
                             "org.freedesktop.DBus.ObjectManager",
                             "InterfacesRemoved", on_interfaces_removed, wait);
}

[[nodiscard]] static int request_mapping(sd_bus *bus,
                                         sd_bus_error *error,
                                         char const *protocol,
                                         uint16_t port,
                                         uint32_t lifetime,
                                         wait_t *wait) {
  sd_bus_message *reply = nullptr;
  int r = sd_bus_call_method(bus, PORTD1_NAME, PORTD1_PATH,
                             PORTD1_MANAGER_INTERFACE, "Map", error, &reply,
                             "squa{sv}", protocol, port, lifetime, 0);
  if (r < 0) {
    return r;
  }

  char const *path;
  r = sd_bus_message_read(reply, "o", &path);
  if (r >= 0) {
    wait->path = strdup(path);
    r = wait->path != nullptr ? 0 : -ENOMEM;
  }
  sd_bus_message_unref(reply);
  return r;
}

// A mapping that's extended may have been granted already. One that's gone
// already has its signals waiting.
[[nodiscard]] static int
check_granted(sd_bus *bus, sd_bus_error *error, wait_t *wait) {
  int const r = sd_bus_get_property_trivial(
      bus, PORTD1_NAME, wait->path, PORTD1_MAPPING_INTERFACE, "ExternalPort",
      error, 'q', &wait->external_port);
  if (sd_bus_error_has_name(error, SD_BUS_ERROR_UNKNOWN_OBJECT)) {
    sd_bus_error_free(error);
    return 0;
  }
  if (r < 0) {
    return r;
  }
  wait->done = wait->external_port != 0;
  return 0;
}

[[nodiscard]] static int run_until_done(sd_bus *bus, wait_t *wait) {
  uint64_t const start = now(CLOCK_MONOTONIC);
  bool told = false;
  while (!wait->done) {
    int r = sd_bus_process(bus, nullptr);
    if (r < 0) {
      return r;
    }
    if (r > 0) {
      continue;
    }

    uint64_t timeout = UINT64_MAX;
    if (!told) {
      uint64_t const waited = now(CLOCK_MONOTONIC) - start;
      if (waited >= PATIENCE_US) {
        fprintf(stderr, "Waiting for the gateway...\n");
        told = true;
      } else {
        timeout = PATIENCE_US - waited;
      }
    }
    r = sd_bus_wait(bus, timeout);
    if (r < 0) {
      return r;
    }
  }
  return 0;
}

// Until the gateway grants the mapping or portd gives up on it
[[nodiscard]] static int wait_for_mapping(sd_bus *bus,
                                          sd_bus_error *error,
                                          char const *protocol,
                                          uint16_t port,
                                          uint32_t lifetime,
                                          wait_t *wait) {
  int r = watch(bus, wait);
  if (r < 0) {
    return r;
  }
  r = request_mapping(bus, error, protocol, port, lifetime, wait);
  if (r < 0) {
    return r;
  }
  r = check_granted(bus, error, wait);
  if (r < 0) {
    return r;
  }
  return run_until_done(bus, wait);
}

[[nodiscard]] static int print_granted(sd_bus *bus,
                                       sd_bus_error *error,
                                       uint16_t port,
                                       char const *protocol,
                                       uint16_t external_port) {
  char *external = nullptr;
  int const r = get_string(bus, "ExternalAddress", error, &external);
  if (r < 0) {
    return r;
  }

  printf("%u/%s reachable at %s%s%u\n", (unsigned)port, protocol, external,
         external[0] != '\0' ? ":" : "port ", (unsigned)external_port);
  free(external);
  return 0;
}

static int map(sd_bus *bus, int argc, char *argv[]) {
  uint16_t port;
  char const *protocol;
  uint32_t lifetime;
  if (!parse_map_arguments(argc, argv, &port, &protocol, &lifetime)) {
    fputs(USAGE, stderr);
    return 2;
  }

  sd_bus_error error = SD_BUS_ERROR_NULL;
  wait_t wait = {};
  int r = wait_for_mapping(bus, &error, protocol, port, lifetime, &wait);
  if (r >= 0 && wait.external_port != 0) {
    r = print_granted(bus, &error, port, protocol, wait.external_port);
  }

  int result = EXIT_FAILURE;
  if (r < 0) {
    fprintf(stderr, "portctl: mapping %u/%s: %s\n", (unsigned)port, protocol,
            sd_bus_error_is_set(&error) ? error.message : strerror(-r));
  } else if (wait.external_port != 0) {
    result = EXIT_SUCCESS;
  } else if (wait.reason != nullptr) {
    fprintf(stderr, "portctl: %u/%s failed: %s\n", (unsigned)port, protocol,
            wait.reason);
  } else {
    fprintf(stderr, "portctl: %u/%s went away before the gateway granted it\n",
            (unsigned)port, protocol);
  }

  free(wait.reason);
  free(wait.path);
  sd_bus_error_free(&error);
  return result;
}

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
  sd_bus_message *reply = nullptr;
  int r = get_mappings(bus, error, &reply);
  if (r < 0) {
    return r;
  }

  r = for_each_mapping(reply, find_mapping, search);
  sd_bus_message_unref(reply);
  if (r < 0) {
    return r;
  }
  return search->out_of_memory ? -ENOMEM : 0;
}

static int release(sd_bus *bus, char const *text) {
  search_t search = {};
  if (!parse_port(text, &search.port, &search.protocol)) {
    fputs(USAGE, stderr);
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

int main(int argc, char *argv[]) {
  bool const is_status = argc == 2 && strcmp(argv[1], "status") == 0;
  bool const is_map = argc >= 2 && strcmp(argv[1], "map") == 0;
  bool const is_release = argc == 3 && strcmp(argv[1], "release") == 0;
  if (!is_status && !is_map && !is_release) {
    fputs(USAGE, stderr);
    return 2;
  }

  sd_bus *bus = nullptr;
  int const r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "portctl: connecting to the system bus: %s\n",
            strerror(-r));
    return EXIT_FAILURE;
  }
  // Each command parses its own options, with the command as argv[0]
  int result;
  if (is_status) {
    result = status(bus);
  } else if (is_map) {
    result = map(bus, argc - 1, &argv[1]);
  } else {
    result = release(bus, argv[2]);
  }
  sd_bus_flush_close_unref(bus);
  return result;
}
