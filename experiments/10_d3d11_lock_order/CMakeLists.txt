add_executable(
  exp10_d3d11_lock_order
  main.cpp
)

target_link_libraries(
  exp10_d3d11_lock_order
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp10_d3d11_lock_order
    TEST_PREFIX "exp10."
    EXTRA_ARGS --rounds 100 --timeout-ms 250
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
