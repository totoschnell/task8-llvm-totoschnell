#pragma clang diagnostic push
#pragma ide diagnostic ignored "bugprone-reserved-identifier"
/// The code in this file is just a skeleton. You are allowed (and encouraged!)
/// to change if it doesn't fit your needs or ideas.

#include <cstdlib>
// #define WZF_DEBUG
#include "MemorySafetyRuntime.h"
#include <sys/mman.h>
#include <iostream>
#include <cstring>

typedef union ptr_UL_t
{
    void *ptr;
    unsigned long ptr_value;
} ptr_UL;

extern "C"
{
#define get_mapped_ptr(ptr) ((ptr >> 3) + shadow_memory_offset)
#define round_up_to_8(size) ((size + 7) & (~7))

    __attribute__((used)) void __runtime_init()
    {
        /*
        Massively simplified compared to ASan. Use mmap to initialize void *shadow_memory.
        This function should be called in main function.
        */
        void *start = ::mmap(shadow_memory_start,
                             shadow_memory_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_NORESERVE | MAP_ANON | MAP_FIXED_NOREPLACE,
                             -1, 0);
        if (start == reinterpret_cast<void *>(-1))
        {
            std::exit(EXIT_FAILURE);
        }
#ifdef WZF_DEBUG
        else
        {
            indicator = 1;
        }
#endif
    }

    __attribute__((used)) void __runtime_cleanup()
    {
        munmap(shadow_memory_start, shadow_memory_size);
#ifdef WZF_DEBUG
        indicator = 0;
#endif
    }

    __attribute__((used)) void __runtime_check_addr(void *ptr, size_t size)
    { // only check aligned basic data type, because task says only simple load and store are involved
        //get shadow ptr
        ptr_UL ptr_union = {.ptr = ptr};
        ptr_union.ptr_value = get_mapped_ptr(ptr_union.ptr_value);
        int8_t *shadow_ptr = reinterpret_cast<int8_t *>(ptr_union.ptr);
        //check access according to size
        switch (size)
        {
        case 8:
            if (*shadow_ptr != 0)
            {
                std::cerr << "Illegal memory access at " << ptr << std::endl;
                exit(EXIT_FAILURE);
            }
            break;
        default:
            if (*shadow_ptr != 0 && static_cast<int8_t>((reinterpret_cast<unsigned long>(ptr) & 7) + size) > *shadow_ptr)//remember 
            {
                std::cerr << "Illegal memory access at " << ptr << std::endl;
                exit(EXIT_FAILURE);
            }
            break;
        }
    }

    __attribute__((used)) void *__runtime_malloc(size_t size)
    {
        size_t malloc_size = round_up_to_8(size);
        malloc_size += 32;
        int8_t *ret = reinterpret_cast<int8_t *>(malloc(malloc_size));
        if (ret)
        {
            ret = ret + 16;
            __runtime_mark_accessable(ret, size);
        }
        return ret;
    }

    __attribute__((used)) void __runtime_free(void *ptr)
    {
        // mark shadow memory as unaccessable
        __runtime_mark_unaccessable(ptr);
        // free the mallocated space
        int8_t *to_free_ptr = reinterpret_cast<int8_t *>(ptr);
        to_free_ptr = to_free_ptr - 16;
        free(to_free_ptr);
    }

    __attribute__((used)) void __runtime_mark_accessable(void *ptr, size_t size)
    {
        // get shadow ptr
        ptr_UL ptr_union = {.ptr = ptr};
        ptr_union.ptr_value = get_mapped_ptr(ptr_union.ptr_value);
        int8_t *shadow_ptr = reinterpret_cast<int8_t *>(ptr_union.ptr);
        // mark front red zone as unaccessable
        shadow_ptr[-2] = -1;
        shadow_ptr[-1] = -1;
        // mark white zone as accessable
        size_t number_eightbytes = size >> 3;
        std::memset(shadow_ptr, 0, number_eightbytes);
        // mark back red zone as unaccessable
        switch (size & 7)
        {
        case 0:
            shadow_ptr[number_eightbytes] = -1;
            shadow_ptr[number_eightbytes + 1] = -1;
            break;
        default:
            shadow_ptr[number_eightbytes] = size & 7;
            shadow_ptr[number_eightbytes + 1] = -1;
            shadow_ptr[number_eightbytes + 2] = -1;
            break;
        }
    }

    __attribute__((used)) void __runtime_mark_unaccessable(void *ptr)
    { // pass in ptr to white zone
        // get shadow ptr
        ptr_UL ptr_union = {.ptr = ptr};
        ptr_union.ptr_value = get_mapped_ptr(ptr_union.ptr_value);
        int8_t *shadow_ptr = reinterpret_cast<int8_t *>(ptr_union.ptr);
        // mark white zone as red zone
        while (*shadow_ptr != -1)
        {
            *shadow_ptr = -1;
            shadow_ptr++;
        }
    }
}
#pragma clang diagnostic pop
