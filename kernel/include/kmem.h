#ifndef KMEM_H
#define KMEM_H

#include <stdint.h>
#include "bootinfo.h"

#define KMEM_PAGE_SIZE 4096u

typedef struct {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t heap_used_bytes;
    uint64_t small_free_blocks;
    uint64_t free_ranges;
    uint64_t largest_free_range_pages;
    uint64_t smallest_free_range_pages;
    uint64_t allocation_failures;
    uint32_t fragmentation_percent;
    uint64_t allocation_count;
    uint64_t free_count;
    uint64_t live_allocations;
    uint64_t peak_live_allocations;
    uint64_t invalid_frees;
    uint64_t double_frees;
    uint64_t guard_failures;
} kmem_stats_t;

typedef struct {
    uint32_t passed;
    uint32_t alloc_attempts;
    uint32_t alloc_successes;
    uint32_t expected_failures;
    uint32_t unexpected_successes;
    uint64_t live_allocations_before;
    uint64_t live_allocations_after;
    uint64_t heap_used_before;
    uint64_t heap_used_after;
    uint64_t free_pages_before;
    uint64_t free_pages_after;
    uint64_t largest_free_range_before;
    uint64_t largest_free_range_after;
    uint64_t small_free_blocks_before;
    uint64_t small_free_blocks_after;
    uint64_t fault_count_before;
    uint64_t fault_count_after;
} kmem_test_result_t;

void kmem_init(const boot_info_t *info);
void *kmalloc(uint32_t size);
void *kzalloc(uint32_t size);
void kfree(void *ptr);
void *page_alloc(uint32_t page_count);
void page_free(void *ptr, uint32_t page_count);
uint64_t kmem_total_pages(void);
uint64_t kmem_free_pages(void);
uint64_t kmem_heap_used_bytes(void);
void kmem_get_stats(kmem_stats_t *stats);
void kmem_run_selftest(kmem_test_result_t *result);

#endif
