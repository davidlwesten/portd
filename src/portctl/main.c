#include <inttypes.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#include "portd1.h"

typedef struct mapping {
  char const *protocol;
  uint16_t port;
  uint32_t owner;
  // CLOCK_REALTIME microseconds
  uint64_t expires;
  uint16_t external_port;
} mapping_t;

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

  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  uint64_t const now_us =
      (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
  char expires[32];
  format_duration(sizeof(expires), expires,
                  mapping->expires > now_us
                      ? (mapping->expires - now_us + 999999) / 1000000
                      : 0);

  printf("%-11s %-21s %-10s in %s\n", port, external_port, owner, expires);
}

// Reads the properties of one object's Mapping interface
[[nodiscard]] static int read_mapping(sd_bus_message *reply,
                                      mapping_t *mapping) {
  int r = sd_bus_message_enter_container(reply, 'a', "{sv}");
  if (r < 0) {
    return r;
  }
  while ((r = sd_bus_message_enter_container(reply, 'e', "sv")) > 0) {
    char const *name;
    r = sd_bus_message_read(reply, "s", &name);
    if (r < 0) {
      return r;
    }
    if (strcmp(name, "Protocol") == 0) {
      r = sd_bus_message_read(reply, "v", "s", &mapping->protocol);
    } else if (strcmp(name, "Port") == 0) {
      r = sd_bus_message_read(reply, "v", "q", &mapping->port);
    } else if (strcmp(name, "Owner") == 0) {
      r = sd_bus_message_read(reply, "v", "u", &mapping->owner);
    } else if (strcmp(name, "Expires") == 0) {
      r = sd_bus_message_read(reply, "v", "t", &mapping->expires);
    } else if (strcmp(name, "ExternalPort") == 0) {
      r = sd_bus_message_read(reply, "v", "q", &mapping->external_port);
    } else {
      r = sd_bus_message_skip(reply, "v");
    }
    if (r < 0) {
      return r;
    }
    r = sd_bus_message_exit_container(reply);
    if (r < 0) {
      return r;
    }
  }
  if (r < 0) {
    return r;
  }
  return sd_bus_message_exit_container(reply);
}

// Prints each mapping in a GetManagedObjects reply, returning how many
[[nodiscard]] static int print_mappings(sd_bus_message *reply,
                                        char const *external) {
  int count = 0;
  int r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
  if (r < 0) {
    return r;
  }
  while ((r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}")) > 0) {
    r = sd_bus_message_skip(reply, "o");
    if (r < 0) {
      return r;
    }
    r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
    if (r < 0) {
      return r;
    }
    while ((r = sd_bus_message_enter_container(reply, 'e', "sa{sv}")) > 0) {
      char const *interface;
      r = sd_bus_message_read(reply, "s", &interface);
      if (r < 0) {
        return r;
      }
      if (strcmp(interface, PORTD1_MAPPING_INTERFACE) == 0) {
        mapping_t mapping = {.protocol = ""};
        r = read_mapping(reply, &mapping);
        if (r < 0) {
          return r;
        }
        if (count++ == 0) {
          printf("%-11s %-21s %-10s %s\n", "PORT", "EXTERNAL", "OWNER",
                 "EXPIRES");
        }
        print_mapping(&mapping, external);
      } else {
        r = sd_bus_message_skip(reply, "a{sv}");
        if (r < 0) {
          return r;
        }
      }
      r = sd_bus_message_exit_container(reply);
      if (r < 0) {
        return r;
      }
    }
    if (r < 0) {
      return r;
    }
    r = sd_bus_message_exit_container(reply);
    if (r < 0) {
      return r;
    }
    r = sd_bus_message_exit_container(reply);
    if (r < 0) {
      return r;
    }
  }
  return r < 0 ? r : count;
}

[[nodiscard]] static int get_string(sd_bus *bus,
                                    char const *property,
                                    sd_bus_error *error,
                                    char **value) {
  return sd_bus_get_property_string(bus, PORTD1_NAME, PORTD1_PATH,
                                    PORTD1_MANAGER_INTERFACE, property, error,
                                    value);
}

static int status(sd_bus *bus) {
  sd_bus_error error = SD_BUS_ERROR_NULL;
  char *gateway = nullptr;
  char *protocol = nullptr;
  char *external = nullptr;
  sd_bus_message *reply = nullptr;
  int r = get_string(bus, "Gateway", &error, &gateway);
  if (r >= 0) {
    r = get_string(bus, "GatewayProtocol", &error, &protocol);
  }
  if (r >= 0) {
    r = get_string(bus, "ExternalAddress", &error, &external);
  }
  if (r >= 0) {
    r = sd_bus_call_method(bus, PORTD1_NAME, PORTD1_PATH,
                           "org.freedesktop.DBus.ObjectManager",
                           "GetManagedObjects", &error, &reply, "");
  }
  if (r < 0) {
    print_error("asking portd", &error, r);
  } else {
    if (gateway[0] == '\0') {
      printf("No gateway\n");
    } else if (external[0] == '\0') {
      printf("Gateway %s (%s)\n", gateway, protocol);
    } else {
      printf("Gateway %s (%s), external address %s\n", gateway, protocol,
             external);
    }
    printf("\n");
    r = print_mappings(reply, external);
    if (r == 0) {
      printf("No mappings\n");
    } else if (r < 0) {
      print_error("reading the mappings", &error, r);
    }
  }
  sd_bus_message_unref(reply);
  free(external);
  free(protocol);
  free(gateway);
  sd_bus_error_free(&error);
  return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
  if (argc != 2 || strcmp(argv[1], "status") != 0) {
    fprintf(stderr, "usage: portctl status\n");
    return 2;
  }

  sd_bus *bus = nullptr;
  int const r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "portctl: connecting to the system bus: %s\n",
            strerror(-r));
    return EXIT_FAILURE;
  }
  int const result = status(bus);
  sd_bus_flush_close_unref(bus);
  return result;
}
