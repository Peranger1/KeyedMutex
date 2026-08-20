add_executable(
  exp02_d3d11_key_state_machine
  main.cpp
)

target_link_libraries(
  exp02_d3d11_key_state_machine
  PRIVATE
    keyed_mutex_d3d11_test_support
)

if(BUILD_TESTING)
  add_test(
    NAME exp02.d3d11_key_state_machine
    COMMAND exp02_d3d11_key_state_machine --timeout-ms 25
  )
  set_tests_properties(
    exp02.d3d11_key_state_machine
    PROPERTIES
      TIMEOUT 10
  )
endif()

