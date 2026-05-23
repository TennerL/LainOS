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
    if (size == 0 || size > (32u * 1024u * 1024u)) {
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
    if (new_size > (32u * 1024u * 1024u)) {
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
#define IMAGE_SCREEN_MAX_DECODE_PIXELS (2048u * 2048u)
#define IMAGE_SCREEN_MAX_TARGET_PIXELS (1024u * 768u)

static uint32_t image_blend_rgb(uint32_t dst, uint32_t src, uint32_t alpha) {
    uint32_t inv;
    uint32_t sr;
    uint32_t sg;
    uint32_t sb;
    uint32_t dr;
    uint32_t dg;
    uint32_t db;

    if (alpha >= 255u) {
        return src;
    }
    if (alpha == 0u) {
        return dst;
    }
    inv = 255u - alpha;
    sr = (src >> 16) & 0xffu;
    sg = (src >> 8) & 0xffu;
    sb = src & 0xffu;
    dr = (dst >> 16) & 0xffu;
    dg = (dst >> 8) & 0xffu;
    db = dst & 0xffu;
    return (((sr * alpha + dr * inv) / 255u) << 16) |
           (((sg * alpha + dg * inv) / 255u) << 8) |
           ((sb * alpha + db * inv) / 255u);
}

typedef struct {
    stbi_uc *pixels;
    stbi_uc *owned_pixels;
    int width;
    int height;
    int components;
} image_rgba_frame_t;

static int image_load_rgba_frame(const uint8_t *data,
                                 uint32_t size,
                                 image_rgba_frame_t *frame) {
    frame->pixels = 0;
    frame->owned_pixels = 0;
    frame->width = 0;
    frame->height = 0;
    frame->components = 0;

    frame->owned_pixels = stbi_load_from_memory(data,
                                                (int)size,
                                                &frame->width,
                                                &frame->height,
                                                &frame->components,
                                                4);
    if (frame->owned_pixels == 0) {
        return IMAGE_ERR_DECODE;
    }
    frame->pixels = frame->owned_pixels;
    return IMAGE_OK;
}

static void image_free_rgba_frame(image_rgba_frame_t *frame) {
    if (frame->owned_pixels != 0) {
        STBI_FREE(frame->owned_pixels);
    }
    frame->pixels = 0;
    frame->owned_pixels = 0;
}

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

int image_decode_to_screen_scaled(const uint8_t *data,
                                  uint32_t size,
                                  uint32_t origin_x,
                                  uint32_t origin_y,
                                  uint32_t max_width,
                                  uint32_t max_height) {
    image_info_t info;
    image_rgba_frame_t decoded;
    uint32_t target_width;
    uint32_t target_height;
    uint32_t clip_width;
    uint32_t clip_height;
    uint32_t x;
    uint32_t y;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t index;
    uint32_t color;
    uint32_t alpha;
    int width = 0;
    int height = 0;
    int components = 0;
    int rc;

    if (max_width == 0u || max_height == 0u) {
        return IMAGE_ERR_OUTPUT;
    }
    if (origin_x >= graphics_width() || origin_y >= graphics_height()) {
        return IMAGE_ERR_OUTPUT;
    }

    rc = image_probe(data, size, &info);
    if (rc != IMAGE_OK) {
        return rc;
    }
    if (info.width > IMAGE_SCREEN_MAX_DECODE_PIXELS / info.height) {
        return IMAGE_ERR_UNSUPPORTED;
    }

    target_width = info.width;
    target_height = info.height;
    if (target_width > max_width) {
        target_height = (uint32_t)(((uint64_t)target_height * max_width) / target_width);
        target_width = max_width;
    }
    if (target_height > max_height) {
        target_width = (uint32_t)(((uint64_t)target_width * max_height) / target_height);
        target_height = max_height;
    }
    if (target_width == 0u) {
        target_width = 1u;
    }
    if (target_height == 0u) {
        target_height = 1u;
    }
    if (target_width > IMAGE_SCREEN_MAX_TARGET_PIXELS / target_height) {
        return IMAGE_ERR_OUTPUT;
    }

    clip_width = target_width;
    clip_height = target_height;
    if (clip_width > graphics_width() - origin_x) {
        clip_width = graphics_width() - origin_x;
    }
    if (clip_height > graphics_height() - origin_y) {
        clip_height = graphics_height() - origin_y;
    }
    if (clip_width == 0u || clip_height == 0u) {
        return IMAGE_ERR_OUTPUT;
    }

    rc = image_load_rgba_frame(data, size, &decoded);
    if (rc != IMAGE_OK) {
        return rc;
    }
    width = decoded.width;
    height = decoded.height;
    components = decoded.components;

    rc = image_fill_info(width, height, components, &info);
    if (rc == IMAGE_OK) {
        if (info.width > IMAGE_SCREEN_MAX_DECODE_PIXELS / info.height) {
            rc = IMAGE_ERR_UNSUPPORTED;
        }
    }
    if (rc == IMAGE_OK) {
        for (y = 0; y < clip_height; ++y) {
            src_y = (uint32_t)(((uint64_t)y * info.height) / target_height);
            if (src_y >= info.height) {
                src_y = info.height - 1u;
            }
            for (x = 0; x < clip_width; ++x) {
                src_x = (uint32_t)(((uint64_t)x * info.width) / target_width);
                if (src_x >= info.width) {
                    src_x = info.width - 1u;
                }
                index = (src_y * info.width + src_x) * 4u;
                color = ((uint32_t)decoded.pixels[index] << 16) |
                        ((uint32_t)decoded.pixels[index + 1u] << 8) |
                        (uint32_t)decoded.pixels[index + 2u];
                alpha = (uint32_t)decoded.pixels[index + 3u];
                if (alpha != 255u) {
                    color = image_blend_rgb(graphics_get_pixel(origin_x + x, origin_y + y), color, alpha);
                }
                graphics_put_pixel(origin_x + x, origin_y + y, color);
            }
        }
    }

    image_free_rgba_frame(&decoded);
    return rc;
}

int image_decode_scaled_to_packed(const uint8_t *data,
                                  uint32_t size,
                                  uint32_t target_width,
                                  uint32_t target_height,
                                  uint32_t bg_rgb_color,
                                  uint32_t *pixels) {
    image_info_t info;
    image_rgba_frame_t decoded;
    uint32_t x;
    uint32_t y;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t index;
    uint32_t color;
    uint32_t alpha;
    int width = 0;
    int height = 0;
    int components = 0;
    int rc;

    if (target_width == 0u || target_height == 0u || pixels == 0) {
        return IMAGE_ERR_OUTPUT;
    }
    if (target_width > 0xffffffffu / target_height ||
        target_width > IMAGE_SCREEN_MAX_TARGET_PIXELS / target_height) {
        return IMAGE_ERR_OUTPUT;
    }

    rc = image_probe(data, size, &info);
    if (rc != IMAGE_OK) {
        return rc;
    }
    if (info.width > IMAGE_SCREEN_MAX_DECODE_PIXELS / info.height) {
        return IMAGE_ERR_UNSUPPORTED;
    }

    rc = image_load_rgba_frame(data, size, &decoded);
    if (rc != IMAGE_OK) {
        return rc;
    }
    width = decoded.width;
    height = decoded.height;
    components = decoded.components;

    rc = image_fill_info(width, height, components, &info);
    if (rc == IMAGE_OK && info.width > IMAGE_SCREEN_MAX_DECODE_PIXELS / info.height) {
        rc = IMAGE_ERR_UNSUPPORTED;
    }
    if (rc == IMAGE_OK) {
        for (y = 0; y < target_height; ++y) {
            src_y = (uint32_t)(((uint64_t)y * info.height) / target_height);
            if (src_y >= info.height) {
                src_y = info.height - 1u;
            }
            for (x = 0; x < target_width; ++x) {
                src_x = (uint32_t)(((uint64_t)x * info.width) / target_width);
                if (src_x >= info.width) {
                    src_x = info.width - 1u;
                }
                index = (src_y * info.width + src_x) * 4u;
                color = ((uint32_t)decoded.pixels[index] << 16) |
                        ((uint32_t)decoded.pixels[index + 1u] << 8) |
                        (uint32_t)decoded.pixels[index + 2u];
                alpha = (uint32_t)decoded.pixels[index + 3u];
                if (alpha != 255u) {
                    color = image_blend_rgb(bg_rgb_color, color, alpha);
                }
                pixels[(uint64_t)y * target_width + x] = graphics_pack_color(color);
            }
        }
    }

    image_free_rgba_frame(&decoded);
    return rc;
}

int image_decode_to_screen_tiled(const uint8_t *data,
                                 uint32_t size,
                                 uint32_t origin_x,
                                 uint32_t origin_y,
                                 uint32_t width,
                                 uint32_t height) {
    image_info_t info;
    image_rgba_frame_t decoded;
    uint32_t clip_width;
    uint32_t clip_height;
    uint32_t x;
    uint32_t y;
    uint32_t src_x;
    uint32_t src_y;
    uint32_t index;
    uint32_t color;
    uint32_t alpha;
    int image_width = 0;
    int image_height = 0;
    int components = 0;
    int rc;

    if (width == 0u || height == 0u) {
        return IMAGE_ERR_OUTPUT;
    }
    if (origin_x >= graphics_width() || origin_y >= graphics_height()) {
        return IMAGE_ERR_OUTPUT;
    }
    if (width > IMAGE_SCREEN_MAX_DECODE_PIXELS / height) {
        return IMAGE_ERR_OUTPUT;
    }

    rc = image_probe(data, size, &info);
    if (rc != IMAGE_OK) {
        return rc;
    }
    if (info.width > IMAGE_SCREEN_MAX_DECODE_PIXELS / info.height) {
        return IMAGE_ERR_UNSUPPORTED;
    }

    clip_width = width;
    clip_height = height;
    if (clip_width > graphics_width() - origin_x) {
        clip_width = graphics_width() - origin_x;
    }
    if (clip_height > graphics_height() - origin_y) {
        clip_height = graphics_height() - origin_y;
    }
    if (clip_width == 0u || clip_height == 0u) {
        return IMAGE_ERR_OUTPUT;
    }

    rc = image_load_rgba_frame(data, size, &decoded);
    if (rc != IMAGE_OK) {
        return rc;
    }
    image_width = decoded.width;
    image_height = decoded.height;
    components = decoded.components;

    rc = image_fill_info(image_width, image_height, components, &info);
    if (rc == IMAGE_OK) {
        if (info.width > IMAGE_SCREEN_MAX_DECODE_PIXELS / info.height) {
            rc = IMAGE_ERR_UNSUPPORTED;
        }
    }
    if (rc == IMAGE_OK) {
        for (y = 0; y < clip_height; ++y) {
            src_y = y % info.height;
            for (x = 0; x < clip_width; ++x) {
                src_x = x % info.width;
                index = (src_y * info.width + src_x) * 4u;
                color = ((uint32_t)decoded.pixels[index] << 16) |
                        ((uint32_t)decoded.pixels[index + 1u] << 8) |
                        (uint32_t)decoded.pixels[index + 2u];
                alpha = (uint32_t)decoded.pixels[index + 3u];
                if (alpha != 255u) {
                    color = image_blend_rgb(graphics_get_pixel(origin_x + x, origin_y + y), color, alpha);
                }
                graphics_put_pixel(origin_x + x, origin_y + y, color);
            }
        }
    }

    image_free_rgba_frame(&decoded);
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
