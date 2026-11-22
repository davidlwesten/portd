#pragma once

#include <stdint.h>
#include <time.h>

#include <systemd/sd-bus.h>

// The commands return the exit status. Each parses its own options, with the
// command as argv[0].
[[nodiscard]] int status(sd_bus *bus);
[[nodiscard]] int map(sd_bus *bus, int argc, char *argv[]);
// `text` is PORT/PROTOCOL
[[nodiscard]] int release(sd_bus *bus, char const *text);

void print_usage(void);

// Splits "12345/udp", leaving the protocol for portd to check
[[nodiscard]] bool
parse_port(char const *text, uint16_t *port, char const **protocol);

// In microseconds
uint64_t now(clockid_t clock);
