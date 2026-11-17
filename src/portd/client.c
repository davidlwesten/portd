#include "client.h"

#include "errors.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#define SERVER_PORT 5351
#define ANNOUNCE_PORT 5350

// Until mappings can be requested over D-Bus
#define TEST_PORT 12345
#define TEST_LIFETIME 120

static bool fill_random(void * /*ctx*/, size_t size, uint8_t data[]) {
  return getrandom(data, size, 0) == (ssize_t)size;
}

static uint64_t now_ms(client_t const *client) {
  uint64_t usec = 0;
  sd_event_now(client->event, CLOCK_MONOTONIC, &usec);
  return usec / 1000;
}

// Called once the session has nothing more to do before `deadline`
static void wait_for(client_t *client, bool has_deadline, uint64_t deadline) {
  if (client->stopping && !has_deadline) {
    sd_event_exit(client->event, 0);
  }

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
    fprintf(stderr, "portd: scheduling the session: %s\n", strerror(-r));
  }
}

static void log_mapped(nat_event_t const *event) {
  char external[PCP_SOCKET_ADDRESS_STRING_LENGTH];
  pcp_socket_address_to_string(sizeof(external), external,
                               &event->mapped.external);
  fprintf(stderr, "portd: mapping %u reachable at %s\n",
          (unsigned)event->mapped.handle, external);
}

static void run(client_t *client) {
  if (client->nat == nullptr) {
    return;
  }
  int const fd = sd_event_source_get_io_fd(client->socket);
  uint64_t const now = now_ms(client);
  for (;;) {
    nat_event_t const event = nat_session_poll(client->nat, now);
    switch (event.type) {
    case NAT_EVENT_SEND:
      // A lost request is retransmitted anyway
      send(fd, event.send.data, event.send.size, 0);
      break;
    case NAT_EVENT_PROTOCOL:
      fprintf(stderr, "portd: the gateway doesn't speak PCP, trying NAT-PMP\n");
      break;
    case NAT_EVENT_ADDRESS: {
      char address[PCP_IP_ADDRESS_STRING_LENGTH];
      pcp_ip_address_to_string(sizeof(address), address,
                               &event.address.external);
      fprintf(stderr, "portd: external address %s\n", address);
      break;
    }
    case NAT_EVENT_MAPPED:
      log_mapped(&event);
      break;
    case NAT_EVENT_UNMAPPED:
      fprintf(stderr, "portd: mapping %u lost\n",
              (unsigned)event.unmapped.handle);
      break;
    case NAT_EVENT_FAILED:
      fprintf(stderr, "portd: mapping %u refused with result %u\n",
              (unsigned)event.failed.handle, (unsigned)event.failed.result);
      break;
    case NAT_EVENT_TIMEOUT:
      wait_for(client, event.timeout.deadline.has_value,
               event.timeout.deadline.value);
      return;
    default:
      break;
    }
  }
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
  r = sd_event_add_time(client->event, &client->timer, CLOCK_MONOTONIC, 0, 0,
                        on_timer, client);
  if (r < 0) {
    client_disconnect(client);
    return r;
  }

  client->nat = nat_session_begin((nat_config_t){
      .local = pcp_ip_address_from_in_addr(&local.sin_addr),
      .rng = {.fill = fill_random},
      .capacity = 1,
  });
  if (client->nat == nullptr) {
    client_disconnect(client);
    return -ENOMEM;
  }
  nat_handle_or_error_t const handle = nat_session_map_request(
      client->nat, TEST_LIFETIME, NAT_PROTOCOL_UDP, TEST_PORT, 0);
  if (!handle.has_value) {
    client_disconnect(client);
    return -handle.error;
  }
  client->mapping = handle.value;

  run(client);
  return 0;
}

void client_stop(client_t *client) {
  if (client->stopping || client->nat == nullptr) {
    sd_event_exit(client->event, 0);
    return;
  }
  client->stopping = true;
  nat_session_release(client->nat, client->mapping);
  run(client);
}

void client_disconnect(client_t *client) {
  if (client->nat != nullptr) {
    nat_session_end(client->nat);
    client->nat = nullptr;
  }
  client->socket = sd_event_source_disable_unref(client->socket);
  client->announcements = sd_event_source_disable_unref(client->announcements);
  client->timer = sd_event_source_disable_unref(client->timer);
}
