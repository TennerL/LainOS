#ifndef KERNEL_EXPORTS_H
#define KERNEL_EXPORTS_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint64_t value;
} kernel_export_t;

const kernel_export_t *kernel_exports_table(uint32_t *out_count);
int kernel_export_value(const char *name, uint64_t *out);

#endif
