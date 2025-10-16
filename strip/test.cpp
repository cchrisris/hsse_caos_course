#include <stdexcept>

#include <gtest/gtest.h>

TEST(NotYetReady, NotYetReady) {
    throw std::runtime_error("Not implemented");
}
