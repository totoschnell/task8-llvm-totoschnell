#ifndef MEMORYSAFETYRUNTIME_H
#define MEMORYSAFETYRUNTIME_H
#include <cstdlib>

#ifdef __cplusplus
extern "C"{
#endif

unsigned long const shadow_memory_offset = 0x7fff8000UL;//assume the user application uses virtual address space from 0x10007fff8000 to 0x7fffffffffff
void *const shadow_memory_start  = reinterpret_cast<void *>(0x02008fff7000UL);
const size_t shadow_memory_size  = 0xdfff0001000UL;
#ifdef WZF_DEBUG
size_t indicator = 0;
#endif

__attribute__((used))
void __runtime_init();

__attribute__((used))
void __runtime_cleanup();

__attribute__((used))
void __runtime_check_addr(void *ptr, size_t size);

__attribute__((used))
void *__runtime_malloc(size_t size);

__attribute__((used))
void __runtime_free(void *ptr);

__attribute__((used))
void __runtime_mark_accessable(void *ptr, size_t);

__attribute__((used))
void __runtime_mark_unaccessable(void *ptr);
#ifdef __cplusplus
}
#endif

#endif