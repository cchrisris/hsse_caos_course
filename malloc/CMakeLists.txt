add_shad_shared_library(malloc_lib main.cpp)
target_compile_options(malloc_lib PRIVATE -fPIC)
set_target_properties(malloc_lib PROPERTIES OUTPUT_NAME "malloc")

add_shad_tests(tester_malloc test.cpp)
add_dependencies(tester_malloc malloc_lib)

add_shad_tests(test_malloc run_test.cpp)
add_dependencies(test_malloc tester_malloc)
target_compile_definitions(test_malloc PRIVATE LIB_PATH=\"$<TARGET_FILE:malloc_lib>\")
target_compile_definitions(test_malloc PRIVATE TEST_PATH=\"$<TARGET_FILE:tester_malloc>\")
