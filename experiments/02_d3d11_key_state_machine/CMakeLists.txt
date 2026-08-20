add_executable(
  exp02_d3d11_key_state_machine
  main.cpp
)

target_link_libraries(
  exp02_d3d11_key_state_machine
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp02_d3d11_key_state_machine
    TEST_PREFIX "exp02."
    EXTRA_ARGS --timeout-ms 25
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 10
    PROPERTIES
      TIMEOUT 10
  )
endif()
