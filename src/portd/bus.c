#include "bus.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char const NAME[] = "io.github.davidlwesten.Portd1";
static char const PATH[] = "/io/github/davidlwesten/Portd1";
static char const MAPPINGS_PATH[] = "/io/github/davidlwesten/Portd1/mapping";
static char const MANAGER_INTERFACE[] = "io.github.davidlwesten.Portd1.Manager";
static char const MAPPING_INTERFACE[] = "io.github.davidlwesten.Portd1.Mapping";
static char const ALREADY_MAPPED[] =
    "io.github.davidlwesten.Portd1.Error.AlreadyMapped";

// Root has no limit
static constexpr size_t MAPPINGS_PER_USER = 16;

// Room for the prefix, a slash and any id
typedef struct mapping_path {
  char value[sizeof(MAPPINGS_PATH) + 1 + 20];
} mapping_path_t;

static mapping_path_t path_of(uint64_t id) {
  mapping_path_t path;
  snprintf(path.value, sizeof(path.value), "%s/%" PRIu64, MAPPINGS_PATH, id);
  return path;
}

// 0 if `path` isn't a mapping's
static uint64_t id_of(char const *path) {
  size_t const length = sizeof(MAPPINGS_PATH) - 1;
  if (strncmp(path, MAPPINGS_PATH, length) != 0 || path[length] != '/') {
    return 0;
  }
  char const *digits = &path[length + 1];
  // Ids start at 1, so a leading 0 is never one
  if (digits[0] < '1' || digits[0] > '9') {
    return 0;
  }
  char *end;
  errno = 0;
  unsigned long long const id = strtoull(digits, &end, 10);
  return errno == 0 && *end == '\0' ? id : 0;
}

static mapping_t const *mapping_at(bus_t const *bus, char const *path) {
  uint64_t const id = id_of(path);
  return id != 0 ? client_find(bus->client, id) : nullptr;
}

static size_t mappings_of(client_t const *client, uid_t owner) {
  size_t count = 0;
  for (size_t i = 0; i < client->count; i++) {
    if (client->mappings[i].owner == owner) {
      count++;
    }
  }
  return count;
}

[[nodiscard]] static int sender_uid(sd_bus_message *message, uid_t *uid) {
  sd_bus_creds *creds = nullptr;
  int r = sd_bus_query_sender_creds(message, SD_BUS_CREDS_EUID, &creds);
  if (r < 0) {
    return r;
  }
  r = sd_bus_creds_get_euid(creds, uid);
  sd_bus_creds_unref(creds);
  return r;
}

// Read on each call, since it can change
[[nodiscard]] static int unprivileged_port_start(void) {
  FILE *file = fopen("/proc/sys/net/ipv4/ip_unprivileged_port_start", "re");
  if (file == nullptr) {
    return -errno;
  }
  int port = 0;
  int const r = fscanf(file, "%d", &port) == 1 ? port : -EIO;
  fclose(file);
  return r;
}

static bool parse_protocol(char const *name, nat_protocol_t *protocol) {
  nat_protocol_t const protocols[] = {NAT_PROTOCOL_TCP, NAT_PROTOCOL_UDP};
  for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); i++) {
    if (strcmp(name, nat_protocol_name(protocols[i])) == 0) {
      *protocol = protocols[i];
      return true;
    }
  }
  return false;
}

static int
on_map(sd_bus_message *message, void *userdata, sd_bus_error *error) {
  bus_t *bus = userdata;
  char const *protocol_name;
  uint16_t port;
  uint32_t lifetime;
  int r = sd_bus_message_read(message, "squ", &protocol_name, &port, &lifetime);
  if (r < 0) {
    return r;
  }
  // No options are known yet, and unknown ones are ignored
  r = sd_bus_message_skip(message, "a{sv}");
  if (r < 0) {
    return r;
  }

  nat_protocol_t protocol;
  if (!parse_protocol(protocol_name, &protocol)) {
    return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                             "Unknown protocol %s", protocol_name);
  }
  if (port == 0) {
    return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS,
                            "The port can't be 0");
  }
  if (lifetime == 0) {
    return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS,
                            "The lifetime can't be 0");
  }

  uid_t uid;
  r = sender_uid(message, &uid);
  if (r < 0) {
    return r;
  }
  if (uid != 0) {
    r = unprivileged_port_start();
    if (r < 0) {
      return r;
    }
    if (port < r) {
      return sd_bus_error_setf(error, SD_BUS_ERROR_ACCESS_DENIED,
                               "Port %u needs root", (unsigned)port);
    }
  }

  mapping_t const *existing = client_find_port(bus->client, protocol, port);
  uint64_t id;
  if (existing != nullptr) {
    if (existing->owner != uid) {
      return sd_bus_error_setf(error, ALREADY_MAPPED,
                               "%u/%s is mapped by another user",
                               (unsigned)port, protocol_name);
    }
    id = existing->id;
    client_extend(bus->client, id, lifetime);
    r = sd_bus_emit_properties_changed(bus->bus, path_of(id).value,
                                       MAPPING_INTERFACE, "Expires", nullptr);
  } else {
    if (uid != 0 && mappings_of(bus->client, uid) >= MAPPINGS_PER_USER) {
      return sd_bus_error_setf(error, SD_BUS_ERROR_LIMITS_EXCEEDED,
                               "Each user can have up to %zu mappings",
                               MAPPINGS_PER_USER);
    }
    r = client_map(bus->client, uid, protocol, port, lifetime, &id);
    if (r < 0) {
      return r;
    }
    r = sd_bus_emit_object_added(bus->bus, path_of(id).value);
  }
  if (r < 0) {
    fprintf(stderr, "portd: announcing mapping %" PRIu64 ": %s\n", id,
            strerror(-r));
  }
  return sd_bus_reply_method_return(message, "o", path_of(id).value);
}

static int
on_release(sd_bus_message *message, void *userdata, sd_bus_error *error) {
  bus_t *bus = userdata;
  mapping_t const *mapping = mapping_at(bus, sd_bus_message_get_path(message));
  uid_t uid;
  int const r = sender_uid(message, &uid);
  if (r < 0) {
    return r;
  }
  if (uid != 0 && uid != mapping->owner) {
    return sd_bus_error_set(error, SD_BUS_ERROR_ACCESS_DENIED,
                            "Only its owner can release the mapping");
  }
  client_release(bus->client, mapping->id);
  return sd_bus_reply_method_return(message, "");
}

// CLOCK_REALTIME microseconds
static uint64_t realtime_of(sd_bus *bus, uint64_t monotonic_ms) {
  sd_event *event = sd_bus_get_event(bus);
  uint64_t realtime = 0;
  uint64_t monotonic = 0;
  sd_event_now(event, CLOCK_REALTIME, &realtime);
  sd_event_now(event, CLOCK_MONOTONIC, &monotonic);
  uint64_t const at = monotonic_ms * 1000;
  return at > monotonic ? realtime + (at - monotonic) : realtime;
}

static int get_mapping_property(sd_bus *bus,
                                char const *path,
                                char const * /*interface*/,
                                char const *property,
                                sd_bus_message *reply,
                                void *userdata,
                                sd_bus_error * /*error*/) {
  mapping_t const *mapping = mapping_at(userdata, path);
  if (strcmp(property, "Protocol") == 0) {
    return sd_bus_message_append(reply, "s",
                                 nat_protocol_name(mapping->protocol));
  }
  if (strcmp(property, "Port") == 0) {
    return sd_bus_message_append(reply, "q", mapping->port);
  }
  if (strcmp(property, "Owner") == 0) {
    return sd_bus_message_append(reply, "u", (uint32_t)mapping->owner);
  }
  if (strcmp(property, "Expires") == 0) {
    return sd_bus_message_append(reply, "t",
                                 realtime_of(bus, mapping->expires));
  }
  return sd_bus_message_append(reply, "q", mapping->external_port);
}

static int find_mapping(sd_bus * /*bus*/,
                        char const *path,
                        char const * /*interface*/,
                        void *userdata,
                        void **found,
                        sd_bus_error * /*error*/) {
  if (mapping_at(userdata, path) == nullptr) {
    return 0;
  }
  *found = userdata;
  return 1;
}

static int enumerate_mappings(sd_bus * /*bus*/,
                              char const * /*prefix*/,
                              void *userdata,
                              char ***nodes,
                              sd_bus_error * /*error*/) {
  client_t const *client = ((bus_t const *)userdata)->client;
  char **paths = calloc((size_t)client->count + 1, sizeof(paths[0]));
  if (paths == nullptr) {
    return -ENOMEM;
  }
  for (size_t i = 0; i < client->count; i++) {
    paths[i] = strdup(path_of(client->mappings[i].id).value);
    if (paths[i] == nullptr) {
      for (size_t j = 0; j < i; j++) {
        free(paths[j]);
      }
      free(paths);
      return -ENOMEM;
    }
  }
  *nodes = paths;
  return 0;
}

static char const *gateway_protocol_name(nat_gateway_protocol_t protocol) {
  switch (protocol) {
  case NAT_GATEWAY_PROTOCOL_PCP:
    return "pcp";
  case NAT_GATEWAY_PROTOCOL_NATPMP:
    return "nat-pmp";
  }
  unreachable();
}

static int get_manager_property(sd_bus * /*bus*/,
                                char const * /*path*/,
                                char const * /*interface*/,
                                char const *property,
                                sd_bus_message *reply,
                                void *userdata,
                                sd_bus_error * /*error*/) {
  client_t const *client = ((bus_t const *)userdata)->client;
  if (strcmp(property, "Gateway") == 0) {
    char address[INET_ADDRSTRLEN] = "";
    if (client->gateway.ifindex != 0) {
      inet_ntop(AF_INET, client->gateway.address, address, sizeof(address));
    }
    return sd_bus_message_append(reply, "s", address);
  }
  if (strcmp(property, "GatewayProtocol") == 0) {
    return sd_bus_message_append(
        reply, "s",
        client->nat != nullptr
            ? gateway_protocol_name(nat_session_protocol(client->nat))
            : "");
  }
  char address[PCP_IP_ADDRESS_STRING_LENGTH] = "";
  if (memcmp(&client->external, &pcp_ip_address_any_v4,
             sizeof(pcp_ip_address_any_v4)) != 0) {
    pcp_ip_address_to_string(sizeof(address), address, &client->external);
  }
  return sd_bus_message_append(reply, "s", address);
}

static sd_bus_vtable const manager_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD_WITH_NAMES("Map",
                             "squa{sv}",
                             SD_BUS_PARAM(protocol) SD_BUS_PARAM(port)
                                 SD_BUS_PARAM(lifetime) SD_BUS_PARAM(options),
                             "o",
                             SD_BUS_PARAM(mapping),
                             on_map,
                             SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("Gateway",
                    "s",
                    get_manager_property,
                    0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("GatewayProtocol",
                    "s",
                    get_manager_property,
                    0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ExternalAddress",
                    "s",
                    get_manager_property,
                    0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END,
};

static sd_bus_vtable const mapping_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", on_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL_WITH_NAMES("Failed", "s", SD_BUS_PARAM(reason), 0),
    SD_BUS_PROPERTY("Protocol",
                    "s",
                    get_mapping_property,
                    0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Port",
                    "q",
                    get_mapping_property,
                    0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Owner",
                    "u",
                    get_mapping_property,
                    0,
                    SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_PROPERTY("Expires",
                    "t",
                    get_mapping_property,
                    0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ExternalPort",
                    "q",
                    get_mapping_property,
                    0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_VTABLE_END,
};

int bus_start(bus_t *bus, sd_event *event, client_t *client) {
  *bus = (bus_t){.client = client};
  int r = sd_bus_open_system(&bus->bus);
  if (r < 0) {
    return r;
  }
  // So the service manager can start portd again
  r = sd_bus_set_exit_on_disconnect(bus->bus, true);
  if (r >= 0) {
    r = sd_bus_attach_event(bus->bus, event, SD_EVENT_PRIORITY_NORMAL);
  }
  if (r >= 0) {
    r = sd_bus_add_object_vtable(bus->bus, nullptr, PATH, MANAGER_INTERFACE,
                                 manager_vtable, bus);
  }
  if (r >= 0) {
    r = sd_bus_add_object_manager(bus->bus, nullptr, PATH);
  }
  if (r >= 0) {
    r = sd_bus_add_fallback_vtable(bus->bus, nullptr, MAPPINGS_PATH,
                                   MAPPING_INTERFACE, mapping_vtable,
                                   find_mapping, bus);
  }
  if (r >= 0) {
    r = sd_bus_add_node_enumerator(bus->bus, nullptr, MAPPINGS_PATH,
                                   enumerate_mappings, bus);
  }
  if (r >= 0) {
    r = sd_bus_request_name(bus->bus, NAME, 0);
  }
  if (r < 0) {
    bus_stop(bus);
    return r;
  }
  return 0;
}

void bus_stop(bus_t *bus) { bus->bus = sd_bus_flush_close_unref(bus->bus); }

static char const *failure_name(nat_result_t result) {
  switch (result) {
  case NAT_RESULT_NOT_AUTHORIZED:
    return "not-authorized";
  case NAT_RESULT_NO_RESOURCES:
    return "no-resources";
  case NAT_RESULT_NETWORK_FAILURE:
    return "network-failure";
  case NAT_RESULT_UNREACHABLE:
    return "unreachable";
  case NAT_RESULT_OTHER:
    return "other";
  }
  unreachable();
}

void bus_mapping_changed(void *userdata, mapping_t const *mapping) {
  bus_t const *bus = userdata;
  int const r = sd_bus_emit_properties_changed(
      bus->bus, path_of(mapping->id).value, MAPPING_INTERFACE, "ExternalPort",
      nullptr);
  if (r < 0) {
    fprintf(stderr, "portd: announcing mapping %" PRIu64 ": %s\n", mapping->id,
            strerror(-r));
  }
}

void bus_mapping_removed(void *userdata,
                         mapping_t const *mapping,
                         nat_result_t const *failure) {
  bus_t const *bus = userdata;
  mapping_path_t const path = path_of(mapping->id);
  int r = 0;
  if (failure != nullptr) {
    r = sd_bus_emit_signal(bus->bus, path.value, MAPPING_INTERFACE, "Failed",
                           "s", failure_name(*failure));
  }
  if (r >= 0) {
    r = sd_bus_emit_object_removed(bus->bus, path.value);
  }
  if (r < 0) {
    fprintf(stderr, "portd: announcing mapping %" PRIu64 ": %s\n", mapping->id,
            strerror(-r));
  }
}

void bus_gateway_changed(void *userdata) {
  bus_t const *bus = userdata;
  int const r = sd_bus_emit_properties_changed(
      bus->bus, PATH, MANAGER_INTERFACE, "Gateway", "GatewayProtocol",
      "ExternalAddress", nullptr);
  if (r < 0) {
    fprintf(stderr, "portd: announcing the gateway: %s\n", strerror(-r));
  }
}
