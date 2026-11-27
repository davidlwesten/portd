add_executable(portctl main.c bus.c map.c release.c status.c)
target_compile_definitions(portctl PRIVATE _GNU_SOURCE)
target_include_directories(portctl PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(portctl PRIVATE portd_warnings PkgConfig::libsystemd)

install(TARGETS portctl)
