#include "kmem.h"
#include "dma.h"

#define EFI_CONVENTIONAL_MEMORY 7u
#define KMEM_MIN_ADDRESS 0x00100000ull
#define KMEM_MAX_RANGES 128u
#define KMEM_SMALL_CLASS_COUNT 8u
#define KMEM_ALLOC_MAGIC 0x4B4D454D48454150ull

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} efi_memory_descriptor_t;

typedef struct {
    uint64_t start;
    uint64_t pages;
} page_range_t;

typedef struct free_block {
    struct free_block *next;
} free_block_t;

typedef struct {
    uint64_t magic;
    uint32_t bytes;
    uint16_t class_index;
    uint16_t page_count;
} kmalloc_header_t;

static page_range_t free_ranges[KMEM_MAX_RANGES];
static uint32_t free_range_count;
static uint64_t total_page_count;
static uint64_t free_page_count;
static uint64_t heap_used_bytes;
static free_block_t *small_free[KMEM_SMALL_CLASS_COUNT];
static volatile int kmem_lock;

static const uint32_t small_class_sizes[KMEM_SMALL_CLASS_COUNT] = {
    32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u
};

static void lock(void) {
    while (__sync_lock_test_and_set(&kmem_lock, 1)) {
        __asm__ __volatile__("pause");
    }
}

static void unlock(void) {
    __sync_lock_release(&kmem_lock);
}

static uint64_t align_up64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

static uint64_t align_down64(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1ull);
}

static int overlaps(uint64_t start, uint64_t end, uint64_t other_start, uint64_t other_end) {
    return start < other_end && other_start < end;
}

static void zero_memory(void *ptr, uint64_t size) {
    uint8_t *p = (uint8_t *)ptr;

    for (uint64_t i = 0; i < size; ++i) {
        p[i] = 0;
    }
}

static void remove_range_index(uint32_t index) {
    if (index >= free_range_count) {
        return;
    }
    for (uint32_t i = index + 1u; i < free_range_count; ++i) {
        free_ranges[i - 1u] = free_ranges[i];
    }
    --free_range_count;
}

static void coalesce_ranges(void) {
    for (uint32_t i = 0; i < free_range_count; ++i) {
        for (uint32_t j = i + 1u; j < free_range_count; ++j) {
            if (free_ranges[j].start < free_ranges[i].start) {
                page_range_t tmp = free_ranges[i];
                free_ranges[i] = free_ranges[j];
                free_ranges[j] = tmp;
            }
        }
    }

    for (uint32_t i = 0; i + 1u < free_range_count;) {
        uint64_t end = free_ranges[i].start + free_ranges[i].pages * KMEM_PAGE_SIZE;
        if (end >= free_ranges[i + 1u].start) {
            uint64_t next_end = free_ranges[i + 1u].start +
                                free_ranges[i + 1u].pages * KMEM_PAGE_SIZE;
            if (next_end > end) {
                free_ranges[i].pages = (next_end - free_ranges[i].start) / KMEM_PAGE_SIZE;
            }
            remove_range_index(i + 1u);
        } else {
            ++i;
        }
    }
}

static void add_free_range(uint64_t start, uint64_t pages) {
    if (pages == 0 || free_range_count >= KMEM_MAX_RANGES) {
        return;
    }

    free_ranges[free_range_count].start = start;
    free_ranges[free_range_count].pages = pages;
    ++free_range_count;
    total_page_count += pages;
    free_page_count += pages;
}

static void reserve_range(uint64_t start, uint64_t end) {
    start = align_down64(start, KMEM_PAGE_SIZE);
    end = align_up64(end, KMEM_PAGE_SIZE);

    for (uint32_t i = 0; i < free_range_count;) {
        uint64_t range_start = free_ranges[i].start;
        uint64_t range_end = range_start + free_ranges[i].pages * KMEM_PAGE_SIZE;

        if (!overlaps(range_start, range_end, start, end)) {
            ++i;
            continue;
        }

        if (start <= range_start && end >= range_end) {
            free_page_count -= free_ranges[i].pages;
            total_page_count -= free_ranges[i].pages;
            remove_range_index(i);
            continue;
        }

        if (start <= range_start) {
            uint64_t removed = (end - range_start) / KMEM_PAGE_SIZE;
            free_ranges[i].start += removed * KMEM_PAGE_SIZE;
            free_ranges[i].pages -= removed;
            free_page_count -= removed;
            total_page_count -= removed;
            ++i;
            continue;
        }

        if (end >= range_end) {
            uint64_t removed = (range_end - start) / KMEM_PAGE_SIZE;
            free_ranges[i].pages -= removed;
            free_page_count -= removed;
            total_page_count -= removed;
            ++i;
            continue;
        }

        if (free_range_count < KMEM_MAX_RANGES) {
            uint64_t left_pages = (start - range_start) / KMEM_PAGE_SIZE;
            uint64_t right_pages = (range_end - end) / KMEM_PAGE_SIZE;
            free_ranges[i].pages = left_pages;
            free_ranges[free_range_count].start = end;
            free_ranges[free_range_count].pages = right_pages;
            ++free_range_count;
            free_page_count -= (end - start) / KMEM_PAGE_SIZE;
            total_page_count -= (end - start) / KMEM_PAGE_SIZE;
        }
        ++i;
    }
}

void kmem_init(const boot_info_t *info) {
    free_range_count = 0;
    total_page_count = 0;
    free_page_count = 0;
    heap_used_bytes = 0;
    for (uint32_t i = 0; i < KMEM_SMALL_CLASS_COUNT; ++i) {
        small_free[i] = 0;
    }

    if (!info ||
        info->memory_map == 0 ||
        info->memory_map_size == 0 ||
        info->memory_map_descriptor_size < sizeof(efi_memory_descriptor_t)) {
        return;
    }

    const uint8_t *map = (const uint8_t *)(uintptr_t)info->memory_map;
    for (uint64_t off = 0;
         off + sizeof(efi_memory_descriptor_t) <= info->memory_map_size;
         off += info->memory_map_descriptor_size) {
        const efi_memory_descriptor_t *desc =
            (const efi_memory_descriptor_t *)(const void *)(map + off);
        uint64_t start;
        uint64_t end;

        if (desc->type != EFI_CONVENTIONAL_MEMORY) {
            continue;
        }

        start = align_up64(desc->physical_start, KMEM_PAGE_SIZE);
        if (start < KMEM_MIN_ADDRESS) {
            start = KMEM_MIN_ADDRESS;
        }
        end = align_down64(desc->physical_start + desc->number_of_pages * KMEM_PAGE_SIZE,
                           KMEM_PAGE_SIZE);
        if (end > start) {
            add_free_range(start, (end - start) / KMEM_PAGE_SIZE);
        }
    }

    reserve_range(info->kernel_base, info->kernel_base + info->kernel_size);
    reserve_range(info->memory_map, info->memory_map + info->memory_map_size);
    if (info->framebuffer_base && info->framebuffer_height && info->framebuffer_pixels_per_scanline) {
        reserve_range(info->framebuffer_base,
                      info->framebuffer_base +
                      (uint64_t)info->framebuffer_height *
                      info->framebuffer_pixels_per_scanline *
                      sizeof(uint32_t));
    }
    if (dma_pool_base() && dma_pool_size()) {
        reserve_range(dma_pool_base(), dma_pool_base() + dma_pool_size());
    }
    coalesce_ranges();
}

void *page_alloc(uint32_t page_count) {
    uint64_t addr = 0;

    if (page_count == 0) {
        return 0;
    }

    lock();
    for (uint32_t i = 0; i < free_range_count; ++i) {
        if (free_ranges[i].pages < page_count) {
            continue;
        }

        addr = free_ranges[i].start;
        free_ranges[i].start += (uint64_t)page_count * KMEM_PAGE_SIZE;
        free_ranges[i].pages -= page_count;
        free_page_count -= page_count;
        if (free_ranges[i].pages == 0) {
            remove_range_index(i);
        }
        break;
    }
    unlock();

    if (addr != 0) {
        zero_memory((void *)(uintptr_t)addr, (uint64_t)page_count * KMEM_PAGE_SIZE);
    }

    return (void *)(uintptr_t)addr;
}

void page_free(void *ptr, uint32_t page_count) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;

    if (addr == 0 || page_count == 0 || (addr & (KMEM_PAGE_SIZE - 1u)) != 0) {
        return;
    }

    lock();
    if (free_range_count < KMEM_MAX_RANGES) {
        free_ranges[free_range_count].start = addr;
        free_ranges[free_range_count].pages = page_count;
        ++free_range_count;
        free_page_count += page_count;
        coalesce_ranges();
    }
    unlock();
}

static int class_for_size(uint32_t bytes) {
    uint32_t need = bytes + (uint32_t)sizeof(kmalloc_header_t);

    for (uint32_t i = 0; i < KMEM_SMALL_CLASS_COUNT; ++i) {
        if (need <= small_class_sizes[i]) {
            return (int)i;
        }
    }
    return -1;
}

static void populate_class(uint32_t class_index) {
    uint32_t block_size = small_class_sizes[class_index];
    uint8_t *page = (uint8_t *)page_alloc(1u);

    if (page == 0) {
        return;
    }

    lock();
    for (uint32_t off = 0; off + block_size <= KMEM_PAGE_SIZE; off += block_size) {
        free_block_t *block = (free_block_t *)(void *)(page + off);
        block->next = small_free[class_index];
        small_free[class_index] = block;
    }
    unlock();
}

void *kmalloc(uint32_t size) {
    kmalloc_header_t *header;
    int class_index;

    if (size == 0) {
        return 0;
    }

    class_index = class_for_size(size);
    if (class_index >= 0) {
        free_block_t *block;

        lock();
        block = small_free[class_index];
        unlock();
        if (block == 0) {
            populate_class((uint32_t)class_index);
        }

        lock();
        block = small_free[class_index];
        if (block != 0) {
            small_free[class_index] = block->next;
            heap_used_bytes += small_class_sizes[class_index];
        }
        unlock();

        if (block == 0) {
            return 0;
        }

        header = (kmalloc_header_t *)(void *)block;
        header->magic = KMEM_ALLOC_MAGIC;
        header->bytes = size;
        header->class_index = (uint16_t)class_index;
        header->page_count = 0;
        return (void *)(header + 1);
    }

    {
        uint32_t pages = (size + (uint32_t)sizeof(kmalloc_header_t) + KMEM_PAGE_SIZE - 1u) / KMEM_PAGE_SIZE;
        if (pages > 0xffffu) {
            return 0;
        }
        header = (kmalloc_header_t *)page_alloc(pages);
        if (header == 0) {
            return 0;
        }
        header->magic = KMEM_ALLOC_MAGIC;
        header->bytes = size;
        header->class_index = 0xffffu;
        header->page_count = (uint16_t)pages;
        lock();
        heap_used_bytes += (uint64_t)pages * KMEM_PAGE_SIZE;
        unlock();
        return (void *)(header + 1);
    }
}

void *kzalloc(uint32_t size) {
    void *ptr = kmalloc(size);

    if (ptr != 0) {
        zero_memory(ptr, size);
    }
    return ptr;
}

void kfree(void *ptr) {
    kmalloc_header_t *header;

    if (ptr == 0) {
        return;
    }

    header = ((kmalloc_header_t *)ptr) - 1;
    if (header->magic != KMEM_ALLOC_MAGIC) {
        return;
    }

    if (header->class_index == 0xffffu) {
        uint32_t pages = header->page_count;
        header->magic = 0;
        lock();
        heap_used_bytes -= (uint64_t)pages * KMEM_PAGE_SIZE;
        unlock();
        page_free(header, pages);
        return;
    }

    if (header->class_index < KMEM_SMALL_CLASS_COUNT) {
        uint16_t class_index = header->class_index;
        free_block_t *block = (free_block_t *)(void *)header;
        header->magic = 0;
        lock();
        block->next = small_free[class_index];
        small_free[class_index] = block;
        heap_used_bytes -= small_class_sizes[class_index];
        unlock();
    }
}

uint64_t kmem_total_pages(void) {
    return total_page_count;
}

uint64_t kmem_free_pages(void) {
    return free_page_count;
}

uint64_t kmem_heap_used_bytes(void) {
    return heap_used_bytes;
}

void kmem_get_stats(kmem_stats_t *stats) {
    uint64_t largest = 0;
    uint64_t small_blocks = 0;

    if (stats == 0) {
        return;
    }

    lock();
    for (uint32_t i = 0; i < free_range_count; ++i) {
        if (free_ranges[i].pages > largest) {
            largest = free_ranges[i].pages;
        }
    }
    for (uint32_t i = 0; i < KMEM_SMALL_CLASS_COUNT; ++i) {
        for (free_block_t *block = small_free[i]; block != 0; block = block->next) {
            ++small_blocks;
        }
    }

    stats->total_pages = total_page_count;
    stats->free_pages = free_page_count;
    stats->heap_used_bytes = heap_used_bytes;
    stats->small_free_blocks = small_blocks;
    stats->free_ranges = free_range_count;
    stats->largest_free_range_pages = largest;
    unlock();
}
