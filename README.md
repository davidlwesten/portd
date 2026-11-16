# portd

A daemon that manages port mappings for this machine on its gateway, using
PCP or NAT-PMP, and `portctl`, a CLI that talks to it over D-Bus.

## Building

portd needs a C23 compiler, CMake 3.24 or newer and libsystemd 253 or newer.

```sh
cmake -S . -B build
cmake --build build
```

## License

MIT, see [LICENSE](LICENSE).
