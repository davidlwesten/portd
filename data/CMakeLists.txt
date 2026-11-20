set(PORTD_DBUS_DIR
    "${CMAKE_INSTALL_DATADIR}/dbus-1"
    CACHE PATH "Where the bus policy and activation file go")
set(PORTD_SYSTEMD_DIR
    "lib/systemd/system"
    CACHE PATH "Where the systemd unit goes")
set(PORTD_SYSUSERS_DIR
    "lib/sysusers.d"
    CACHE PATH "Where the sysusers.d file goes")

configure_file(io.github.davidlwesten.Portd1.service.in
               io.github.davidlwesten.Portd1.service @ONLY)
configure_file(portd.service.in portd.service @ONLY)

install(FILES io.github.davidlwesten.Portd1.conf
        DESTINATION "${PORTD_DBUS_DIR}/system.d")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/io.github.davidlwesten.Portd1.service"
        DESTINATION "${PORTD_DBUS_DIR}/system-services")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/portd.service"
        DESTINATION "${PORTD_SYSTEMD_DIR}")
install(FILES portd.sysusers
        DESTINATION "${PORTD_SYSUSERS_DIR}"
        RENAME portd.conf)
