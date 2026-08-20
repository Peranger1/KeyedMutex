add_executable(
  exp09_d3d11_multithread_context
  main.cpp
)

target_link_libraries(
  exp09_d3d11_multithread_context
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp09_d3d11_multithread_context
    TEST_PREFIX "exp09."
    EXTRA_ARGS --workers 4 --iterations 100 --no-debug-layer
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
