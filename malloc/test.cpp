#include <vector>

#include <gtest/gtest.h>

size_t GetChunkSize(void* chunk) {
    char* ptr = static_cast<char*>(chunk);
    size_t chunk_size = *reinterpret_cast<size_t*>(ptr - sizeof(size_t));
    size_t unused_bits = 0x7;
    return chunk_size & ~unused_bits;
}

TEST(MallocTests, MallocSimple) {
    int* ptr = static_cast<int*>(malloc(sizeof(int)));
    *ptr = 5;

    ASSERT_EQ(*ptr, 5);
    free(ptr);
}

TEST(MallocTests, Malloc) {
    EXPECT_EXIT(
        [] {
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
            exit(0);
        }(),
        ::testing::ExitedWithCode(0), "");
}

TEST(MallocTests, Realloc) {
    EXPECT_EXIT(
        [] {
            void* ptr = malloc(1024);  // chunk_size should be 1040
            void* ptr_copy = ptr;
            int size = GetChunkSize(ptr);
            ptr = realloc(ptr, 1030);
            ASSERT_EQ(GetChunkSize(ptr), size);
            ptr = realloc(ptr, 50);
            ASSERT_EQ(ptr, ptr_copy);
            ptr = realloc(ptr, 2000);
            ASSERT_TRUE(GetChunkSize(ptr) > 2000);
            free(ptr);
            exit(0);
        }(),
        ::testing::ExitedWithCode(0), "");
}

TEST(MallocTests, Calloc) {
    for (int i = 0; i < 100; ++i) {
        char* str = static_cast<char*>(calloc(256, sizeof(char)));
        for (int i = 0; i < 256; ++i) {
            ASSERT_EQ(str[i], 0);
            str[i] = i;
        }
        free(str);
    }
}

TEST(MallocTests, Errors) {
    EXPECT_DEATH(
        [] {
            void* ptr = malloc(1040);
            free((char*)ptr + 4);
        }(),
        ".*");  // invalid pointer
    EXPECT_DEATH(
        [] {
            void* ptr = malloc(1040);
            free(ptr);
            free(ptr);
        }(),
        ".*");  // double free
}

TEST(MallocTests, BRK) {
    EXPECT_EXIT(
        [] {
            void* current_brk = sbrk(0);
            void* ptrs[1000];
            size_t brk_calls = 0;
            for (int i = 1; i < 1000; ++i) {
                ptrs[i] = malloc(1040);
                if (current_brk != sbrk(0)) {
                    current_brk = sbrk(0);
                    ++brk_calls;
                }
            }

            ASSERT_LE(brk_calls, 100);

            for (int i = 1; i < 1000; ++i) {
                free(ptrs[i]);
            }
            exit(0);
        }(),
        ::testing::ExitedWithCode(0), "");
}

TEST(MallocTests, MMAP) {
    char* ptr = static_cast<char*>(malloc(1040));
    size_t chunk_size = *reinterpret_cast<size_t*>(ptr - sizeof(size_t));
    ASSERT_EQ(chunk_size & 0x2, 0);

    char* mmaped_ptr = static_cast<char*>(malloc(2 * 1024 * 1024));
    chunk_size = *reinterpret_cast<size_t*>(mmaped_ptr - sizeof(size_t));
    ASSERT_EQ(chunk_size & 0x2, 0x2);

    free(ptr);
    free(mmaped_ptr);
}

void RunMallocExtra() {
    // 1. We check that for small allocations, only brk() syscall is used.
    void* current_brk = sbrk(0);
    void* ptrs[1000];
    for (int i = 1; i < 1000; ++i) {
        ptrs[i] = malloc(i);
    }

    // 2. Test that we did not asked too much from OS, i.e. brk has been moved not
    // too far
    //   We have allocated totally 1000*1001/2 = 500'500 bytes,
    //   and we assume that brk has moved not too further than this number
    ASSERT_TRUE(((char*)sbrk(0) - (char*)current_brk < 550'000));

    // 3. Test that memory is indeed allocated, i.e. we can write into it
    for (int i = 1; i < 1000; ++i) {
        for (int j = 0; j < i; ++j) {
            *(static_cast<char*>(ptrs[i]) + j) = 'a';
        }
    }

    // 4. Test that malloc always returns aligned memory
    for (int i = 1; i < 1000; ++i) {
        ASSERT_TRUE(((uint64_t)ptrs[i] % alignof(std::max_align_t) == 0));
    }

    // 5. Check that memory is reused, i.e. repeated allocations of the same sizes
    //  will not call brk() again
    current_brk = sbrk(0);
    for (int i = 1; i < 1000; ++i) {
        free(ptrs[i]);
    }
    for (int i = 999; i >= 1; --i) {
        ptrs[i] = malloc(i);
    }
    ASSERT_EQ(current_brk, sbrk(0));

    // 6. Check that if I free() and then immediately malloc() the same size,
    //  I get the same address
    for (int i = 1; i < 500; ++i) {
        void* oldptr = ptrs[i];
        free(ptrs[i]);
        ptrs[i] = malloc(i);
        ASSERT_EQ(oldptr, ptrs[i]);
    }

    // 7. Check sizes of bins.
    // Bin size should be stored just before the pointer returned by malloc.
    //
    for (int i = 1; i < 500; ++i) {
        char* bin_size_ptr = static_cast<char*>(ptrs[i]) - sizeof(size_t);
        size_t bin_size = static_cast<size_t>(*bin_size_ptr);

        // Bin sizes should be multiples of 16,
        // but last bit in the number indicates if the bin is occupied.
        ASSERT_TRUE(bin_size & 0x1);
        bin_size &= ~0x1;
        ASSERT_TRUE(bin_size % 16 == 0);

        // Bin sizes should not be too big.
        ASSERT_TRUE(bin_size > (size_t)i);
        ASSERT_TRUE(i < 32 || bin_size - i <= 32);
    }

    // 8. Check that memory from smaller bins can be reused for bigger sizes.
    current_brk = sbrk(0);

    for (int i = 1; i < 500; ++i) {
        free(ptrs[i]);
    }

    // We freed 500*501/2 = 125'250 bytes,
    // it should be enough for allocating 150*700 bytes more without calling brk()
    void* newptrs[150];
    for (int i = 0; i < 150; ++i) {
        newptrs[i] = malloc(700);
    }

    ASSERT_EQ(current_brk, sbrk(0));

    for (int i = 0; i < 150; ++i) {
        free(newptrs[i]);
    }

    // 9. And now check that this memory again can be reused for small bins
    for (int i = 1; i < 500; ++i) {
        ptrs[i] = malloc(i);
    }
    ASSERT_EQ(current_brk, sbrk(0));

    // 10. Test realloc. First, check that if we extend memory not exceeding bins
    // size,
    //   then realloc will not move it to anywhere
    for (int i = 1; i < 1000; ++i) {
        void* oldptr = ptrs[i];
        size_t increase = i % 8 != 0 ? 8 - i % 8 : 0;
        size_t newsize = i + increase;
        ptrs[i] = realloc(ptrs[i], newsize);
        ASSERT_EQ(oldptr, ptrs[i]);
    }
    for (int i = 1; i < 1000; ++i) {
        free(ptrs[i]);
    }

    // 11. Check that realloc is indeed able to reallocate
    {
        void* ptr = malloc(50);
        size_t bin_size = static_cast<size_t>(*(static_cast<char*>(ptr) - sizeof(size_t)));
        ASSERT_TRUE(bin_size % 16 == 1);  // Last bit indicates that bin is occupied
        ASSERT_TRUE(bin_size < 80);       // Likely to be 65
        for (int i = 0; i < 50; ++i) {
            *(static_cast<char*>(ptr) + i) = 'a';
        }

        void* newptr = realloc(ptr, 120);
        size_t new_bin_size = static_cast<size_t>(*(static_cast<char*>(newptr) - sizeof(size_t)));
        ASSERT_TRUE(new_bin_size > 80);
        ASSERT_TRUE(new_bin_size < 150);  // Likely to be 129
        ASSERT_TRUE(new_bin_size % 16 == 1);

        for (int i = 0; i < 50; ++i) {
            char c = *(static_cast<char*>(newptr) + i);
            ASSERT_TRUE(c == 'a');
        }

        // For some reason, for std::realloc this doesn't hold, so we skip this
        // check
        /*if (ptr != newptr) {
            size_t bin_size = (size_t&) *((char*) ptr - sizeof(size_t));
            assert(bin_size % 16 == 0);
        }*/

        free(newptr);
    }
}

TEST(MallocTests, MallocExtra) {
    EXPECT_EXIT(
        [] {
            RunMallocExtra();
            exit(0);
        }(),
        ::testing::ExitedWithCode(0), "");
}

// glhf
TEST(MallocTests, ChunkSizes) {
    EXPECT_EXIT(
        [] {
            for (size_t size = sizeof(size_t); size < 1024UL; size += sizeof(size_t)) {
                void* ptr = malloc(size);
                size_t chunk_size = GetChunkSize(ptr);
                size_t expected_size = std::max(32UL, ((size + 8) + 15) / 16 * 16);
                ASSERT_EQ(chunk_size, expected_size);
                free(ptr);
            }
            exit(0);
        }(),
        ::testing::ExitedWithCode(0), "");
}

TEST(MallocTests, Vector) {
    std::vector<int> vec{1, 2, 3, 4, 5};
    for (size_t i = 0; i < vec.size(); ++i) {
        ASSERT_EQ(vec[i], i + 1);
    }
    vec.resize(1024);
    ASSERT_EQ(vec.capacity(), 1024);
}

TEST(MallocTests, New) {
    int* array = new int[10];
    int* value = new int(5);

    array[5] = 10;
    ASSERT_EQ(array[5], 10);
    ASSERT_EQ(*value, 5);

    delete value;
    delete[] array;
}