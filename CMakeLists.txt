cmake_minimum_required(VERSION 3.14)

project(mipt-caos)

set(CMAKE_C_STANDARD               23)
set(CMAKE_CXX_STANDARD             23)
set(CMAKE_MODULE_PATH              "${CMAKE_SOURCE_DIR}/tools/cmake")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_EXPORT_COMPILE_COMMANDS  ON)

include(tools/cmake/BuildFlags.cmake)

find_package(GTest REQUIRED)

include(tools/cmake/TestSolution.cmake)

function(add_if_exists name)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${name})
    add_subdirectory(${name})
  endif()
endfunction()

add_if_exists(private)

add_if_exists(multiplication)
add_if_exists(strip)
add_if_exists(patchelf)
add_if_exists(du)
add_if_exists(top)
