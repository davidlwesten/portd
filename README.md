# portd

A daemon that keeps port mappings open for this machine on its gateway. It
follows the default gateway over netlink and speaks PCP, falling back to
NAT-PMP when that's all the gateway supports. Applications will request
mappings over D-Bus, and `portctl` will do the same from the command line.

portd is in early development: the daemon maps a fixed test port, and the
D-Bus interface and `portctl` are still to come.

## Building

portd needs a C23 compiler, CMake 3.24 or newer, libmnl, and libsystemd 253 or
newer. [libpcp](https://github.com/byo-io/libpcp) and
[libnatpmp](https://github.com/byo-io/libnatpmp) are used when installed, and
fetched otherwise.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## License

MIT, see [LICENSE](LICENSE).
