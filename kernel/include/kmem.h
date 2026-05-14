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
} kmem_stats_t;

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

#endif
