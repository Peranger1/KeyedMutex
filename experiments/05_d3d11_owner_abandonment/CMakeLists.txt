add_executable(
  exp05_d3d11_owner_abandonment
  main.cpp
)

target_link_libraries(
  exp05_d3d11_owner_abandonment
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp05_d3d11_owner_abandonment
    TEST_PREFIX "exp05."
    EXTRA_ARGS --iterations 3 --timeout-ms 5000
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()

