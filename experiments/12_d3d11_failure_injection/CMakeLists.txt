add_executable(
  exp12_d3d11_failure_injection
  main.cpp
)

target_link_libraries(
  exp12_d3d11_failure_injection
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp12_d3d11_failure_injection
    TEST_PREFIX "exp12."
    EXTRA_ARGS --timeout-ms 1000
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
