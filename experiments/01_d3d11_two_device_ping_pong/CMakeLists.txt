add_executable(
  exp01_d3d11_two_device_ping_pong
  main.cpp
)

target_link_libraries(
  exp01_d3d11_two_device_ping_pong
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp01_d3d11_two_device_ping_pong
    TEST_PREFIX "exp01."
    EXTRA_ARGS --iterations 250 --timeout-ms 5000
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
