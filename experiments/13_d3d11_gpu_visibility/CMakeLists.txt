add_executable(
  exp13_d3d11_gpu_visibility
  main.cpp
)

target_link_libraries(
  exp13_d3d11_gpu_visibility
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
    d3dcompiler
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp13_d3d11_gpu_visibility
    TEST_PREFIX "exp13."
    EXTRA_ARGS --iterations 30 --timeout-ms 5000
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()
