add_executable(
  exp08_d3d11_flush_visibility
  main.cpp
)

target_link_libraries(
  exp08_d3d11_flush_visibility
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp08_d3d11_flush_visibility
    TEST_PREFIX "exp08."
    EXTRA_ARGS --iterations 300 --timeout-ms 5000
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
