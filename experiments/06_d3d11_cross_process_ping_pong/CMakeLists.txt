add_executable(
  exp06_d3d11_cross_process_ping_pong
  main.cpp
)

target_link_libraries(
  exp06_d3d11_cross_process_ping_pong
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp06_d3d11_cross_process_ping_pong
    TEST_PREFIX "exp06."
    EXTRA_ARGS --iterations 100 --timeout-ms 5000
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
