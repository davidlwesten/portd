add_library(nat STATIC nat.c)
target_include_directories(nat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nat PUBLIC byo::pcp PRIVATE byo::natpmp portd_warnings)
