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

add_subdirectory(experiments/01_d3d11_two_device_ping_pong)

