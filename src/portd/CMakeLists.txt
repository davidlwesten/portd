add_executable(portd main.c bus.c client.c gateway.c)
target_compile_definitions(portd PRIVATE _GNU_SOURCE)
target_include_directories(portd PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(portd PRIVATE portd_warnings nat PkgConfig::libmnl
                                    PkgConfig::libsystemd)

install(TARGETS portd)
