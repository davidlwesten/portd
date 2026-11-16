add_executable(portd main.c gateway.c)
target_compile_definitions(portd PRIVATE _GNU_SOURCE)
target_link_libraries(portd PRIVATE portd_warnings PkgConfig::libmnl
                                    PkgConfig::libsystemd)
