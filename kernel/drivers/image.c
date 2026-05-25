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

typedef struct {
    int width;
    int height;
    int components;
    int ascii;
    int bitmap;
    int max_value;
    uint32_t data_offset;
} image_pnm_info_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
    uint32_t offset;
    uint32_t bit_count;
    int png;
} image_ico_entry_t;

static int image_fill_info(int width, int height, int components, image_info_t *out_image);

static uint16_t image_read_le16(const uint8_t *data) {
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t image_read_le32(const uint8_t *data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static int32_t image_read_le_i32(const uint8_t *data) {
    return (int32_t)image_read_le32(data);
}

static int image_is_png_signature(const uint8_t *data, uint32_t size) {
    return size >= 8u &&
           data[0] == 0x89u &&
           data[1] == 'P' &&
           data[2] == 'N' &&
           data[3] == 'G' &&
           data[4] == 0x0du &&
           data[5] == 0x0au &&
           data[6] == 0x1au &&
           data[7] == 0x0au;
}

static int image_is_ico(const uint8_t *data, uint32_t size) {
    uint16_t type;
    uint16_t count;

    if (data == 0 || size < 6u || image_read_le16(data) != 0u) {
        return 0;
    }
    type = image_read_le16(data + 2u);
    count = image_read_le16(data + 4u);
    return (type == 1u || type == 2u) && count != 0u;
}

static int image_ico_read_entry(const uint8_t *data,
                                uint32_t size,
                                uint32_t index,
                                image_ico_entry_t *entry) {
    uint32_t pos;
    uint32_t width;
    uint32_t height;
    uint32_t bytes;
    uint32_t offset;

    pos = 6u + index * 16u;
    if (entry == 0 || pos > size || size - pos < 16u) {
        return IMAGE_ERR_FORMAT;
    }

    width = data[pos] == 0u ? 256u : (uint32_t)data[pos];
    height = data[pos + 1u] == 0u ? 256u : (uint32_t)data[pos + 1u];
    bytes = image_read_le32(data + pos + 8u);
    offset = image_read_le32(data + pos + 12u);
    if (width == 0u ||
        height == 0u ||
        width > IMAGE_MAX_DIMENSION ||
        height > IMAGE_MAX_DIMENSION ||
        width > IMAGE_MAX_PIXELS / height ||
        bytes == 0u ||
        offset > size ||
        bytes > size - offset) {
        return IMAGE_ERR_FORMAT;
    }

    entry->width = width;
    entry->height = height;
    entry->bytes = bytes;
    entry->offset = offset;
    entry->bit_count = (uint32_t)image_read_le16(data + pos + 6u);
    entry->png = image_is_png_signature(data + offset, bytes);
    return IMAGE_OK;
}

static int image_ico_select_entry(const uint8_t *data,
                                  uint32_t size,
                                  image_ico_entry_t *selected) {
    uint32_t count;
    uint32_t i;
    uint32_t best_score;
    int found;

    if (!image_is_ico(data, size) || selected == 0) {
        return IMAGE_ERR_FORMAT;
    }
    count = (uint32_t)image_read_le16(data + 4u);
    if (count > (size - 6u) / 16u) {
        return IMAGE_ERR_FORMAT;
    }

    best_score = 0u;
    found = 0;
    for (i = 0; i < count; ++i) {
        image_ico_entry_t candidate;
        uint32_t area;
        uint32_t bit_count;
        uint32_t score;

        if (image_ico_read_entry(data, size, i, &candidate) != IMAGE_OK) {
            continue;
        }
        area = candidate.width * candidate.height;
        bit_count = candidate.bit_count;
        if (bit_count == 0u) {
            bit_count = candidate.png ? 32u : 1u;
        }
        score = area * 64u + bit_count;
        if (candidate.png) {
            score += 32u;
        }
        if (found == 0 || score > best_score) {
            *selected = candidate;
            best_score = score;
            found = 1;
        }
    }

    return found != 0 ? IMAGE_OK : IMAGE_ERR_UNSUPPORTED;
}

static int image_probe_ico_info(const uint8_t *data, uint32_t size, image_info_t *out_image) {
    image_ico_entry_t entry;
    int width;
    int height;
    int components;

    if (image_ico_select_entry(data, size, &entry) != IMAGE_OK) {
        return image_is_ico(data, size) ? IMAGE_ERR_UNSUPPORTED : IMAGE_ERR_FORMAT;
    }

    width = (int)entry.width;
    height = (int)entry.height;
    components = entry.bit_count >= 32u ? 4 : 3;
    if (entry.png &&
        stbi_info_from_memory(data + entry.offset,
                              (int)entry.bytes,
                              &width,
                              &height,
                              &components)) {
        /* stbi filled the real embedded PNG dimensions. */
    }
    return image_fill_info(width, height, components, out_image);
}

static uint32_t image_ico_stride(uint32_t width, uint32_t bits_per_pixel) {
    return ((width * bits_per_pixel + 31u) / 32u) * 4u;
}

static int image_ico_load_png_entry(const uint8_t *data,
                                    const image_ico_entry_t *entry,
                                    image_rgba_frame_t *frame) {
    int width;
    int height;
    int components;

    width = 0;
    height = 0;
    components = 0;
    frame->owned_pixels = stbi_load_from_memory(data + entry->offset,
                                                (int)entry->bytes,
                                                &width,
                                                &height,
                                                &components,
                                                4);
    if (frame->owned_pixels == 0) {
        return IMAGE_ERR_DECODE;
    }
    if (image_fill_info(width, height, components, 0) != IMAGE_OK) {
        STBI_FREE(frame->owned_pixels);
        frame->owned_pixels = 0;
        return IMAGE_ERR_UNSUPPORTED;
    }
    frame->pixels = frame->owned_pixels;
    frame->width = width;
    frame->height = height;
    frame->components = 4;
    return IMAGE_OK;
}

static int image_ico_load_dib_entry(const uint8_t *data,
                                    const image_ico_entry_t *entry,
                                    image_rgba_frame_t *frame) {
    const uint8_t *dib;
    uint32_t dib_size;
    uint32_t header_size;
    uint32_t width;
    uint32_t height;
    uint32_t bit_count;
    uint32_t colors_used;
    uint32_t palette_count;
    uint32_t palette_offset;
    uint32_t xor_offset;
    uint32_t xor_stride;
    uint32_t mask_stride;
    uint32_t mask_offset;
    uint32_t byte_count;
    uint32_t x;
    uint32_t y;
    int32_t dib_width;
    int32_t dib_height;
    int top_down;
    int has_alpha;
    stbi_uc *pixels;

    dib = data + entry->offset;
    dib_size = entry->bytes;
    if (dib_size < 40u) {
        return IMAGE_ERR_UNSUPPORTED;
    }

    header_size = image_read_le32(dib);
    if (header_size < 40u || header_size > dib_size) {
        return IMAGE_ERR_UNSUPPORTED;
    }
    dib_width = image_read_le_i32(dib + 4u);
    dib_height = image_read_le_i32(dib + 8u);
    if (dib_width <= 0 || dib_height == 0) {
        return IMAGE_ERR_UNSUPPORTED;
    }
    top_down = dib_height < 0;
    width = (uint32_t)dib_width;
    height = top_down ? (uint32_t)(-dib_height) : (uint32_t)dib_height;
    if (height >= entry->height * 2u) {
        height = height / 2u;
    } else if (entry->height != 0u) {
        height = entry->height;
    }
    if (width != entry->width && entry->width != 0u) {
        width = entry->width;
    }

    if (width == 0u ||
        height == 0u ||
        width > IMAGE_MAX_DIMENSION ||
        height > IMAGE_MAX_DIMENSION ||
        width > IMAGE_MAX_PIXELS / height) {
        return IMAGE_ERR_UNSUPPORTED;
    }

    if (image_read_le16(dib + 12u) != 1u) {
        return IMAGE_ERR_UNSUPPORTED;
    }
    bit_count = (uint32_t)image_read_le16(dib + 14u);
    if (bit_count != 32u &&
        bit_count != 24u &&
        bit_count != 8u &&
        bit_count != 4u &&
        bit_count != 1u) {
        return IMAGE_ERR_UNSUPPORTED;
    }
    if (image_read_le32(dib + 16u) != 0u) {
        return IMAGE_ERR_UNSUPPORTED;
    }

    colors_used = header_size >= 40u ? image_read_le32(dib + 32u) : 0u;
    palette_count = 0u;
    if (bit_count <= 8u) {
        palette_count = colors_used != 0u ? colors_used : (1u << bit_count);
    }
    palette_offset = header_size;
    xor_offset = palette_offset + palette_count * 4u;
    xor_stride = image_ico_stride(width, bit_count);
    mask_stride = image_ico_stride(width, 1u);
    if (xor_offset > dib_size ||
        xor_stride > 0xffffffffu / height ||
        xor_stride * height > dib_size - xor_offset) {
        return IMAGE_ERR_FORMAT;
    }
    mask_offset = xor_offset + xor_stride * height;

    if (palette_count != 0u &&
        (palette_offset > dib_size || palette_count > (dib_size - palette_offset) / 4u)) {
        return IMAGE_ERR_FORMAT;
    }

    if (width > 0xffffffffu / height / 4u) {
        return IMAGE_ERR_OUTPUT;
    }
    byte_count = width * height * 4u;
    pixels = image_stbi_malloc(byte_count);
    if (pixels == 0) {
        return IMAGE_ERR_OUTPUT;
    }

    has_alpha = 0;
    if (bit_count == 32u) {
        for (y = 0; y < height && has_alpha == 0; ++y) {
            const uint8_t *row = dib + xor_offset + y * xor_stride;
            for (x = 0; x < width; ++x) {
                if (row[x * 4u + 3u] != 0u) {
                    has_alpha = 1;
                    break;
                }
            }
        }
    }

    for (y = 0; y < height; ++y) {
        uint32_t src_y;
        const uint8_t *row;
        const uint8_t *mask_row;

        src_y = top_down ? y : (height - 1u - y);
        row = dib + xor_offset + src_y * xor_stride;
        mask_row = 0;
        if (mask_offset <= dib_size && mask_stride <= (dib_size - mask_offset) / height) {
            mask_row = dib + mask_offset + src_y * mask_stride;
        }

        for (x = 0; x < width; ++x) {
            uint8_t r;
            uint8_t g;
            uint8_t b;
            uint8_t a;
            uint8_t index;
            stbi_uc *dst;

            r = 0u;
            g = 0u;
            b = 0u;
            a = 0xffu;
            if (bit_count == 32u) {
                b = row[x * 4u];
                g = row[x * 4u + 1u];
                r = row[x * 4u + 2u];
                a = has_alpha != 0 ? row[x * 4u + 3u] : 0xffu;
            } else if (bit_count == 24u) {
                b = row[x * 3u];
                g = row[x * 3u + 1u];
                r = row[x * 3u + 2u];
            } else {
                if (bit_count == 8u) {
                    index = row[x];
                } else if (bit_count == 4u) {
                    uint8_t packed = row[x / 2u];
                    index = (x & 1u) == 0u ? (uint8_t)(packed >> 4) : (uint8_t)(packed & 0x0fu);
                } else {
                    index = (uint8_t)((row[x / 8u] >> (7u - (x & 7u))) & 1u);
                }
                if ((uint32_t)index >= palette_count) {
                    image_stbi_free(pixels);
                    return IMAGE_ERR_FORMAT;
                }
                b = dib[palette_offset + (uint32_t)index * 4u];
                g = dib[palette_offset + (uint32_t)index * 4u + 1u];
                r = dib[palette_offset + (uint32_t)index * 4u + 2u];
            }

            if (mask_row != 0 &&
                ((mask_row[x / 8u] >> (7u - (x & 7u))) & 1u) != 0u) {
                a = 0u;
            }
            dst = pixels + ((size_t)y * (size_t)width + (size_t)x) * 4u;
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst[3] = a;
        }
    }

    frame->pixels = pixels;
    frame->owned_pixels = pixels;
    frame->width = (int)width;
    frame->height = (int)height;
    frame->components = 4;
    return IMAGE_OK;
}

static int image_load_ico_rgba(const uint8_t *data, uint32_t size, image_rgba_frame_t *frame) {
    image_ico_entry_t entry;
    int rc;

    if (frame == 0) {
        return IMAGE_ERR_OUTPUT;
    }
    rc = image_ico_select_entry(data, size, &entry);
    if (rc != IMAGE_OK) {
        return image_is_ico(data, size) ? rc : IMAGE_ERR_FORMAT;
    }
    if (entry.png) {
        return image_ico_load_png_entry(data, &entry, frame);
    }
    return image_ico_load_dib_entry(data, &entry, frame);
}

static int image_parse_decimal(const uint8_t *data, uint32_t size, uint32_t *pos, uint32_t *out_value) {
    uint32_t value;
    int saw_digit;

    value = 0;
    saw_digit = 0;
    while (*pos < size && data[*pos] >= '0' && data[*pos] <= '9') {
        value = value * 10u + (uint32_t)(data[*pos] - '0');
        *pos = *pos + 1u;
        saw_digit = 1;
    }
    if (saw_digit == 0) {
        return 0;
    }
    *out_value = value;
    return 1;
}

static void image_skip_pnm_ws_and_comments(const uint8_t *data, uint32_t size, uint32_t *pos) {
    while (*pos < size) {
        if (data[*pos] == '#') {
            while (*pos < size && data[*pos] != '\n' && data[*pos] != '\r') {
                *pos = *pos + 1u;
            }
            continue;
        }
        if (data[*pos] == ' ' ||
            data[*pos] == '\t' ||
            data[*pos] == '\n' ||
            data[*pos] == '\r' ||
            data[*pos] == '\f' ||
            data[*pos] == '\v') {
            *pos = *pos + 1u;
            continue;
        }
        break;
    }
}

static int image_parse_pnm_info(const uint8_t *data, uint32_t size, image_pnm_info_t *out_info) {
    uint32_t pos;
    uint32_t width;
    uint32_t height;
    uint32_t max_value;
    int ascii;
    int bitmap;
    int components;
    uint8_t kind;

    if (out_info == 0 || data == 0 || size < 3u || data[0] != 'P') {
        return IMAGE_ERR_FORMAT;
    }

    kind = data[1];
    if (kind < '1' || kind > '6') {
        return IMAGE_ERR_FORMAT;
    }
    ascii = (kind == '1' || kind == '2' || kind == '3');
    bitmap = (kind == '1' || kind == '4');
    components = (kind == '3' || kind == '6') ? 3 : 1;
    pos = 2u;

    image_skip_pnm_ws_and_comments(data, size, &pos);
    if (image_parse_decimal(data, size, &pos, &width) == 0) {
        return IMAGE_ERR_FORMAT;
    }
    image_skip_pnm_ws_and_comments(data, size, &pos);
    if (image_parse_decimal(data, size, &pos, &height) == 0) {
        return IMAGE_ERR_FORMAT;
    }

    if (bitmap != 0) {
        max_value = 1u;
    } else {
        image_skip_pnm_ws_and_comments(data, size, &pos);
        if (image_parse_decimal(data, size, &pos, &max_value) == 0 || max_value == 0u || max_value > 65535u) {
            return IMAGE_ERR_FORMAT;
        }
    }

    if (width == 0u || height == 0u || width > 0x7fffffffu || height > 0x7fffffffu) {
        return IMAGE_ERR_UNSUPPORTED;
    }

    image_skip_pnm_ws_and_comments(data, size, &pos);
    if (pos >= size) {
        return IMAGE_ERR_FORMAT;
    }

    out_info->width = (int)width;
    out_info->height = (int)height;
    out_info->components = components;
    out_info->ascii = ascii;
    out_info->bitmap = bitmap;
    out_info->max_value = (int)max_value;
    out_info->data_offset = pos;
    return IMAGE_OK;
}

static uint8_t image_scale_pnm_sample(uint32_t sample, uint32_t max_value) {
    if (max_value <= 1u) {
        return sample == 0u ? 0xffu : 0x00u;
    }
    if (sample >= max_value) {
        return 0xffu;
    }
    return (uint8_t)((sample * 255u + max_value / 2u) / max_value);
}

static int image_load_ascii_pnm(const uint8_t *data, uint32_t size, image_rgba_frame_t *frame) {
    image_pnm_info_t info;
    uint32_t pos;
    uint32_t pixel_count;
    uint32_t byte_count;
    stbi_uc *pixels;
    uint32_t i;
    int rc;

    rc = image_parse_pnm_info(data, size, &info);
    if (rc != IMAGE_OK || info.ascii == 0) {
        return rc == IMAGE_OK ? IMAGE_ERR_FORMAT : rc;
    }

    pixel_count = (uint32_t)info.width * (uint32_t)info.height;
    if (pixel_count == 0u || pixel_count > IMAGE_MAX_PIXELS || pixel_count > 0xffffffffu / 4u) {
        return IMAGE_ERR_UNSUPPORTED;
    }
    byte_count = pixel_count * 4u;
    pixels = image_stbi_malloc(byte_count);
    if (pixels == 0) {
        return IMAGE_ERR_OUTPUT;
    }

    pos = info.data_offset;
    for (i = 0; i < pixel_count; ++i) {
        uint32_t r;
        uint32_t g;
        uint32_t b;
        uint8_t gray;

        image_skip_pnm_ws_and_comments(data, size, &pos);
        if (info.components == 3) {
            if (image_parse_decimal(data, size, &pos, &r) == 0) {
                image_stbi_free(pixels);
                return IMAGE_ERR_FORMAT;
            }
            image_skip_pnm_ws_and_comments(data, size, &pos);
            if (image_parse_decimal(data, size, &pos, &g) == 0) {
                image_stbi_free(pixels);
                return IMAGE_ERR_FORMAT;
            }
            image_skip_pnm_ws_and_comments(data, size, &pos);
            if (image_parse_decimal(data, size, &pos, &b) == 0) {
                image_stbi_free(pixels);
                return IMAGE_ERR_FORMAT;
            }
            pixels[i * 4u] = image_scale_pnm_sample(r, (uint32_t)info.max_value);
            pixels[i * 4u + 1u] = image_scale_pnm_sample(g, (uint32_t)info.max_value);
            pixels[i * 4u + 2u] = image_scale_pnm_sample(b, (uint32_t)info.max_value);
        } else {
            if (image_parse_decimal(data, size, &pos, &r) == 0) {
                image_stbi_free(pixels);
                return IMAGE_ERR_FORMAT;
            }
            gray = image_scale_pnm_sample(r, (uint32_t)info.max_value);
            pixels[i * 4u] = gray;
            pixels[i * 4u + 1u] = gray;
            pixels[i * 4u + 2u] = gray;
        }
        pixels[i * 4u + 3u] = 0xffu;
    }

    frame->pixels = pixels;
    frame->owned_pixels = pixels;
    frame->width = info.width;
    frame->height = info.height;
    frame->components = info.components;
    return IMAGE_OK;
}

static int image_load_rgba_frame(const uint8_t *data,
                                 uint32_t size,
                                 image_rgba_frame_t *frame) {
    image_pnm_info_t pnm_info;
    int rc;

    frame->pixels = 0;
    frame->owned_pixels = 0;
    frame->width = 0;
    frame->height = 0;
    frame->components = 0;

    if (image_parse_pnm_info(data, size, &pnm_info) == IMAGE_OK &&
        pnm_info.ascii != 0) {
        return image_load_ascii_pnm(data, size, frame);
    }

    rc = image_load_ico_rgba(data, size, frame);
    if (rc == IMAGE_OK || image_is_ico(data, size)) {
        return rc;
    }

    frame->owned_pixels = stbi_load_from_memory(data,
                                                (int)size,
                                                &frame->width,
                                                &frame->height,
                                                &frame->components,
                                                4);
    if (frame->owned_pixels == 0) {
        return image_load_ascii_pnm(data, size, frame);
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
    image_pnm_info_t pnm_info;
    int rc;

    if (data == 0 || size == 0 || size > 0x7fffffffu) {
        return IMAGE_ERR_INPUT;
    }

    if (image_parse_pnm_info(data, size, &pnm_info) == IMAGE_OK &&
        pnm_info.ascii != 0) {
        return image_fill_info(pnm_info.width, pnm_info.height, pnm_info.components, out_image);
    }

    rc = image_probe_ico_info(data, size, out_image);
    if (rc == IMAGE_OK || image_is_ico(data, size)) {
        return rc;
    }

    if (!stbi_info_from_memory(data, (int)size, &width, &height, &components)) {
        rc = image_parse_pnm_info(data, size, &pnm_info);
        if (rc != IMAGE_OK) {
            return rc == IMAGE_ERR_FORMAT ? IMAGE_ERR_FORMAT : rc;
        }
        width = pnm_info.width;
        height = pnm_info.height;
        components = pnm_info.components;
    }

    return image_fill_info(width, height, components, out_image);
}

int image_decode_rgb24(const uint8_t *data,
                       uint32_t size,
                       uint8_t *rgb,
                       uint32_t rgb_capacity,
                       image_info_t *out_image) {
    image_info_t info;
    image_rgba_frame_t decoded;
    uint32_t byte_count;
    uint32_t x;
    uint32_t y;
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

    rc = image_load_rgba_frame(data, size, &decoded);
    if (rc != IMAGE_OK) {
        return rc;
    }

    rc = image_fill_info(decoded.width, decoded.height, decoded.components, &info);
    if (rc == IMAGE_OK) {
        byte_count = info.width * info.height * 3u;
        if (rgb_capacity >= byte_count) {
            for (y = 0; y < info.height; ++y) {
                for (x = 0; x < info.width; ++x) {
                    uint32_t src_index = (y * info.width + x) * 4u;
                    uint32_t dst_index = (y * info.width + x) * 3u;
                    rgb[dst_index] = decoded.pixels[src_index];
                    rgb[dst_index + 1u] = decoded.pixels[src_index + 1u];
                    rgb[dst_index + 2u] = decoded.pixels[src_index + 2u];
                }
            }
            if (out_image != 0) {
                info.components = 3u;
                info.stride = info.width * 3u;
                *out_image = info;
            }
        } else {
            rc = IMAGE_ERR_OUTPUT;
        }
    }

    image_free_rgba_frame(&decoded);
    return rc;
}

int image_decode_rgba32(const uint8_t *data,
                        uint32_t size,
                        uint8_t *rgba,
                        uint32_t rgba_capacity,
                        image_info_t *out_image) {
    image_info_t info;
    image_rgba_frame_t decoded;
    uint32_t byte_count;
    int width = 0;
    int height = 0;
    int components = 0;
    int rc;

    if (rgba == 0) {
        return IMAGE_ERR_OUTPUT;
    }

    rc = image_probe(data, size, &info);
    if (rc != IMAGE_OK) {
        return rc;
    }
    if (info.width > 0xffffffffu / info.height / 4u) {
        return IMAGE_ERR_OUTPUT;
    }
    byte_count = info.width * info.height * 4u;
    if (rgba_capacity < byte_count) {
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
        byte_count = info.width * info.height * 4u;
        if (rgba_capacity >= byte_count) {
            memcpy(rgba, decoded.pixels, byte_count);
            if (out_image != 0) {
                info.components = 4u;
                info.stride = info.width * 4u;
                *out_image = info;
            }
        } else {
            rc = IMAGE_ERR_OUTPUT;
        }
    }

    image_free_rgba_frame(&decoded);
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
    return "JPEG PNG BMP GIF ICO TGA PSD PIC PNM";
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
