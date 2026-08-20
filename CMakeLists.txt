cmake_minimum_required(VERSION 3.25)

project(KeyedMutexExperiments VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(KEYED_MUTEX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

add_library(keyed_mutex_project_options INTERFACE)
target_compile_features(keyed_mutex_project_options INTERFACE cxx_std_20)

if(MSVC)
  target_compile_options(
    keyed_mutex_project_options
    INTERFACE
      /W4
      /permissive-
      /EHsc
      /utf-8
      $<$<BOOL:${KEYED_MUTEX_WARNINGS_AS_ERRORS}>:/WX>
  )
  target_compile_definitions(
    keyed_mutex_project_options
    INTERFACE
      NOMINMAX
      WIN32_LEAN_AND_MEAN
      UNICODE
      _UNICODE
  )
endif()

add_library(
  keyed_mutex_d3d11_test_support
  src/d3d11_test_support.cpp
)
target_include_directories(
  keyed_mutex_d3d11_test_support
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(
  keyed_mutex_d3d11_test_support
  PUBLIC
    keyed_mutex_project_options
    d3d11
    dxgi
)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
include(GoogleTest)

add_subdirectory(experiments/01_d3d11_two_device_ping_pong)
add_subdirectory(experiments/02_d3d11_key_state_machine)
add_subdirectory(experiments/03_d3d11_three_device_ring)
add_subdirectory(experiments/04_d3d11_same_key_contention)
add_subdirectory(experiments/05_d3d11_owner_abandonment)
add_subdirectory(experiments/06_d3d11_cross_process_ping_pong)
add_subdirectory(experiments/07_d3d11_same_interface_threads)
add_subdirectory(experiments/08_d3d11_flush_visibility)
add_subdirectory(experiments/09_d3d11_multithread_context)
add_subdirectory(experiments/10_d3d11_lock_order)
add_subdirectory(experiments/11_d3d11_multiple_open_interfaces)
add_subdirectory(experiments/12_d3d11_failure_injection)
add_subdirectory(experiments/13_d3d11_gpu_visibility)
add_subdirectory(experiments/14_d3d11_handle_com_lifetime)
