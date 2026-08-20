add_executable(
  exp03_d3d11_three_device_ring
  main.cpp
)

target_link_libraries(
  exp03_d3d11_three_device_ring
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp03_d3d11_three_device_ring
    TEST_PREFIX "exp03."
    EXTRA_ARGS --iterations 200 --timeout-ms 5000
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
