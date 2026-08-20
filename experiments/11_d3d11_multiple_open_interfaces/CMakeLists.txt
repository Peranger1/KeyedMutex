add_executable(
  exp11_d3d11_multiple_open_interfaces
  main.cpp
)

target_link_libraries(
  exp11_d3d11_multiple_open_interfaces
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp11_d3d11_multiple_open_interfaces
    TEST_PREFIX "exp11."
    EXTRA_ARGS --timeout-ms 250
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
