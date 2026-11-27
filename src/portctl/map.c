#include <errno.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#include "bus.h"
#include "dbus.h"
#include "portctl.h"

static constexpr uint32_t DEFAULT_LIFETIME = 3600;
// How long to wait before saying the gateway hasn't answered yet
static constexpr uint64_t PATIENCE_US = 2'000'000;

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
  if (strcmp(interface, MAPPING_INTERFACE) != 0) {
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
  int r = sd_bus_match_signal(bus, nullptr, BUS_NAME, nullptr,
                              "org.freedesktop.DBus.Properties",
                              "PropertiesChanged", on_properties_changed, wait);
  if (r < 0) {
    return r;
  }
  r = sd_bus_match_signal(bus, nullptr, BUS_NAME, nullptr, MAPPING_INTERFACE,
                          "Failed", on_failed, wait);
  if (r < 0) {
    return r;
  }
  return sd_bus_match_signal(bus, nullptr, BUS_NAME, MANAGER_PATH,
                             "org.freedesktop.DBus.ObjectManager",
                             "InterfacesRemoved", on_interfaces_removed, wait);
}

// A mapping that's extended may have been granted already. One that's gone
// already has its signals waiting.
[[nodiscard]] static int
check_granted(sd_bus *bus, sd_bus_error *error, wait_t *wait) {
  int const r = sd_bus_get_property_trivial(bus, BUS_NAME, wait->path,
                                            MAPPING_INTERFACE, "ExternalPort",
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
  r = request_mapping(bus, error, protocol, port, lifetime, &wait->path);
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

int map(sd_bus *bus, int argc, char *argv[]) {
  uint16_t port;
  char const *protocol;
  uint32_t lifetime;
  if (!parse_map_arguments(argc, argv, &port, &protocol, &lifetime)) {
    print_usage();
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
