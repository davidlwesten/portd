add_executable(portd main.c)
target_link_libraries(portd PRIVATE portd_warnings PkgConfig::libsystemd)
