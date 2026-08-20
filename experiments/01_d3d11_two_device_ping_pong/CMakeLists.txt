add_executable(
  exp01_d3d11_two_device_ping_pong
  main.cpp
)

target_link_libraries(
  exp01_d3d11_two_device_ping_pong
  PRIVATE
    keyed_mutex_project_options
    d3d11
    dxgi
)

if(BUILD_TESTING)
  add_test(
    NAME exp01.d3d11_two_device_ping_pong
    COMMAND exp01_d3d11_two_device_ping_pong --iterations 250 --timeout-ms 5000
  )
  set_tests_properties(
    exp01.d3d11_two_device_ping_pong
    PROPERTIES
      TIMEOUT 30
  )
endif()

