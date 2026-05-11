#include "dma.h"

#define EFI_CONVENTIONAL_MEMORY 7u
#define DMA_POOL_LIMIT 0x100000000ull
#define DMA_POOL_MIN 0x01000000ull
#define DMA_POOL_SIZE (512ull * 1024ull)

typedef struct {
    uint32_t type;
    uint32_t pad;
    uint64_t physical_start;
    uint64_t virtual_start;
    uint64_t number_of_pages;
    uint64_t attribute;
} efi_memory_descriptor_t;

static uint64_t dma_pool_base;
static uint64_t dma_pool_size;
static uint64_t dma_pool_offset;

static uint64_t align_up64(uint64_t value, uint64_t alignment) {
    if (alignment == 0) {
        return value;
    }

    return (value + alignment - 1ull) & ~(alignment - 1ull);
}

static int overlaps(uint64_t start, uint64_t end, uint64_t other_start, uint64_t other_end) {
    return start < other_end && other_start < end;
}

void dma_init(const boot_info_t *info) {
    dma_pool_base = 0;
    dma_pool_size = 0;
    dma_pool_offset = 0;

    if (!info ||
        info->memory_map == 0 ||
        info->memory_map_size == 0 ||
        info->memory_map_descriptor_size < sizeof(efi_memory_descriptor_t)) {
        return;
    }

    const uint8_t *map = (const uint8_t *)(uintptr_t)info->memory_map;
    uint64_t kernel_start = info->kernel_base;
    uint64_t kernel_end = info->kernel_base + info->kernel_size;
    uint64_t map_start = info->memory_map;
    uint64_t map_end = info->memory_map + info->memory_map_size;

    for (uint64_t off = 0;
         off + sizeof(efi_memory_descriptor_t) <= info->memory_map_size;
         off += info->memory_map_descriptor_size) {
        const efi_memory_descriptor_t *desc =
            (const efi_memory_descriptor_t *)(const void *)(map + off);

        if (desc->type != EFI_CONVENTIONAL_MEMORY) {
            continue;
        }

        uint64_t start = desc->physical_start;
        uint64_t end = start + desc->number_of_pages * 4096ull;
        if (end > DMA_POOL_LIMIT) {
            end = DMA_POOL_LIMIT;
        }
        if (start < DMA_POOL_MIN) {
            start = DMA_POOL_MIN;
        }

        start = align_up64(start, 4096ull);
        if (end <= start || end - start < DMA_POOL_SIZE) {
            continue;
        }
        if (overlaps(start, start + DMA_POOL_SIZE, kernel_start, kernel_end) ||
            overlaps(start, start + DMA_POOL_SIZE, map_start, map_end)) {
            continue;
        }

        dma_pool_base = start;
        dma_pool_size = DMA_POOL_SIZE;
        dma_pool_offset = 0;
        return;
    }
}

void *dma_alloc(uint32_t size, uint32_t alignment) {
    if (dma_pool_base == 0 || size == 0) {
        return 0;
    }

    uint64_t offset = align_up64(dma_pool_offset, alignment ? alignment : 16u);
    if (offset + size > dma_pool_size) {
        return 0;
    }

    uint64_t addr = dma_pool_base + offset;
    dma_pool_offset = offset + size;

    uint8_t *p = (uint8_t *)(uintptr_t)addr;
    for (uint32_t i = 0; i < size; ++i) {
        p[i] = 0;
    }

    return (void *)(uintptr_t)addr;
}

uint64_t dma_phys(const void *ptr) {
    return (uint64_t)(uintptr_t)ptr;
}
