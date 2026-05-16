#include "kmem.h"
#include "dma.h"

#define EFI_CONVENTIONAL_MEMORY 7u
#define KMEM_MIN_ADDRESS 0x00100000ull
#define KMEM_MAX_RANGES 128u
#define KMEM_SMALL_CLASS_COUNT 8u
#define KMEM_ALLOC_MAGIC 0x4B4D454D48454150ull
#define KMEM_FREED_MAGIC 0x4B4D454D46524545ull
#define KMEM_GUARD_MAGIC 0xC0FFEE42u

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
    uint32_t allocated_bytes;
    uint16_t class_index;
    uint16_t page_count;
    uint32_t reserved;
} kmalloc_header_t;

static page_range_t free_ranges[KMEM_MAX_RANGES];
static uint32_t free_range_count;
static uint64_t total_page_count;
static uint64_t free_page_count;
static uint64_t heap_used_bytes;
static uint64_t allocation_count;
static uint64_t free_count;
static uint64_t live_allocations;
static uint64_t peak_live_allocations;
static uint64_t invalid_frees;
static uint64_t double_frees;
static uint64_t guard_failures;
static uint64_t allocation_failures;
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

static void fill_memory(void *ptr, uint64_t size, uint8_t value) {
    uint8_t *p = (uint8_t *)ptr;

    for (uint64_t i = 0; i < size; ++i) {
        p[i] = value;
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
    allocation_count = 0;
    free_count = 0;
    live_allocations = 0;
    peak_live_allocations = 0;
    invalid_frees = 0;
    double_frees = 0;
    guard_failures = 0;
    allocation_failures = 0;
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

    if (addr == 0) {
        lock();
        ++allocation_failures;
        unlock();
    }

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
    uint32_t overhead = (uint32_t)sizeof(kmalloc_header_t) + (uint32_t)sizeof(uint32_t);
    uint32_t need;

    if (bytes > 0xffffffffu - overhead) {
        return -1;
    }
    need = bytes + overhead;

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

static uint32_t *kmalloc_guard_for_header(kmalloc_header_t *header) {
    return (uint32_t *)(void *)((uint8_t *)(void *)(header + 1) + header->bytes);
}

static int small_free_list_contains(void *block) {
    for (uint32_t i = 0; i < KMEM_SMALL_CLASS_COUNT; ++i) {
        for (free_block_t *it = small_free[i]; it != 0; it = it->next) {
            if ((void *)it == block) {
                return 1;
            }
        }
    }
    return 0;
}

static void kmalloc_note_alloc(uint64_t charged_bytes) {
    heap_used_bytes += charged_bytes;
    ++allocation_count;
    ++live_allocations;
    if (live_allocations > peak_live_allocations) {
        peak_live_allocations = live_allocations;
    }
}

static void kmalloc_note_free(uint64_t charged_bytes) {
    if (heap_used_bytes >= charged_bytes) {
        heap_used_bytes -= charged_bytes;
    } else {
        heap_used_bytes = 0;
    }
    ++free_count;
    if (live_allocations > 0) {
        --live_allocations;
    }
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
        }
        unlock();

        if (block == 0) {
            lock();
            ++allocation_failures;
            unlock();
            return 0;
        }

        header = (kmalloc_header_t *)(void *)block;
        header->magic = KMEM_ALLOC_MAGIC;
        header->bytes = size;
        header->allocated_bytes = small_class_sizes[class_index];
        header->class_index = (uint16_t)class_index;
        header->page_count = 0;
        header->reserved = 0;
        *kmalloc_guard_for_header(header) = KMEM_GUARD_MAGIC;
        fill_memory((uint8_t *)(void *)(header + 1), size, 0xA5u);
        lock();
        kmalloc_note_alloc(small_class_sizes[class_index]);
        unlock();
        return (void *)(header + 1);
    }

    {
        uint32_t overhead = (uint32_t)sizeof(kmalloc_header_t) + (uint32_t)sizeof(uint32_t);
        uint32_t pages;

        if (size > 0xffffffffu - overhead - (KMEM_PAGE_SIZE - 1u)) {
            lock();
            ++allocation_failures;
            unlock();
            return 0;
        }
        pages = (size + overhead + KMEM_PAGE_SIZE - 1u) / KMEM_PAGE_SIZE;
        if (pages > 0xffffu) {
            lock();
            ++allocation_failures;
            unlock();
            return 0;
        }
        header = (kmalloc_header_t *)page_alloc(pages);
        if (header == 0) {
            return 0;
        }
        header->magic = KMEM_ALLOC_MAGIC;
        header->bytes = size;
        header->allocated_bytes = pages * KMEM_PAGE_SIZE;
        header->class_index = 0xffffu;
        header->page_count = (uint16_t)pages;
        header->reserved = 0;
        *kmalloc_guard_for_header(header) = KMEM_GUARD_MAGIC;
        lock();
        kmalloc_note_alloc((uint64_t)pages * KMEM_PAGE_SIZE);
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
        lock();
        if (header->magic == KMEM_FREED_MAGIC || small_free_list_contains(header)) {
            ++double_frees;
        } else {
            ++invalid_frees;
        }
        unlock();
        return;
    }
    if (*kmalloc_guard_for_header(header) != KMEM_GUARD_MAGIC) {
        lock();
        ++guard_failures;
        unlock();
    }

    if (header->class_index == 0xffffu) {
        uint32_t pages = header->page_count;
        header->magic = KMEM_FREED_MAGIC;
        *kmalloc_guard_for_header(header) = 0;
        fill_memory(ptr, header->bytes, 0xDDu);
        lock();
        kmalloc_note_free((uint64_t)pages * KMEM_PAGE_SIZE);
        unlock();
        page_free(header, pages);
        return;
    }

    if (header->class_index < KMEM_SMALL_CLASS_COUNT) {
        uint16_t class_index = header->class_index;
        free_block_t *block = (free_block_t *)(void *)header;
        uint32_t block_size = small_class_sizes[class_index];
        *kmalloc_guard_for_header(header) = 0;
        fill_memory(ptr, header->bytes, 0xDDu);
        header->magic = KMEM_FREED_MAGIC;
        lock();
        block->next = small_free[class_index];
        small_free[class_index] = block;
        kmalloc_note_free(block_size);
        unlock();
        return;
    }

    lock();
    ++invalid_frees;
    unlock();
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
    uint64_t smallest = 0;
    uint64_t small_blocks = 0;

    if (stats == 0) {
        return;
    }

    lock();
    for (uint32_t i = 0; i < free_range_count; ++i) {
        if (free_ranges[i].pages > largest) {
            largest = free_ranges[i].pages;
        }
        if (smallest == 0 || free_ranges[i].pages < smallest) {
            smallest = free_ranges[i].pages;
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
    stats->smallest_free_range_pages = smallest;
    stats->allocation_failures = allocation_failures;
    if (free_page_count == 0 || largest >= free_page_count) {
        stats->fragmentation_percent = 0;
    } else {
        stats->fragmentation_percent =
            (uint32_t)(((free_page_count - largest) * 100ull) / free_page_count);
    }
    stats->allocation_count = allocation_count;
    stats->free_count = free_count;
    stats->live_allocations = live_allocations;
    stats->peak_live_allocations = peak_live_allocations;
    stats->invalid_frees = invalid_frees;
    stats->double_frees = double_frees;
    stats->guard_failures = guard_failures;
    unlock();
}

static uint64_t kmem_fault_total(const kmem_stats_t *stats) {
    return stats->invalid_frees + stats->double_frees + stats->guard_failures;
}

void kmem_run_selftest(kmem_test_result_t *result) {
    enum { BLOCK_COUNT = 64, GAP_COUNT = 16 };
    static const uint32_t sizes[BLOCK_COUNT] = {
        1u, 7u, 16u, 24u, 33u, 48u, 63u, 80u,
        96u, 127u, 160u, 191u, 224u, 255u, 300u, 384u,
        511u, 600u, 700u, 900u, 1023u, 1200u, 1500u, 1800u,
        2040u, 2500u, 3000u, 3500u, 4090u, 5000u, 7000u, 9000u,
        12000u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u,
        3072u, 4096u, 8192u, 12288u, 5u, 17u, 65u, 129u,
        257u, 513u, 1025u, 1536u, 2049u, 4097u, 6144u, 10000u,
        31u, 62u, 124u, 248u, 496u, 992u, 1984u, 3968u
    };
    void *blocks[BLOCK_COUNT];
    void *gaps[GAP_COUNT];
    kmem_stats_t before;
    kmem_stats_t after;
    uint8_t *zero_block;
    void *too_large;
    void *too_many_pages;
    uint32_t bad = 0;

    if (result == 0) {
        return;
    }

    zero_memory(result, sizeof(*result));
    for (uint32_t i = 0; i < BLOCK_COUNT; ++i) {
        blocks[i] = 0;
    }
    for (uint32_t i = 0; i < GAP_COUNT; ++i) {
        gaps[i] = 0;
    }

    kmem_get_stats(&before);
    result->live_allocations_before = before.live_allocations;
    result->heap_used_before = before.heap_used_bytes;
    result->free_pages_before = before.free_pages;
    result->largest_free_range_before = before.largest_free_range_pages;
    result->small_free_blocks_before = before.small_free_blocks;
    result->fault_count_before = kmem_fault_total(&before);

    too_large = kmalloc(0xfffffff0u);
    ++result->expected_failures;
    if (too_large != 0) {
        ++result->unexpected_successes;
        kfree(too_large);
    }

    too_many_pages = page_alloc(before.free_pages < 0xffffffffull ?
                               (uint32_t)before.free_pages + 1u :
                               0xffffffffu);
    ++result->expected_failures;
    if (too_many_pages != 0) {
        ++result->unexpected_successes;
        page_free(too_many_pages, before.free_pages < 0xffffffffull ?
                  (uint32_t)before.free_pages + 1u :
                  0xffffffffu);
    }

    zero_block = (uint8_t *)kzalloc(257u);
    ++result->alloc_attempts;
    if (zero_block == 0) {
        bad = 1;
    } else {
        ++result->alloc_successes;
        for (uint32_t i = 0; i < 257u; ++i) {
            if (zero_block[i] != 0) {
                bad = 1;
                break;
            }
        }
        kfree(zero_block);
    }

    for (uint32_t i = 0; i < BLOCK_COUNT; ++i) {
        blocks[i] = kmalloc(sizes[i]);
        ++result->alloc_attempts;
        if (blocks[i] != 0) {
            ++result->alloc_successes;
        } else {
            bad = 1;
        }
    }

    for (uint32_t i = 0; i < BLOCK_COUNT; i += 2u) {
        kfree(blocks[i]);
        blocks[i] = 0;
    }

    for (uint32_t i = 0; i < GAP_COUNT; ++i) {
        gaps[i] = kmalloc(96u + i * 37u);
        ++result->alloc_attempts;
        if (gaps[i] != 0) {
            ++result->alloc_successes;
        } else {
            bad = 1;
        }
    }

    for (uint32_t i = 0; i < GAP_COUNT; ++i) {
        kfree(gaps[i]);
    }
    for (uint32_t i = 0; i < BLOCK_COUNT; ++i) {
        kfree(blocks[i]);
    }

    kmem_get_stats(&after);
    result->live_allocations_after = after.live_allocations;
    result->heap_used_after = after.heap_used_bytes;
    result->free_pages_after = after.free_pages;
    result->largest_free_range_after = after.largest_free_range_pages;
    result->small_free_blocks_after = after.small_free_blocks;
    result->fault_count_after = kmem_fault_total(&after);
    result->passed = !bad &&
                     result->unexpected_successes == 0u &&
                     result->live_allocations_after == result->live_allocations_before &&
                     result->heap_used_after == result->heap_used_before &&
                     result->fault_count_after == result->fault_count_before;
}
