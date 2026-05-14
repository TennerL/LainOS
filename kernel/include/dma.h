#ifndef DMA_H
#define DMA_H

#include <stdint.h>
#include "bootinfo.h"

void dma_init(const boot_info_t *info);
void *dma_alloc(uint32_t size, uint32_t alignment);
uint64_t dma_phys(const void *ptr);
uint64_t dma_pool_base(void);
uint64_t dma_pool_size(void);

#endif
