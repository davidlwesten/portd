#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-event.h>

#include "client.h"
#include "gateway.h"

static constexpr uint16_t CAPACITY = 64;

static void
on_gateway_changed(void *userdata, size_t family, gateway_t const *gateway) {
  if (family != GATEWAY_IPV4) {
    return;
  }
  int const r = client_connect(userdata, gateway);
  if (r < 0) {
    fprintf(stderr, "portd: connecting to the gateway: %s\n", strerror(-r));
  }
}

static int on_signal(sd_event_source * /*source*/,
                     struct signalfd_siginfo const * /*info*/,
                     void *userdata) {
  client_stop(userdata);
  return 0;
}

int main(void) {
  sd_event *event = nullptr;
  int r = sd_event_default(&event);
  if (r < 0) {
    fprintf(stderr, "portd: creating the event loop: %s\n", strerror(-r));
    return EXIT_FAILURE;
  }

  client_t client;
  r = client_begin(&client, event, CAPACITY);
  if (r < 0) {
    fprintf(stderr, "portd: starting the client: %s\n", strerror(-r));
    sd_event_unref(event);
    return EXIT_FAILURE;
  }
  // Until mappings can be requested over D-Bus
  r = client_map(&client, NAT_PROTOCOL_UDP, 12345, 120);
  if (r >= 0) {
    r = client_map(&client, NAT_PROTOCOL_TCP, 8080, 200);
  }
  if (r < 0) {
    fprintf(stderr, "portd: mapping the test ports: %s\n", strerror(-r));
    client_end(&client);
    sd_event_unref(event);
    return EXIT_FAILURE;
  }

  int const signals[] = {SIGTERM, SIGINT};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
    r = sd_event_add_signal(event, nullptr,
                            signals[i] | SD_EVENT_SIGNAL_PROCMASK, on_signal,
                            &client);
    if (r < 0) {
      fprintf(stderr, "portd: handling signals: %s\n", strerror(-r));
      client_end(&client);
      sd_event_unref(event);
      return EXIT_FAILURE;
    }
  }

  gateway_monitor_t gateways;
  r = gateway_monitor_start(&gateways, event, on_gateway_changed, &client);
  if (r < 0) {
    fprintf(stderr, "portd: following route changes: %s\n", strerror(-r));
    client_end(&client);
    sd_event_unref(event);
    return EXIT_FAILURE;
  }

  r = sd_event_loop(event);
  client_end(&client);
  sd_event_unref(event);
  gateway_monitor_stop(&gateways);
  if (r < 0) {
    fprintf(stderr, "portd: event loop: %s\n", strerror(-r));
    return EXIT_FAILURE;
  }
  return r;
}
