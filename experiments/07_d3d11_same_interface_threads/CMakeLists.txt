add_executable(
  exp07_d3d11_same_interface_threads
  main.cpp
)

target_link_libraries(
  exp07_d3d11_same_interface_threads
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp07_d3d11_same_interface_threads
    TEST_PREFIX "exp07."
    EXTRA_ARGS --rounds 100 --waiters 8 --timeout-ms 1000 --poll-timeout-ms 25
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
