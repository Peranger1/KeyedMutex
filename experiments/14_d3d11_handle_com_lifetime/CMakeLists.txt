add_executable(
  exp14_d3d11_handle_com_lifetime
  main.cpp
)

target_link_libraries(
  exp14_d3d11_handle_com_lifetime
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp14_d3d11_handle_com_lifetime
    TEST_PREFIX "exp14."
    EXTRA_ARGS --timeout-ms 1000
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
