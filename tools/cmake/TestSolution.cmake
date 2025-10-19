function(add_shad_executable NAME)
  file(GLOB_RECURSE local_sources "src/*.cpp" "src/*.c")

  add_executable(${NAME} ${ARGN} ${local_sources})
  set_target_properties(${NAME} PROPERTIES COMPILE_FLAGS "-Wall -Werror -Wextra -Wpedantic")
  target_link_libraries(${NAME} PRIVATE elf)
endfunction()

function(add_shad_library NAME)
  add_library(${NAME} ${ARGN})
  set_target_properties(${NAME} PROPERTIES COMPILE_FLAGS "-Wall -Werror -Wextra -Wpedantic")
endfunction()

function(add_shad_shared_library NAME)
  add_library(${NAME} SHARED ${ARGN})
  set_target_properties(${NAME} PROPERTIES COMPILE_FLAGS "-Wall -Werror -Wextra -Wpedantic")
endfunction()

function(add_shad_tests TARGET)
  add_shad_executable(${TARGET} ${ARGN})
  target_link_libraries(${TARGET} PRIVATE GTest::gtest_main)
endfunction()
