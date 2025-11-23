#include <gtest/gtest.h>

TEST(MallocTests, Malloc) {
    size_t size = 1040;
    void* ptr_fresh[4];
    for (int i = 0; i < 4; ++i) {
        ptr_fresh[i] = malloc(size);
        ASSERT_EQ((uintptr_t)ptr_fresh[i] % 2 * sizeof(size_t), 0);
    }
    free(ptr_fresh[1]);
    void* ptr = malloc(size);
    ASSERT_EQ(ptr, ptr_fresh[1]);
    free(ptr);
    free(ptr_fresh[0]);
    free(ptr_fresh[2]);

    ptr = malloc(2 * size);
    ASSERT_EQ(ptr, ptr_fresh[0]);
    free(ptr_fresh[3]);
    free(ptr);
}