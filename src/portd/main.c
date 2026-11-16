#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <systemd/sd-event.h>

#include "gateway.h"

int main(void) {
  sd_event *event = nullptr;
  int r = sd_event_default(&event);
  if (r < 0) {
    fprintf(stderr, "portd: creating the event loop: %s\n", strerror(-r));
    return EXIT_FAILURE;
  }

  // Without a handler, the signal exits the loop with the userdata as the code
  int const signals[] = {SIGTERM, SIGINT};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
    r = sd_event_add_signal(event, nullptr,
                            signals[i] | SD_EVENT_SIGNAL_PROCMASK, nullptr,
                            nullptr);
    if (r < 0) {
      fprintf(stderr, "portd: handling signals: %s\n", strerror(-r));
      sd_event_unref(event);
      return EXIT_FAILURE;
    }
  }

  gateway_monitor_t gateways;
  r = gateway_monitor_start(&gateways, event);
  if (r < 0) {
    fprintf(stderr, "portd: following route changes: %s\n", strerror(-r));
    sd_event_unref(event);
    return EXIT_FAILURE;
  }

  r = sd_event_loop(event);
  sd_event_unref(event);
  gateway_monitor_stop(&gateways);
  if (r < 0) {
    fprintf(stderr, "portd: event loop: %s\n", strerror(-r));
    return EXIT_FAILURE;
  }
  return r;
}
