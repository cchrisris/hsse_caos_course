#include <cstdlib>
#include <iostream>
#include <string>

#include <gtest/gtest.h>

#ifndef LIB_PATH
#define LIB_PATH "./libmalloc.so"
#endif

#ifndef TEST_PATH
#define TEST_PATH "test_malloc"
#endif

TEST(RunMalloc, TheOneAndOnly) {
    std::string cmd = std::string("LD_PRELOAD=") + LIB_PATH + " /" + TEST_PATH;
    std::cout << cmd << std::endl;
    ASSERT_EQ(system(cmd.c_str()), 0);
}
