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

#define PCP_SERVER_PORT 5351

// Until mappings can be requested over D-Bus
#define TEST_PORT 12345
#define TEST_LIFETIME 120

static bool fill_random(void * /*ctx*/, size_t size, uint8_t data[]) {
  return getrandom(data, size, 0) == (ssize_t)size;
}

static pcp_time_ms_t now_ms(client_t const *client) {
  uint64_t usec = 0;
  sd_event_now(client->event, CLOCK_MONOTONIC, &usec);
  return usec / 1000;
}

static void schedule(client_t *client, pcp_optional_time_ms_t deadline) {
  int r = 0;
  if (deadline.has_value) {
    r = sd_event_source_set_time(client->timer, deadline.value * 1000);
    if (r >= 0) {
      r = sd_event_source_set_enabled(client->timer, SD_EVENT_ONESHOT);
    }
  } else {
    r = sd_event_source_set_enabled(client->timer, SD_EVENT_OFF);
  }
  if (r < 0) {
    fprintf(stderr, "portd: scheduling the PCP session: %s\n", strerror(-r));
  }
}

static void run(client_t *client) {
  int const fd = sd_event_source_get_io_fd(client->socket);
  pcp_time_ms_t const now = now_ms(client);
  for (;;) {
    pcp_event_t const event = pcp_session_poll(client->session, now);
    switch (event.type) {
    case PCP_EVENT_SEND:
      // A lost request is retransmitted anyway
      send(fd, event.send.data, event.send.size, 0);
      break;
    case PCP_EVENT_MAPPED: {
      char external[PCP_SOCKET_ADDRESS_STRING_LENGTH];
      pcp_socket_address_to_string(sizeof(external), external,
                                   &event.mapped.external);
      fprintf(stderr, "portd: mapping %u reachable at %s\n",
              (unsigned)event.mapped.handle, external);
      break;
    }
    case PCP_EVENT_UNMAPPED:
      fprintf(stderr, "portd: mapping %u lost\n",
              (unsigned)event.unmapped.handle);
      break;
    case PCP_EVENT_FAILED:
      fprintf(stderr, "portd: mapping %u refused with result %u\n",
              (unsigned)event.failed.handle, (unsigned)event.failed.result);
      break;
    case PCP_EVENT_TIMEOUT:
      schedule(client, event.timeout.deadline);
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

  pcp_time_ms_t const now = now_ms(client);
  uint8_t buffer[PCP_PACKET_MAX_SIZE];
  ssize_t size;
  while ((size = recv(fd, buffer, sizeof(buffer), 0)) >= 0) {
    pcp_session_on_datagram(client->session, now, (size_t)size, buffer);
  }
  if (errno != EAGAIN) {
    fprintf(stderr, "portd: receiving from the gateway: %s\n", strerror(errno));
  }

  run(client);
  return 0;
}

static int
on_timer(sd_event_source * /*source*/, uint64_t /*usec*/, void *userdata) {
  run(userdata);
  return 0;
}

[[nodiscard]] static int open_socket(client_t *client,
                                     gateway_t const *gateway,
                                     struct sockaddr_in *local) {
  int const fd =
      errno_or(socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
  if (fd < 0) {
    return fd;
  }
  int r = sd_event_add_io(client->event, &client->socket, fd, EPOLLIN,
                          on_datagram, client);
  if (r < 0) {
    close(fd);
    return r;
  }
  r = sd_event_source_set_io_fd_own(client->socket, true);
  if (r < 0) {
    close(fd);
    return r;
  }

  struct sockaddr_in server = {
      .sin_family = AF_INET,
      .sin_port = htons(PCP_SERVER_PORT),
  };
  memcpy(&server.sin_addr, gateway->address, sizeof(server.sin_addr));
  r = errno_or(connect(fd, (struct sockaddr *)&server, sizeof(server)));
  if (r < 0) {
    return r;
  }
  socklen_t size = sizeof(*local);
  return errno_or(getsockname(fd, (struct sockaddr *)local, &size));
}

int client_connect(client_t *client, gateway_t const *gateway) {
  client_disconnect(client);
  if (gateway->ifindex == 0) {
    return 0;
  }

  struct sockaddr_in local;
  int r = open_socket(client, gateway, &local);
  if (r < 0) {
    client_disconnect(client);
    return r;
  }
  r = sd_event_add_time(client->event, &client->timer, CLOCK_MONOTONIC, 0, 0,
                        on_timer, client);
  if (r < 0) {
    client_disconnect(client);
    return r;
  }

  client->session = pcp_session_begin((pcp_config_t){
      .local = pcp_ip_address_from_in_addr(&local.sin_addr),
      .rng = {.fill = fill_random},
      .buffer = {.size = sizeof(client->buffer), .data = client->buffer},
  });
  if (client->session == nullptr) {
    client_disconnect(client);
    return -ENOMEM;
  }

  pcp_handle_or_error_t const handle = pcp_session_map_request(
      client->session, TEST_LIFETIME, PCP_PROTOCOL_UDP, TEST_PORT,
      (pcp_socket_address_t){.ip = pcp_ip_address_any_v4}, 0, nullptr);
  if (!handle.has_value) {
    client_disconnect(client);
    return -handle.error;
  }

  run(client);
  return 0;
}

void client_disconnect(client_t *client) {
  if (client->session != nullptr) {
    pcp_session_end(client->session);
    client->session = nullptr;
  }
  client->socket = sd_event_source_disable_unref(client->socket);
  client->timer = sd_event_source_disable_unref(client->timer);
}
