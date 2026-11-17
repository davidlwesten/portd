#include "client.h"

#include "errors.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 5351
#define ANNOUNCE_PORT 5350

typedef struct mapping {
  // CLOCK_MONOTONIC milliseconds, 0 for a free slot
  uint64_t expires;
  nat_protocol_t protocol;
  uint16_t port;
  // 0 while it isn't requested from a gateway
  nat_handle_t handle;
} mapping_t;

static bool fill_random(void * /*ctx*/, size_t size, uint8_t data[]) {
  return getrandom(data, size, 0) == (ssize_t)size;
}

static uint64_t now_ms(client_t const *client) {
  uint64_t usec = 0;
  sd_event_now(client->event, CLOCK_MONOTONIC, &usec);
  return usec / 1000;
}

static char const *protocol_name(nat_protocol_t protocol) {
  switch (protocol) {
  case NAT_PROTOCOL_TCP:
    return "tcp";
  case NAT_PROTOCOL_UDP:
    return "udp";
  }
  unreachable();
}

static mapping_t *find_mapping(client_t *client, nat_handle_t handle) {
  for (size_t i = 0; i < client->capacity; i++) {
    mapping_t *mapping = &client->mappings[i];
    if (mapping->expires != 0 && mapping->handle == handle) {
      return mapping;
    }
  }
  return nullptr;
}

[[nodiscard]] static int
request(client_t *client, mapping_t *mapping, uint32_t lifetime) {
  nat_handle_or_error_t const handle = nat_session_map_request(
      client->nat, lifetime, mapping->protocol, mapping->port, 0);
  if (!handle.has_value) {
    return -handle.error;
  }
  mapping->handle = handle.value;
  return 0;
}

// Frees the slot, releasing the mapping on the gateway if it was requested
static void release(client_t *client, mapping_t *mapping) {
  if (mapping->handle != 0) {
    nat_session_release(client->nat, mapping->handle);
  }
  *mapping = (mapping_t){};
}

static void expire(client_t *client, uint64_t now) {
  for (size_t i = 0; i < client->capacity; i++) {
    mapping_t *mapping = &client->mappings[i];
    if (mapping->expires != 0 && mapping->expires <= now) {
      fprintf(stderr, "portd: %s/%u expired\n",
              protocol_name(mapping->protocol), (unsigned)mapping->port);
      release(client, mapping);
    }
  }
}

static void wait_until(client_t *client, bool has_deadline, uint64_t deadline) {
  int r = 0;
  if (has_deadline) {
    r = sd_event_source_set_time(client->timer, deadline * 1000);
    if (r >= 0) {
      r = sd_event_source_set_enabled(client->timer, SD_EVENT_ONESHOT);
    }
  } else {
    r = sd_event_source_set_enabled(client->timer, SD_EVENT_OFF);
  }
  if (r < 0) {
    fprintf(stderr, "portd: scheduling the timer: %s\n", strerror(-r));
  }
}

static void on_event(client_t *client, nat_event_t const *event) {
  mapping_t *mapping;
  switch (event->type) {
  case NAT_EVENT_PROTOCOL:
    fprintf(stderr, "portd: the gateway doesn't speak PCP, trying NAT-PMP\n");
    break;
  case NAT_EVENT_ADDRESS: {
    char address[PCP_IP_ADDRESS_STRING_LENGTH];
    pcp_ip_address_to_string(sizeof(address), address,
                             &event->address.external);
    fprintf(stderr, "portd: external address %s\n", address);
    break;
  }
  case NAT_EVENT_MAPPED: {
    mapping = find_mapping(client, event->mapped.handle);
    char external[PCP_SOCKET_ADDRESS_STRING_LENGTH];
    pcp_socket_address_to_string(sizeof(external), external,
                                 &event->mapped.external);
    fprintf(stderr, "portd: %s/%u reachable at %s\n",
            protocol_name(mapping->protocol), (unsigned)mapping->port,
            external);
    break;
  }
  case NAT_EVENT_UNMAPPED:
    mapping = find_mapping(client, event->unmapped.handle);
    fprintf(stderr, "portd: %s/%u lost, requesting it again\n",
            protocol_name(mapping->protocol), (unsigned)mapping->port);
    break;
  case NAT_EVENT_FAILED:
    mapping = find_mapping(client, event->failed.handle);
    fprintf(stderr, "portd: %s/%u failed with result %u\n",
            protocol_name(mapping->protocol), (unsigned)mapping->port,
            (unsigned)event->failed.result);
    *mapping = (mapping_t){};
    break;
  default:
    break;
  }
}

// Polls the session, returning when it next needs to be polled
static void poll_session(client_t *client,
                         uint64_t now,
                         bool *has_deadline,
                         uint64_t *deadline) {
  int const fd = sd_event_source_get_io_fd(client->socket);
  for (;;) {
    nat_event_t const event = nat_session_poll(client->nat, now);
    if (event.type == NAT_EVENT_TIMEOUT) {
      *has_deadline = event.timeout.deadline.has_value;
      *deadline = event.timeout.deadline.value;
      return;
    }
    if (event.type == NAT_EVENT_SEND) {
      // A lost request is retransmitted anyway
      send(fd, event.send.data, event.send.size, 0);
    } else {
      on_event(client, &event);
    }
  }
}

static void run(client_t *client) {
  uint64_t const now = now_ms(client);
  expire(client, now);

  bool has_deadline = false;
  uint64_t deadline = 0;
  if (client->nat != nullptr) {
    poll_session(client, now, &has_deadline, &deadline);
  }
  if (client->stopping && !has_deadline) {
    sd_event_exit(client->event, 0);
    return;
  }

  for (size_t i = 0; i < client->capacity; i++) {
    uint64_t const expires = client->mappings[i].expires;
    if (expires != 0 && (!has_deadline || expires < deadline)) {
      has_deadline = true;
      deadline = expires;
    }
  }
  wait_until(client, has_deadline, deadline);
}

static int on_datagram(sd_event_source * /*source*/,
                       int fd,
                       uint32_t /*revents*/,
                       void *userdata) {
  client_t *client = userdata;

  uint64_t const now = now_ms(client);
  uint8_t buffer[NAT_PACKET_MAX_SIZE];
  for (;;) {
    ssize_t const size = recv(fd, buffer, sizeof(buffer), 0);
    if (size >= 0) {
      nat_session_on_datagram(client->nat, now, (size_t)size, buffer);
    } else if (errno == ECONNREFUSED) {
      if (nat_session_protocol(client->nat) == NAT_GATEWAY_PROTOCOL_PCP) {
        fprintf(stderr, "portd: the gateway refused PCP\n");
      }
      nat_session_on_host_unreachable(client->nat);
    } else {
      if (errno != EAGAIN) {
        fprintf(stderr, "portd: receiving from the gateway: %s\n",
                strerror(errno));
      }
      break;
    }
  }

  run(client);
  return 0;
}

static int
on_timer(sd_event_source * /*source*/, uint64_t /*usec*/, void *userdata) {
  run(userdata);
  return 0;
}

// The socket is owned by `*source`
[[nodiscard]] static int add_socket(client_t *client,
                                    sd_event_source **source) {
  int const fd =
      errno_or(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (fd < 0) {
    return fd;
  }
  int r =
      sd_event_add_io(client->event, source, fd, EPOLLIN, on_datagram, client);
  if (r < 0) {
    close(fd);
    return r;
  }
  r = sd_event_source_set_io_fd_own(*source, true);
  if (r < 0) {
    close(fd);
    return r;
  }
  return fd;
}

[[nodiscard]] static int connect_to_server(int fd, gateway_t const *gateway) {
  struct sockaddr_in server = {
      .sin_family = AF_INET,
      .sin_port = htons(SERVER_PORT),
  };
  memcpy(&server.sin_addr, gateway->address, sizeof(server.sin_addr));
  return errno_or(connect(fd, (struct sockaddr *)&server, sizeof(server)));
}

[[nodiscard]] static int open_socket(client_t *client,
                                     gateway_t const *gateway,
                                     struct sockaddr_in *local) {
  int const fd = add_socket(client, &client->socket);
  if (fd < 0) {
    return fd;
  }
  int const r = connect_to_server(fd, gateway);
  if (r < 0) {
    return r;
  }
  socklen_t size = sizeof(*local);
  return errno_or(getsockname(fd, (struct sockaddr *)local, &size));
}

// The server announces from its own port to 224.0.0.1, so connecting to it
// leaves out everyone else's announcements
[[nodiscard]] static int open_announcements(client_t *client,
                                            gateway_t const *gateway) {
  int const fd = add_socket(client, &client->announcements);
  if (fd < 0) {
    return fd;
  }

  // Other clients on this host may be listening too
  int const on = 1;
  int r = errno_or(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)));
  if (r < 0) {
    return r;
  }
  // Only the group joined on the gateway's interface below
  int const off = 0;
  r = errno_or(setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &off, sizeof(off)));
  if (r < 0) {
    return r;
  }

  struct sockaddr_in const group = {
      .sin_family = AF_INET,
      .sin_port = htons(ANNOUNCE_PORT),
      .sin_addr.s_addr = htonl(INADDR_ALLHOSTS_GROUP),
  };
  r = errno_or(bind(fd, (struct sockaddr const *)&group, sizeof(group)));
  if (r < 0) {
    return r;
  }
  struct ip_mreqn const membership = {
      .imr_multiaddr = group.sin_addr,
      .imr_ifindex = (int)gateway->ifindex,
  };
  r = errno_or(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                          sizeof(membership)));
  if (r < 0) {
    return r;
  }
  return connect_to_server(fd, gateway);
}

static void client_disconnect(client_t *client) {
  if (client->nat != nullptr) {
    nat_session_end(client->nat);
    client->nat = nullptr;
  }
  for (size_t i = 0; i < client->capacity; i++) {
    client->mappings[i].handle = 0;
  }
  client->socket = sd_event_source_disable_unref(client->socket);
  client->announcements = sd_event_source_disable_unref(client->announcements);
}

int client_begin(client_t *client, sd_event *event, uint16_t capacity) {
  assert(capacity > 0);
  *client = (client_t){.event = event, .capacity = capacity};
  client->mappings = calloc(capacity, sizeof(client->mappings[0]));
  if (client->mappings == nullptr) {
    return -ENOMEM;
  }
  int r = sd_event_add_time(event, &client->timer, CLOCK_MONOTONIC, 0, 0,
                            on_timer, client);
  if (r >= 0) {
    r = sd_event_source_set_enabled(client->timer, SD_EVENT_OFF);
  }
  if (r < 0) {
    client_end(client);
    return r;
  }
  return 0;
}

void client_end(client_t *client) {
  client_disconnect(client);
  client->timer = sd_event_source_disable_unref(client->timer);
  free(client->mappings);
  client->mappings = nullptr;
}

int client_map(client_t *client,
               nat_protocol_t protocol,
               uint16_t port,
               uint32_t lifetime) {
  assert(lifetime > 0);
  for (size_t i = 0; i < client->capacity; i++) {
    mapping_t *mapping = &client->mappings[i];
    if (mapping->expires != 0) {
      continue;
    }

    *mapping = (mapping_t){
        .expires = now_ms(client) + (uint64_t)lifetime * 1000,
        .protocol = protocol,
        .port = port,
    };
    if (client->nat != nullptr) {
      int const r = request(client, mapping, lifetime);
      if (r < 0) {
        *mapping = (mapping_t){};
        return r;
      }
    }
    run(client);
    return 0;
  }
  return -ENOBUFS;
}

// Asks the new gateway for every mapping, for the time each has left
static void request_all(client_t *client) {
  uint64_t const now = now_ms(client);
  for (size_t i = 0; i < client->capacity; i++) {
    mapping_t *mapping = &client->mappings[i];
    if (mapping->expires == 0 || mapping->expires <= now) {
      continue;
    }
    uint64_t const left = (mapping->expires - now + 999) / 1000;
    int const r = request(client, mapping,
                          left < UINT32_MAX ? (uint32_t)left : UINT32_MAX);
    if (r < 0) {
      fprintf(stderr, "portd: requesting %s/%u: %s\n",
              protocol_name(mapping->protocol), (unsigned)mapping->port,
              strerror(-r));
      *mapping = (mapping_t){};
    }
  }
}

int client_connect(client_t *client, gateway_t const *gateway) {
  client_disconnect(client);
  if (client->stopping) {
    // The mappings on the old gateway can't be released anymore
    sd_event_exit(client->event, 0);
    return 0;
  }
  if (gateway->ifindex == 0) {
    return 0;
  }

  struct sockaddr_in local;
  int r = open_socket(client, gateway, &local);
  if (r < 0) {
    client_disconnect(client);
    return r;
  }
  r = open_announcements(client, gateway);
  if (r < 0) {
    fprintf(stderr, "portd: not listening for announcements: %s\n",
            strerror(-r));
    client->announcements =
        sd_event_source_disable_unref(client->announcements);
  }

  client->nat = nat_session_begin((nat_config_t){
      .local = pcp_ip_address_from_in_addr(&local.sin_addr),
      .rng = {.fill = fill_random},
      .capacity = client->capacity,
  });
  if (client->nat == nullptr) {
    client_disconnect(client);
    return -ENOMEM;
  }
  request_all(client);
  run(client);
  return 0;
}

void client_stop(client_t *client) {
  if (client->stopping || client->nat == nullptr) {
    sd_event_exit(client->event, 0);
    return;
  }
  client->stopping = true;
  for (size_t i = 0; i < client->capacity; i++) {
    if (client->mappings[i].expires != 0) {
      release(client, &client->mappings[i]);
    }
  }
  run(client);
}
