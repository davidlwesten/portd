# portd

A daemon that keeps port mappings open for this machine on its gateway. It
follows the default gateway over netlink and speaks PCP, falling back to
NAT-PMP when that's all the gateway supports. Applications request
mappings over D-Bus, and `portctl` does the same from the command line.

portd is in early development.

## Usage

```sh
portctl map 12345/udp --lifetime 2h
portctl status
portctl release 12345/udp
```

`map` waits for the gateway and prints the external address it granted. A
mapping lasts an hour unless `--lifetime` says otherwise, and asking again
extends it.

## Building

portd needs a C23 compiler, CMake 3.28 or newer, libmnl, and libsystemd 253 or
newer. [libpcp](https://github.com/byo-io/libpcp) and
[libnatpmp](https://github.com/byo-io/libnatpmp) are used when installed, and
fetched otherwise.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Installing

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
sudo systemd-sysusers
sudo systemctl reload dbus
```

portd runs as the `portd` user and is started by the bus when something first
asks for a mapping.

## License

MIT, see [LICENSE](LICENSE).
