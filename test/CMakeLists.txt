include(FetchContent)
FetchContent_Declare(
  unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG v2.7.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(unity)

# A copy of the module built with the sanitizers, so its memory errors and
# undefined behavior fail the tests
add_executable(nat_test nat.c ${PROJECT_SOURCE_DIR}/lib/nat.c)
target_include_directories(nat_test PRIVATE ${PROJECT_SOURCE_DIR}/lib)
target_link_libraries(nat_test PRIVATE portd_warnings byo::natpmp byo::pcp
                                       unity::framework)
target_compile_options(nat_test PRIVATE -fsanitize=address,undefined
                       -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
target_link_options(nat_test PRIVATE -fsanitize=address,undefined)
add_test(NAME nat COMMAND nat_test)
