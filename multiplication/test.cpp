#include "multiplication.hpp"

#include <gtest/gtest.h>

TEST(Static, Signature) {
    static_assert(std::is_same_v<decltype(&Multiply), int64_t (*)(int, int)>,
                  "Do not change Multiply signature");
}

TEST(Runtime, Simple) {
    ASSERT_EQ(Multiply(2, 3), 6);
}

TEST(Runtime, Advanced) {
    ASSERT_EQ(Multiply(999'999'993, -100'000'019), -100'000'018'299'999'867);
}
