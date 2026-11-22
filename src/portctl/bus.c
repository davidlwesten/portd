#include <string.h>

#include "bus.h"
#include "portd1.h"

// Reads one property of a Mapping interface, skipping those it doesn't know
[[nodiscard]] static int read_property(sd_bus_message *reply,
                                       mapping_t *mapping) {
  char const *name;
  int const r = sd_bus_message_read(reply, "s", &name);
  if (r < 0) {
    return r;
  }

  if (strcmp(name, "Protocol") == 0) {
    return sd_bus_message_read(reply, "v", "s", &mapping->protocol);
  }
  if (strcmp(name, "Port") == 0) {
    return sd_bus_message_read(reply, "v", "q", &mapping->port);
  }
  if (strcmp(name, "Owner") == 0) {
    return sd_bus_message_read(reply, "v", "u", &mapping->owner);
  }
  if (strcmp(name, "Expires") == 0) {
    return sd_bus_message_read(reply, "v", "t", &mapping->expires);
  }
  if (strcmp(name, "ExternalPort") == 0) {
    return sd_bus_message_read(reply, "v", "q", &mapping->external_port);
  }
  return sd_bus_message_skip(reply, "v");
}

int read_mapping(sd_bus_message *reply, mapping_t *mapping) {
  int r = sd_bus_message_enter_container(reply, 'a', "{sv}");
  if (r < 0) {
    return r;
  }

  for (;;) {
    r = sd_bus_message_enter_container(reply, 'e', "sv");
    if (r < 0) {
      return r;
    }
    if (r == 0) {
      return sd_bus_message_exit_container(reply);
    }

    r = read_property(reply, mapping);
    if (r < 0) {
      return r;
    }
    r = sd_bus_message_exit_container(reply);
    if (r < 0) {
      return r;
    }
  }
}

// One interface of the object at `path`, with its properties
[[nodiscard]] static int read_interface(sd_bus_message *reply,
                                        char const *path,
                                        visit_t *visit,
                                        void *userdata) {
  char const *interface;
  int r = sd_bus_message_read(reply, "s", &interface);
  if (r < 0) {
    return r;
  }
  if (strcmp(interface, PORTD1_MAPPING_INTERFACE) != 0) {
    return sd_bus_message_skip(reply, "a{sv}");
  }

  mapping_t mapping = {.protocol = ""};
  r = read_mapping(reply, &mapping);
  if (r < 0) {
    return r;
  }
  visit(userdata, path, &mapping);
  return 0;
}

// One object's path and its interfaces
[[nodiscard]] static int
read_object(sd_bus_message *reply, visit_t *visit, void *userdata) {
  char const *path;
  int r = sd_bus_message_read(reply, "o", &path);
  if (r < 0) {
    return r;
  }
  r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
  if (r < 0) {
    return r;
  }

  for (;;) {
    r = sd_bus_message_enter_container(reply, 'e', "sa{sv}");
    if (r < 0) {
      return r;
    }
    if (r == 0) {
      return sd_bus_message_exit_container(reply);
    }

    r = read_interface(reply, path, visit, userdata);
    if (r < 0) {
      return r;
    }
    r = sd_bus_message_exit_container(reply);
    if (r < 0) {
      return r;
    }
  }
}

int for_each_mapping(sd_bus_message *reply, visit_t *visit, void *userdata) {
  int r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
  if (r < 0) {
    return r;
  }

  for (;;) {
    r = sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}");
    if (r < 0) {
      return r;
    }
    if (r == 0) {
      return sd_bus_message_exit_container(reply);
    }

    r = read_object(reply, visit, userdata);
    if (r < 0) {
      return r;
    }
    r = sd_bus_message_exit_container(reply);
    if (r < 0) {
      return r;
    }
  }
}

int get_mappings(sd_bus *bus, sd_bus_error *error, sd_bus_message **reply) {
  return sd_bus_call_method(bus, PORTD1_NAME, PORTD1_PATH,
                            "org.freedesktop.DBus.ObjectManager",
                            "GetManagedObjects", error, reply, "");
}
int get_string(sd_bus *bus,
               char const *property,
               sd_bus_error *error,
               char **value) {
  return sd_bus_get_property_string(bus, PORTD1_NAME, PORTD1_PATH,
                                    PORTD1_MANAGER_INTERFACE, property, error,
                                    value);
}
