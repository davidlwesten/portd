#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <systemd/sd-bus.h>

#include "portctl.h"

uint64_t now(clockid_t clock) {
  struct timespec time;
  clock_gettime(clock, &time);
  return (uint64_t)time.tv_sec * 1000000 + (uint64_t)time.tv_nsec / 1000;
}

static char const USAGE[] =
    "usage: portctl status\n"
    "       portctl map PORT/PROTOCOL [--lifetime DURATION]\n"
    "       portctl release PORT/PROTOCOL\n";

void print_usage(void) { fputs(USAGE, stderr); }

bool parse_port(char const *text, uint16_t *port, char const **protocol) {
  char *end;
  errno = 0;
  unsigned long const value = strtoul(text, &end, 10);
  if (errno != 0 || end == text || text[0] == '-') {
    return false;
  }
  if (value == 0 || value > UINT16_MAX) {
    return false;
  }
  if (*end != '/' || end[1] == '\0') {
    return false;
  }

  *port = (uint16_t)value;
  *protocol = &end[1];
  return true;
}

int main(int argc, char *argv[]) {
  bool const is_status = argc == 2 && strcmp(argv[1], "status") == 0;
  bool const is_map = argc >= 2 && strcmp(argv[1], "map") == 0;
  bool const is_release = argc == 3 && strcmp(argv[1], "release") == 0;
  if (!is_status && !is_map && !is_release) {
    print_usage();
    return 2;
  }

  sd_bus *bus = nullptr;
  int const r = sd_bus_open_system(&bus);
  if (r < 0) {
    fprintf(stderr, "portctl: connecting to the system bus: %s\n",
            strerror(-r));
    return EXIT_FAILURE;
  }
  int result;
  if (is_status) {
    result = status(bus);
  } else if (is_map) {
    result = map(bus, argc - 1, &argv[1]);
  } else {
    result = release(bus, argv[2]);
  }
  sd_bus_flush_close_unref(bus);
  return result;
}
