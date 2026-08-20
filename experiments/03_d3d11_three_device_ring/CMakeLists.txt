add_executable(
  exp03_d3d11_three_device_ring
  main.cpp
)

target_link_libraries(
  exp03_d3d11_three_device_ring
  PRIVATE
    keyed_mutex_d3d11_test_support
)

if(BUILD_TESTING)
  add_test(
    NAME exp03.d3d11_three_device_ring
    COMMAND exp03_d3d11_three_device_ring --iterations 200 --timeout-ms 5000
  )
  set_tests_properties(
    exp03.d3d11_three_device_ring
    PROPERTIES
      TIMEOUT 30
  )
endif()

