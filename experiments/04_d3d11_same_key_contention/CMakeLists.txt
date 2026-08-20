add_executable(
  exp04_d3d11_same_key_contention
  main.cpp
)

target_link_libraries(
  exp04_d3d11_same_key_contention
  PRIVATE
    keyed_mutex_d3d11_test_support
    GTest::gtest
)

if(BUILD_TESTING)
  gtest_discover_tests(
    exp04_d3d11_same_key_contention
    TEST_PREFIX "exp04."
    EXTRA_ARGS --rounds 300 --timeout-ms 5000 --poll-timeout-ms 50
    DISCOVERY_MODE PRE_TEST
    DISCOVERY_TIMEOUT 30
    PROPERTIES
      TIMEOUT 30
  )
endif()

