cmake_minimum_required(VERSION 3.24)

project(portd VERSION 0.1.0 LANGUAGES C)

option(PORTD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/warnings.cmake)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_COMPILE_WARNING_AS_ERROR ${PORTD_WARNINGS_AS_ERRORS})

find_package(PkgConfig REQUIRED)
pkg_check_modules(libmnl REQUIRED IMPORTED_TARGET libmnl)
pkg_check_modules(libsystemd REQUIRED IMPORTED_TARGET libsystemd>=253)

add_subdirectory(src/portd)
