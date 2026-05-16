#include <stddef.h>
#include <stdint.h>

#include "graphics.h"
#include "image.h"
#include "kmem.h"

void *memcpy(void *dst, const void *src, size_t len);

static int image_abs(int value) {
    return value < 0 ? -value : value;
}

static void *image_stbi_malloc(size_t size) {
    if (size == 0 || size > 0xffffffffu) {
        return 0;
    }
    return kmalloc((uint32_t)size);
}

static void image_stbi_free(void *ptr) {
    if (ptr != 0) {
        kfree(ptr);
    }
}

static void *image_stbi_realloc_sized(void *ptr, size_t old_size, size_t new_size) {
    void *next;
    size_t copy_size;

    if (ptr == 0) {
        return image_stbi_malloc(new_size);
    }
    if (new_size == 0) {
        image_stbi_free(ptr);
        return 0;
    }
    if (new_size > 0xffffffffu) {
        return 0;
    }

    next = kmalloc((uint32_t)new_size);
    if (next == 0) {
        return 0;
    }

    copy_size = old_size;
    if (copy_size > new_size) {
        copy_size = new_size;
    }
    memcpy(next, ptr, copy_size);
    kfree(ptr);
    return next;
}

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_ONLY_PSD
#define STBI_ONLY_PIC
#define STBI_ONLY_PNM
#define STBI_NO_FAILURE_STRINGS
#define STBI_MALLOC(sz) image_stbi_malloc((size_t)(sz))
#define STBI_REALLOC_SIZED(p, oldsz, newsz) image_stbi_realloc_sized((p), (size_t)(oldsz), (size_t)(newsz))
#define STBI_FREE(p) image_stbi_free((p))
#define STBI_ASSERT(x) do { (void)sizeof(x); } while (0)
#define abs image_abs
#include "stb_image.h"
#undef abs

#define IMAGE_MAX_DIMENSION 4096u
#define IMAGE_MAX_PIXELS (IMAGE_MAX_DIMENSION * IMAGE_MAX_DIMENSION)

static int image_fill_info(int width, int height, int components, image_info_t *out_image) {
    uint32_t w;
    uint32_t h;

    if (width <= 0 || height <= 0) {
        return IMAGE_ERR_FORMAT;
    }

    w = (uint32_t)width;
    h = (uint32_t)height;
    if (w > IMAGE_MAX_DIMENSION || h > IMAGE_MAX_DIMENSION || w > IMAGE_MAX_PIXELS / h) {
        return IMAGE_ERR_UNSUPPORTED;
    }

    if (out_image != 0) {
        out_image->width = w;
        out_image->height = h;
        out_image->components = components > 0 ? (uint32_t)components : 0u;
        out_image->stride = w * 3u;
    }
    return IMAGE_OK;
}

int image_probe(const uint8_t *data, uint32_t size, image_info_t *out_image) {
    int width = 0;
    int height = 0;
    int components = 0;

    if (data == 0 || size == 0 || size > 0x7fffffffu) {
        return IMAGE_ERR_INPUT;
    }

    if (!stbi_info_from_memory(data, (int)size, &width, &height, &components)) {
        return IMAGE_ERR_FORMAT;
    }

    return image_fill_info(width, height, components, out_image);
}

int image_decode_rgb24(const uint8_t *data,
                       uint32_t size,
                       uint8_t *rgb,
                       uint32_t rgb_capacity,
                       image_info_t *out_image) {
    image_info_t info;
    stbi_uc *decoded;
    uint32_t byte_count;
    int width = 0;
    int height = 0;
    int components = 0;
    int rc;

    if (rgb == 0) {
        return IMAGE_ERR_OUTPUT;
    }

    rc = image_probe(data, size, &info);
    if (rc != IMAGE_OK) {
        return rc;
    }
    if (info.width > 0xffffffffu / info.height / 3u) {
        return IMAGE_ERR_OUTPUT;
    }
    byte_count = info.width * info.height * 3u;
    if (rgb_capacity < byte_count) {
        return IMAGE_ERR_OUTPUT;
    }

    decoded = stbi_load_from_memory(data, (int)size, &width, &height, &components, 3);
    if (decoded == 0) {
        return IMAGE_ERR_DECODE;
    }

    rc = image_fill_info(width, height, components, &info);
    if (rc == IMAGE_OK) {
        byte_count = info.width * info.height * 3u;
        if (rgb_capacity >= byte_count) {
            memcpy(rgb, decoded, byte_count);
            if (out_image != 0) {
                *out_image = info;
            }
        } else {
            rc = IMAGE_ERR_OUTPUT;
        }
    }

    STBI_FREE(decoded);
    return rc;
}

int image_decode_to_screen(const uint8_t *data,
                           uint32_t size,
                           uint32_t origin_x,
                           uint32_t origin_y,
                           image_info_t *out_image) {
    image_info_t info;
    stbi_uc *decoded;
    uint32_t x;
    uint32_t y;
    uint32_t index;
    uint32_t color;
    int width = 0;
    int height = 0;
    int components = 0;
    int rc;

    rc = image_probe(data, size, &info);
    if (rc != IMAGE_OK) {
        return rc;
    }

    decoded = stbi_load_from_memory(data, (int)size, &width, &height, &components, 3);
    if (decoded == 0) {
        return IMAGE_ERR_DECODE;
    }

    rc = image_fill_info(width, height, components, &info);
    if (rc == IMAGE_OK) {
        for (y = 0; y < info.height; ++y) {
            for (x = 0; x < info.width; ++x) {
                index = (y * info.width + x) * 3u;
                color = ((uint32_t)decoded[index] << 16) |
                        ((uint32_t)decoded[index + 1u] << 8) |
                        (uint32_t)decoded[index + 2u];
                graphics_put_pixel(origin_x + x, origin_y + y, color);
            }
        }
        if (out_image != 0) {
            *out_image = info;
        }
    }

    STBI_FREE(decoded);
    return rc;
}

const char *image_supported_formats(void) {
    return "JPEG PNG BMP GIF TGA PSD PIC PNM";
}

int jpg_probe(const uint8_t *data, uint32_t size, image_info_t *out_image) {
    return image_probe(data, size, out_image);
}

int jpg_decode_rgb24(const uint8_t *data,
                     uint32_t size,
                     uint8_t *rgb,
                     uint32_t rgb_capacity,
                     image_info_t *out_image) {
    return image_decode_rgb24(data, size, rgb, rgb_capacity, out_image);
}

int jpg_decode_to_screen(const uint8_t *data,
                         uint32_t size,
                         uint32_t origin_x,
                         uint32_t origin_y,
                         image_info_t *out_image) {
    return image_decode_to_screen(data, size, origin_x, origin_y, out_image);
}

uint32_t jpg_entropy_error_detail(void) {
    return 0;
}

uint32_t jpg_entropy_error_block(void) {
    return 0;
}

uint32_t jpg_huffman_code_count(uint32_t slot, uint32_t length) {
    (void)slot;
    (void)length;
    return 0;
}

uint32_t jpg_huffman_value(uint32_t slot, uint32_t index) {
    (void)slot;
    (void)index;
    return 0;
}

int jpg_entropy_probe_first(const uint8_t *data, uint32_t size) {
    (void)data;
    (void)size;
    return IMAGE_ERR_UNSUPPORTED;
}

int jpg_decode_first_block_probe(const uint8_t *data, uint32_t size) {
    (void)data;
    (void)size;
    return IMAGE_ERR_UNSUPPORTED;
}

int jpg_decode_two_block_probe(const uint8_t *data, uint32_t size) {
    (void)data;
    (void)size;
    return IMAGE_ERR_UNSUPPORTED;
}

int jpg_decode_first_ac_probe(const uint8_t *data, uint32_t size) {
    (void)data;
    (void)size;
    return IMAGE_ERR_UNSUPPORTED;
}

uint32_t jpg_debug_scan_offset(void) {
    return 0;
}

uint32_t jpg_debug_stream_pos(void) {
    return 0;
}

uint32_t jpg_debug_bits_left(void) {
    return 0;
}

uint32_t jpg_debug_zigzag(uint32_t index) {
    static const uint8_t zigzag[64] = {
        0, 1, 8, 16, 9, 2, 3, 10,
        17, 24, 32, 25, 18, 11, 4, 5,
        12, 19, 26, 33, 40, 48, 41, 34,
        27, 20, 13, 6, 7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36,
        29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46,
        53, 60, 61, 54, 47, 55, 62, 63
    };

    if (index >= 64u) {
        return 0;
    }
    return zigzag[index];
}

uint32_t jpg_debug_byte_at(const uint8_t *data, uint32_t pos) {
    if (data == 0) {
        return 0;
    }
    return data[pos];
}

int jpg_debug_first_entropy_bits(const uint8_t *data, uint32_t size) {
    (void)data;
    (void)size;
    return IMAGE_ERR_UNSUPPORTED;
}
