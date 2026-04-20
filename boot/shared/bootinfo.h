#ifndef BOOTINFO_H
#define BOOTINFO_H

#if defined(__GNUC__)
#include <stdint.h>
typedef uint64_t BOOTINFO_U64;
typedef uint32_t BOOTINFO_U32;
#else
#include <efi.h>
typedef UINT64 BOOTINFO_U64;
typedef UINT32 BOOTINFO_U32;
#endif

#define BOOTINFO_MAGIC 0x4C41494E424F4F54ULL

typedef struct {
    BOOTINFO_U64 magic;
    BOOTINFO_U64 kernel_base;
    BOOTINFO_U64 kernel_size;
    BOOTINFO_U64 memory_map;
    BOOTINFO_U64 memory_map_size;
    BOOTINFO_U64 memory_map_descriptor_size;
    BOOTINFO_U64 memory_map_descriptor_version;
    BOOTINFO_U64 framebuffer_base;
    BOOTINFO_U32 framebuffer_width;
    BOOTINFO_U32 framebuffer_height;
    BOOTINFO_U32 framebuffer_pixels_per_scanline;
    BOOTINFO_U32 framebuffer_format;
    BOOTINFO_U64 rsdp;
} boot_info_t;

#endif
