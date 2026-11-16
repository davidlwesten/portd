#include "gateway.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include <linux/rtnetlink.h>

typedef struct candidate {
  gateway_t gateway;
  uint32_t priority;
} candidate_t;

static int on_route_attr(struct nlattr const *attr, void *data) {
  struct nlattr const **attrs = data;
  if (mnl_attr_type_valid(attr, RTA_MAX) >= 0) {
    attrs[mnl_attr_get_type(attr)] = attr;
  }
  return MNL_CB_OK;
}

// A missing attribute leaves `value` as is
static bool get_u32(struct nlattr const *attr, uint32_t *value) {
  if (attr == nullptr) {
    return true;
  }
  if (mnl_attr_validate(attr, MNL_TYPE_U32) < 0) {
    return false;
  }
  *value = mnl_attr_get_u32(attr);
  return true;
}

static int on_route(struct nlmsghdr const *nlh, void *data) {
  candidate_t *candidates = data;
  struct rtmsg const *rtm = mnl_nlmsg_get_payload(nlh);
  if (rtm->rtm_dst_len != 0 || rtm->rtm_type != RTN_UNICAST) {
    return MNL_CB_OK;
  }

  size_t family;
  size_t address_size;
  if (rtm->rtm_family == AF_INET) {
    family = GATEWAY_IPV4;
    address_size = 4;
  } else if (rtm->rtm_family == AF_INET6) {
    family = GATEWAY_IPV6;
    address_size = 16;
  } else {
    return MNL_CB_OK;
  }

  struct nlattr const *attrs[RTA_MAX + 1] = {};
  if (mnl_attr_parse(nlh, sizeof(*rtm), on_route_attr, attrs) < 0) {
    return MNL_CB_OK;
  }
  uint32_t table = rtm->rtm_table;
  uint32_t priority = 0;
  uint32_t ifindex = 0;
  struct nlattr const *gateway = attrs[RTA_GATEWAY];
  if (!get_u32(attrs[RTA_TABLE], &table) ||
      !get_u32(attrs[RTA_PRIORITY], &priority) ||
      !get_u32(attrs[RTA_OIF], &ifindex) || gateway == nullptr ||
      mnl_attr_get_payload_len(gateway) != address_size) {
    return MNL_CB_OK;
  }

  candidate_t *best = &candidates[family];
  if (table != RT_TABLE_MAIN || gateway == nullptr || ifindex == 0 ||
      (best->gateway.ifindex != 0 && priority >= best->priority)) {
    return MNL_CB_OK;
  }
  *best = (candidate_t){.gateway.ifindex = ifindex, .priority = priority};
  memcpy(best->gateway.address, mnl_attr_get_payload(gateway), address_size);
  return MNL_CB_OK;
}

static int dump_routes(candidate_t candidates[GATEWAY_FAMILIES]) {
  struct mnl_socket *socket = mnl_socket_open2(NETLINK_ROUTE, SOCK_CLOEXEC);
  if (socket == nullptr) {
    return -errno;
  }
  int r = 0;
  if (mnl_socket_bind(socket, 0, MNL_SOCKET_AUTOPID) < 0) {
    r = -errno;
    goto out;
  }

  alignas(struct nlmsghdr) char buffer[MNL_SOCKET_DUMP_SIZE];
  struct nlmsghdr *nlh = mnl_nlmsg_put_header(buffer);
  nlh->nlmsg_type = RTM_GETROUTE;
  nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  nlh->nlmsg_seq = 1;
  struct rtmsg *rtm = mnl_nlmsg_put_extra_header(nlh, sizeof(*rtm));
  rtm->rtm_family = AF_UNSPEC;
  if (mnl_socket_sendto(socket, nlh, nlh->nlmsg_len) < 0) {
    r = -errno;
    goto out;
  }

  unsigned const portid = mnl_socket_get_portid(socket);
  int status;
  do {
    ssize_t const size = mnl_socket_recvfrom(socket, buffer, sizeof(buffer));
    if (size < 0) {
      r = -errno;
      goto out;
    }
    status = mnl_cb_run(buffer, (size_t)size, 1, portid, on_route, candidates);
  } while (status > MNL_CB_STOP);
  if (status < 0) {
    r = -errno;
  }

out:
  mnl_socket_close(socket);
  return r;
}

static void log_gateway(size_t family, gateway_t const *gateway) {
  char const *name = family == GATEWAY_IPV4 ? "IPv4" : "IPv6";
  if (gateway->ifindex == 0) {
    fprintf(stderr, "portd: no %s gateway\n", name);
    return;
  }

  char address[INET6_ADDRSTRLEN];
  inet_ntop(family == GATEWAY_IPV4 ? AF_INET : AF_INET6, gateway->address,
            address, sizeof(address));
  char ifname[IF_NAMESIZE];
  if (if_indextoname(gateway->ifindex, ifname) == nullptr) {
    snprintf(ifname, sizeof(ifname), "%u", gateway->ifindex);
  }
  fprintf(stderr, "portd: %s gateway %s on %s\n", name, address, ifname);
}

static bool gateway_equal(gateway_t const *a, gateway_t const *b) {
  return a->ifindex == b->ifindex &&
         memcmp(a->address, b->address, sizeof(a->address)) == 0;
}

static void sync_gateways(gateway_monitor_t *monitor) {
  candidate_t candidates[GATEWAY_FAMILIES] = {};
  int const r = dump_routes(candidates);
  if (r < 0) {
    // The change that interrupted the dump also wakes the monitor again
    fprintf(stderr, "portd: dumping routes: %s\n", strerror(-r));
    return;
  }

  for (size_t family = 0; family < GATEWAY_FAMILIES; family++) {
    gateway_t const *gateway = &candidates[family].gateway;
    if (!gateway_equal(gateway, &monitor->gateways[family])) {
      monitor->gateways[family] = *gateway;
      log_gateway(family, gateway);
    }
  }
}

static int on_route_change(sd_event_source * /*source*/,
                           int fd,
                           uint32_t /*revents*/,
                           void *userdata) {

  // What changed doesn't matter, the gateways come from a fresh dump. An
  // overflowed socket (ENOBUFS) just means more changes.
  char buffer[MNL_SOCKET_BUFFER_SIZE];
  while (recv(fd, buffer, sizeof(buffer), 0) >= 0 || errno == ENOBUFS) {
  }
  if (errno != EAGAIN) {
    fprintf(stderr, "portd: reading route changes: %s\n", strerror(errno));
  }

  sync_gateways(userdata);
  return 0;
}

int gateway_monitor_start(gateway_monitor_t *monitor, sd_event *event) {
  *monitor = (gateway_monitor_t){};
  monitor->socket =
      mnl_socket_open2(NETLINK_ROUTE, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (monitor->socket == nullptr) {
    return -errno;
  }

  // Subscribed before the first dump, so no change in between gets lost
  int r = 0;
  if (mnl_socket_bind(monitor->socket, RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE,
                      MNL_SOCKET_AUTOPID) < 0) {
    r = -errno;
  } else {
    r = sd_event_add_io(event, nullptr, mnl_socket_get_fd(monitor->socket),
                        EPOLLIN, on_route_change, monitor);
  }
  if (r < 0) {
    mnl_socket_close(monitor->socket);
    monitor->socket = nullptr;
    return r;
  }

  sync_gateways(monitor);
  return 0;
}

void gateway_monitor_stop(gateway_monitor_t *monitor) {
  if (monitor->socket != nullptr) {
    mnl_socket_close(monitor->socket);
    monitor->socket = nullptr;
  }
}
