#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <assert.h>

#include "graphics.h"
#include "kernel.h"
#include "kmem.h"
#include "libc.h"
#include "netsurf_frontend.h"
#include "utils/errors.h"
#include "netsurf/layout.h"
#include "netsurf/plotters.h"
#include "dejavu_sans_ttf.h"

static int netsurf_kernel_stb_floor(float value);
static int netsurf_kernel_stb_ceil(float value);
static double netsurf_kernel_stb_sqrt(double value);
static double netsurf_kernel_stb_fabs(double value);
static double netsurf_kernel_stb_fmod(double x, double y);
static double netsurf_kernel_stb_pow(double x, double y);
static double netsurf_kernel_stb_cos(double x);
static double netsurf_kernel_stb_acos(double x);

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(size, userdata) ((void)(userdata), kmalloc((uint32_t)(size)))
#define STBTT_free(ptr, userdata) ((void)(userdata), kfree(ptr))
#define STBTT_ifloor(x) netsurf_kernel_stb_floor((float)(x))
#define STBTT_iceil(x) netsurf_kernel_stb_ceil((float)(x))
#define STBTT_sqrt(x) netsurf_kernel_stb_sqrt((double)(x))
#define STBTT_pow(x, y) netsurf_kernel_stb_pow((double)(x), (double)(y))
#define STBTT_fmod(x, y) netsurf_kernel_stb_fmod((double)(x), (double)(y))
#define STBTT_cos(x) netsurf_kernel_stb_cos((double)(x))
#define STBTT_acos(x) netsurf_kernel_stb_acos((double)(x))
#define STBTT_fabs(x) netsurf_kernel_stb_fabs((double)(x))
#include "stb_truetype.h"

#define NETSURF_KERNEL_FRONTEND_LAYOUT       0x01u
#define NETSURF_KERNEL_FRONTEND_POSITION     0x02u
#define NETSURF_KERNEL_FRONTEND_SPLIT        0x04u
#define NETSURF_KERNEL_FRONTEND_PLOTTERS     0x08u
#define NETSURF_KERNEL_FRONTEND_CONTEXT      0x10u
#define NETSURF_KERNEL_FRONTEND_EXPECTED     0x1fu
#define NETSURF_KERNEL_POLYGON_POINT_LIMIT   128u
#define NETSURF_KERNEL_PATH_BEZIER_STEPS     8u
#define NETSURF_KERNEL_FONT_CACHE_SLOTS      192u

static netsurf_kernel_plot_stats_t netsurf_kernel_global_stats;
static uint32_t netsurf_kernel_frontend_last_status;
static bool netsurf_kernel_plot_output_enabled = true;
static bool netsurf_kernel_clip_valid;
static struct rect netsurf_kernel_clip;

typedef struct netsurf_kernel_font_cache_entry {
    uint32_t codepoint;
    int px;
    int width;
    int height;
    int xoff;
    int yoff;
    int advance;
    int lsb;
    uint32_t age;
    uint8_t *alpha;
} netsurf_kernel_font_cache_entry_t;

static stbtt_fontinfo netsurf_kernel_font_info;
static bool netsurf_kernel_font_ready;
static bool netsurf_kernel_font_tried;
static netsurf_kernel_font_cache_entry_t netsurf_kernel_font_cache[NETSURF_KERNEL_FONT_CACHE_SLOTS];
static uint32_t netsurf_kernel_font_cache_age;

static int netsurf_kernel_utf8_is_trail(unsigned char ch);
static int netsurf_kernel_font_px(const struct plot_font_style *fstyle);
static int netsurf_kernel_glyph_width(const struct plot_font_style *fstyle);
static int netsurf_kernel_ttf_char_width(const struct plot_font_style *fstyle, uint32_t codepoint);

static int netsurf_kernel_iabs(int value) {
    return value < 0 ? -value : value;
}

static int netsurf_kernel_float_to_int(float value) {
    return (int)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static int netsurf_kernel_stb_floor(float value) {
    int i = (int)value;
    return (value < (float)i) ? i - 1 : i;
}

static int netsurf_kernel_stb_ceil(float value) {
    int i = (int)value;
    return (value > (float)i) ? i + 1 : i;
}

static double netsurf_kernel_stb_fabs(double value) {
    return value < 0.0 ? -value : value;
}

static double netsurf_kernel_stb_sqrt(double value) {
    double x;
    int i;

    if (value <= 0.0) {
        return 0.0;
    }
    x = value > 1.0 ? value : 1.0;
    for (i = 0; i < 16; ++i) {
        x = 0.5 * (x + value / x);
    }
    return x;
}

static double netsurf_kernel_stb_fmod(double x, double y) {
    int q;
    if (y == 0.0) {
        return 0.0;
    }
    q = (int)(x / y);
    return x - (double)q * y;
}

static double netsurf_kernel_stb_pow(double x, double y) {
    double guess;
    int i;

    if (y == 0.0) {
        return 1.0;
    }
    if (y > 0.32 && y < 0.34) {
        if (x == 0.0) {
            return 0.0;
        }
        guess = x > 1.0 ? x / 3.0 : 1.0;
        if (guess < 0.0) {
            guess = -guess;
        }
        for (i = 0; i < 12; ++i) {
            guess = (2.0 * guess + x / (guess * guess)) / 3.0;
        }
        return guess;
    }
    return x;
}

static double netsurf_kernel_stb_cos(double x) {
    const double pi = 3.14159265358979323846;
    double x2;

    while (x > pi) {
        x -= 2.0 * pi;
    }
    while (x < -pi) {
        x += 2.0 * pi;
    }
    x2 = x * x;
    return 1.0 - x2 / 2.0 + (x2 * x2) / 24.0 - (x2 * x2 * x2) / 720.0;
}

static double netsurf_kernel_stb_acos(double x) {
    const double half_pi = 1.57079632679489661923;
    if (x <= -1.0) {
        return 2.0 * half_pi;
    }
    if (x >= 1.0) {
        return 0.0;
    }
    return half_pi - x;
}

static uint32_t netsurf_kernel_colour_to_rgb(colour value) {
    return ((value & 0x0000ffu) << 16) |
           (value & 0x00ff00u) |
           ((value & 0xff0000u) >> 16);
}

static uint32_t netsurf_kernel_blend_rgb(uint32_t dst, uint32_t src, uint32_t alpha) {
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

static int netsurf_kernel_positive_mod(int value, int divisor) {
    int remainder;
    if (divisor <= 0) {
        return 0;
    }
    remainder = value % divisor;
    return remainder < 0 ? remainder + divisor : remainder;
}

static int netsurf_kernel_point_visible(int x, int y) {
    if (x < 0 || y < 0 || x >= (int)graphics_width() || y >= (int)graphics_height()) {
        return 0;
    }
    if (netsurf_kernel_clip_valid &&
        (x < netsurf_kernel_clip.x0 || y < netsurf_kernel_clip.y0 ||
         x >= netsurf_kernel_clip.x1 || y >= netsurf_kernel_clip.y1)) {
        return 0;
    }
    return 1;
}

static uint32_t netsurf_kernel_lerp_u8(uint32_t a, uint32_t b, uint32_t weight) {
    return (a * (256u - weight) + b * weight + 128u) >> 8;
}

static void netsurf_kernel_sample_bitmap_rgba(const uint8_t *pixels,
                                              int rowstride,
                                              int source_width,
                                              int source_height,
                                              int x_fp,
                                              int y_fp,
                                              uint32_t *out_r,
                                              uint32_t *out_g,
                                              uint32_t *out_b,
                                              uint32_t *out_a) {
    int x0;
    int y0;
    int x1;
    int y1;
    uint32_t wx;
    uint32_t wy;
    const uint8_t *p00;
    const uint8_t *p10;
    const uint8_t *p01;
    const uint8_t *p11;
    uint32_t r0;
    uint32_t r1;
    uint32_t g0;
    uint32_t g1;
    uint32_t b0;
    uint32_t b1;
    uint32_t a0;
    uint32_t a1;

    if (x_fp < 0) {
        x_fp = 0;
    }
    if (y_fp < 0) {
        y_fp = 0;
    }
    x0 = x_fp >> 8;
    y0 = y_fp >> 8;
    if (x0 >= source_width) {
        x0 = source_width - 1;
        x_fp = x0 << 8;
    }
    if (y0 >= source_height) {
        y0 = source_height - 1;
        y_fp = y0 << 8;
    }
    x1 = x0 + 1;
    y1 = y0 + 1;
    if (x1 >= source_width) {
        x1 = x0;
    }
    if (y1 >= source_height) {
        y1 = y0;
    }
    wx = (uint32_t)(x_fp & 0xff);
    wy = (uint32_t)(y_fp & 0xff);

    p00 = pixels + (size_t)y0 * (size_t)rowstride + (size_t)x0 * 4u;
    p10 = pixels + (size_t)y0 * (size_t)rowstride + (size_t)x1 * 4u;
    p01 = pixels + (size_t)y1 * (size_t)rowstride + (size_t)x0 * 4u;
    p11 = pixels + (size_t)y1 * (size_t)rowstride + (size_t)x1 * 4u;

    r0 = netsurf_kernel_lerp_u8((uint32_t)p00[0], (uint32_t)p10[0], wx);
    r1 = netsurf_kernel_lerp_u8((uint32_t)p01[0], (uint32_t)p11[0], wx);
    g0 = netsurf_kernel_lerp_u8((uint32_t)p00[1], (uint32_t)p10[1], wx);
    g1 = netsurf_kernel_lerp_u8((uint32_t)p01[1], (uint32_t)p11[1], wx);
    b0 = netsurf_kernel_lerp_u8((uint32_t)p00[2], (uint32_t)p10[2], wx);
    b1 = netsurf_kernel_lerp_u8((uint32_t)p01[2], (uint32_t)p11[2], wx);
    a0 = netsurf_kernel_lerp_u8((uint32_t)p00[3], (uint32_t)p10[3], wx);
    a1 = netsurf_kernel_lerp_u8((uint32_t)p01[3], (uint32_t)p11[3], wx);

    *out_r = netsurf_kernel_lerp_u8(r0, r1, wy);
    *out_g = netsurf_kernel_lerp_u8(g0, g1, wy);
    *out_b = netsurf_kernel_lerp_u8(b0, b1, wy);
    *out_a = netsurf_kernel_lerp_u8(a0, a1, wy);
}

static int netsurf_kernel_clip_rect(int *x0, int *y0, int *x1, int *y1) {
    int width = (int)graphics_width();
    int height = (int)graphics_height();

    if (*x1 <= *x0 || *y1 <= *y0 || width <= 0 || height <= 0) {
        return 0;
    }
    if (netsurf_kernel_clip_valid) {
        if (*x0 < netsurf_kernel_clip.x0) {
            *x0 = netsurf_kernel_clip.x0;
        }
        if (*y0 < netsurf_kernel_clip.y0) {
            *y0 = netsurf_kernel_clip.y0;
        }
        if (*x1 > netsurf_kernel_clip.x1) {
            *x1 = netsurf_kernel_clip.x1;
        }
        if (*y1 > netsurf_kernel_clip.y1) {
            *y1 = netsurf_kernel_clip.y1;
        }
    }
    if (*x0 < 0) {
        *x0 = 0;
    }
    if (*y0 < 0) {
        *y0 = 0;
    }
    if (*x1 > width) {
        *x1 = width;
    }
    if (*y1 > height) {
        *y1 = height;
    }
    return *x1 > *x0 && *y1 > *y0;
}

static int netsurf_kernel_style_width(const plot_style_t *pstyle) {
    int width = 1;
    if (pstyle != 0 && pstyle->stroke_width > 0) {
        width = plot_style_fixed_to_int(pstyle->stroke_width);
    }
    if (width < 1) {
        width = 1;
    } else if (width > 16) {
        width = 16;
    }
    return width;
}

static void netsurf_kernel_draw_pixel(int x, int y, uint32_t rgb) {
    if (netsurf_kernel_point_visible(x, y)) {
        graphics_put_pixel((uint32_t)x, (uint32_t)y, rgb);
    }
}

static void netsurf_kernel_draw_line_raw(int x0, int y0, int x1, int y1, uint32_t rgb) {
    int dx = netsurf_kernel_iabs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -netsurf_kernel_iabs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        int e2;
        netsurf_kernel_draw_pixel(x0, y0, rgb);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void netsurf_kernel_draw_line_width(int x0, int y0, int x1, int y1, int width, uint32_t rgb) {
    int offset;
    int half = width / 2;

    if (width <= 1) {
        netsurf_kernel_draw_line_raw(x0, y0, x1, y1, rgb);
        return;
    }

    if (netsurf_kernel_iabs(x1 - x0) >= netsurf_kernel_iabs(y1 - y0)) {
        for (offset = -half; offset <= half; ++offset) {
            netsurf_kernel_draw_line_raw(x0, y0 + offset, x1, y1 + offset, rgb);
        }
    } else {
        for (offset = -half; offset <= half; ++offset) {
            netsurf_kernel_draw_line_raw(x0 + offset, y0, x1 + offset, y1, rgb);
        }
    }
}

static void netsurf_kernel_fill_rect(int x0, int y0, int x1, int y1, uint32_t rgb) {
    if (netsurf_kernel_clip_rect(&x0, &y0, &x1, &y1)) {
        graphics_fill_rect((uint32_t)x0,
                           (uint32_t)y0,
                           (uint32_t)(x1 - x0),
                           (uint32_t)(y1 - y0),
                           rgb);
    }
}

static void netsurf_kernel_sort_ints(int *values, unsigned int count) {
    unsigned int i;

    for (i = 1u; i < count; ++i) {
        int value = values[i];
        unsigned int j = i;

        while (j > 0u && values[j - 1u] > value) {
            values[j] = values[j - 1u];
            --j;
        }
        values[j] = value;
    }
}

static void netsurf_kernel_fill_polygon_points(const int *p, unsigned int n, uint32_t rgb) {
    int min_y;
    int max_y;
    int y;
    unsigned int i;

    if (p == 0 || n < 3u || n > NETSURF_KERNEL_POLYGON_POINT_LIMIT) {
        return;
    }

    min_y = p[1];
    max_y = p[1];
    for (i = 1u; i < n; ++i) {
        int py = p[i * 2u + 1u];
        if (py < min_y) {
            min_y = py;
        }
        if (py > max_y) {
            max_y = py;
        }
    }

    if (netsurf_kernel_clip_valid) {
        if (min_y < netsurf_kernel_clip.y0) {
            min_y = netsurf_kernel_clip.y0;
        }
        if (max_y >= netsurf_kernel_clip.y1) {
            max_y = netsurf_kernel_clip.y1 - 1;
        }
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_y >= (int)graphics_height()) {
        max_y = (int)graphics_height() - 1;
    }

    for (y = min_y; y <= max_y; ++y) {
        int intersections[NETSURF_KERNEL_POLYGON_POINT_LIMIT];
        unsigned int count = 0u;

        for (i = 0u; i < n; ++i) {
            unsigned int next = (i + 1u) % n;
            int x0 = p[i * 2u];
            int y0 = p[i * 2u + 1u];
            int x1 = p[next * 2u];
            int y1 = p[next * 2u + 1u];

            if (((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) &&
                count < NETSURF_KERNEL_POLYGON_POINT_LIMIT) {
                intersections[count++] =
                    x0 + (int)(((int64_t)(y - y0) * (int64_t)(x1 - x0)) / (int64_t)(y1 - y0));
            }
        }

        netsurf_kernel_sort_ints(intersections, count);
        for (i = 0u; i + 1u < count; i += 2u) {
            int x0 = intersections[i];
            int x1 = intersections[i + 1u];
            if (x1 > x0) {
                netsurf_kernel_fill_rect(x0, y, x1, y + 1, rgb);
            }
        }
    }
}

static void netsurf_kernel_draw_rect_outline(int x0, int y0, int x1, int y1, int width, uint32_t rgb) {
    int i;

    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    for (i = 0; i < width; ++i) {
        netsurf_kernel_draw_line_width(x0, y0 + i, x1 - 1, y0 + i, 1, rgb);
        netsurf_kernel_draw_line_width(x0, y1 - 1 - i, x1 - 1, y1 - 1 - i, 1, rgb);
        netsurf_kernel_draw_line_width(x0 + i, y0, x0 + i, y1 - 1, 1, rgb);
        netsurf_kernel_draw_line_width(x1 - 1 - i, y0, x1 - 1 - i, y1 - 1, 1, rgb);
    }
}

static int netsurf_kernel_utf8_is_trail(unsigned char ch) {
    return (ch & 0xc0u) == 0x80u;
}

static size_t netsurf_kernel_utf8_next(const char *string, size_t length, size_t offset) {
    size_t next = offset + 1u;
    if (string == 0 || offset >= length) {
        return length;
    }
    while (next < length && netsurf_kernel_utf8_is_trail((unsigned char)string[next])) {
        ++next;
    }
    return next;
}

static size_t netsurf_kernel_utf8_count(const char *string, size_t length) {
    size_t count = 0;
    size_t offset = 0;
    while (offset < length) {
        offset = netsurf_kernel_utf8_next(string, length, offset);
        ++count;
    }
    return count;
}

static uint32_t netsurf_kernel_utf8_codepoint(const char *string, size_t length, size_t offset) {
    const unsigned char *s = (const unsigned char *)string;
    unsigned char ch;

    if (s == 0 || offset >= length) {
        return 0xfffdu;
    }
    ch = s[offset];
    if (ch < 0x80u) {
        return ch;
    }
    if ((ch & 0xe0u) == 0xc0u && offset + 1u < length &&
        netsurf_kernel_utf8_is_trail(s[offset + 1u])) {
        return ((uint32_t)(ch & 0x1fu) << 6) |
               (uint32_t)(s[offset + 1u] & 0x3fu);
    }
    if ((ch & 0xf0u) == 0xe0u && offset + 2u < length &&
        netsurf_kernel_utf8_is_trail(s[offset + 1u]) &&
        netsurf_kernel_utf8_is_trail(s[offset + 2u])) {
        return ((uint32_t)(ch & 0x0fu) << 12) |
               ((uint32_t)(s[offset + 1u] & 0x3fu) << 6) |
               (uint32_t)(s[offset + 2u] & 0x3fu);
    }
    if ((ch & 0xf8u) == 0xf0u && offset + 3u < length &&
        netsurf_kernel_utf8_is_trail(s[offset + 1u]) &&
        netsurf_kernel_utf8_is_trail(s[offset + 2u]) &&
        netsurf_kernel_utf8_is_trail(s[offset + 3u])) {
        return ((uint32_t)(ch & 0x07u) << 18) |
               ((uint32_t)(s[offset + 1u] & 0x3fu) << 12) |
               ((uint32_t)(s[offset + 2u] & 0x3fu) << 6) |
               (uint32_t)(s[offset + 3u] & 0x3fu);
    }
    return 0xfffdu;
}

static bool netsurf_kernel_ttf_ready(void) {
    int offset;

    if (netsurf_kernel_font_tried) {
        return netsurf_kernel_font_ready;
    }
    netsurf_kernel_font_tried = true;
    offset = stbtt_GetFontOffsetForIndex(dejavu_sans_ttf, 0);
    if (offset < 0) {
        return false;
    }
    netsurf_kernel_font_ready =
        stbtt_InitFont(&netsurf_kernel_font_info, dejavu_sans_ttf, offset) != 0;
    return netsurf_kernel_font_ready;
}

static float netsurf_kernel_ttf_scale(int px) {
    if (px < 8) {
        px = 8;
    } else if (px > 96) {
        px = 96;
    }
    return stbtt_ScaleForPixelHeight(&netsurf_kernel_font_info, (float)px);
}

static int netsurf_kernel_ttf_char_width(const struct plot_font_style *fstyle, uint32_t codepoint) {
    int advance = 0;
    int lsb = 0;
    int width;

    if (!netsurf_kernel_ttf_ready()) {
        return 0;
    }
    if (codepoint == '\t') {
        codepoint = ' ';
    }
    stbtt_GetCodepointHMetrics(&netsurf_kernel_font_info, (int)codepoint, &advance, &lsb);
    width = netsurf_kernel_float_to_int((float)advance *
                                        netsurf_kernel_ttf_scale(netsurf_kernel_font_px(fstyle)));
    if (fstyle != 0 && fstyle->weight >= 700) {
        ++width;
    }
    if (width < 1 && codepoint != 0u) {
        width = netsurf_kernel_font_px(fstyle) / 2;
    }
    return width;
}

static netsurf_kernel_font_cache_entry_t *netsurf_kernel_ttf_glyph(const struct plot_font_style *fstyle,
                                                                   uint32_t codepoint) {
    netsurf_kernel_font_cache_entry_t *entry = 0;
    uint32_t oldest_age = 0xffffffffu;
    uint32_t oldest_index = 0u;
    uint32_t i;
    int px = netsurf_kernel_font_px(fstyle);
    float scale;
    int advance = 0;
    int lsb = 0;

    if (!netsurf_kernel_ttf_ready()) {
        return 0;
    }
    if (codepoint == '\t') {
        codepoint = ' ';
    }
    ++netsurf_kernel_font_cache_age;
    if (netsurf_kernel_font_cache_age == 0u) {
        netsurf_kernel_font_cache_age = 1u;
    }
    for (i = 0; i < NETSURF_KERNEL_FONT_CACHE_SLOTS; ++i) {
        entry = &netsurf_kernel_font_cache[i];
        if (entry->alpha != 0 && entry->codepoint == codepoint && entry->px == px) {
            entry->age = netsurf_kernel_font_cache_age;
            return entry;
        }
        if (entry->alpha == 0) {
            oldest_index = i;
            oldest_age = 0u;
            break;
        }
        if (entry->age < oldest_age) {
            oldest_age = entry->age;
            oldest_index = i;
        }
    }

    entry = &netsurf_kernel_font_cache[oldest_index];
    if (entry->alpha != 0) {
        kfree(entry->alpha);
    }
    memset(entry, 0, sizeof(*entry));
    scale = netsurf_kernel_ttf_scale(px);
    stbtt_GetCodepointHMetrics(&netsurf_kernel_font_info, (int)codepoint, &advance, &lsb);
    entry->alpha = stbtt_GetCodepointBitmap(&netsurf_kernel_font_info,
                                            0.0f,
                                            scale,
                                            (int)codepoint,
                                            &entry->width,
                                            &entry->height,
                                            &entry->xoff,
                                            &entry->yoff);
    entry->codepoint = codepoint;
    entry->px = px;
    entry->advance = netsurf_kernel_float_to_int((float)advance * scale);
    entry->lsb = netsurf_kernel_float_to_int((float)lsb * scale);
    entry->age = netsurf_kernel_font_cache_age;
    if (entry->advance < 1) {
        entry->advance = netsurf_kernel_glyph_width(fstyle);
    }
    if (entry->alpha == 0) {
        memset(entry, 0, sizeof(*entry));
        return 0;
    }
    return entry;
}

static int netsurf_kernel_plot_ttf_glyph(const struct plot_font_style *fstyle,
                                         uint32_t codepoint,
                                         int x,
                                         int baseline_y,
                                         uint32_t fg,
                                         int bold,
                                         int italic) {
    netsurf_kernel_font_cache_entry_t *glyph = netsurf_kernel_ttf_glyph(fstyle, codepoint);
    int row;
    int col;
    int extra_passes = bold != 0 ? 2 : 1;
    int pass;

    if (glyph == 0 || glyph->alpha == 0) {
        return 0;
    }
    for (pass = 0; pass < extra_passes; ++pass) {
        for (row = 0; row < glyph->height; ++row) {
            int shear = italic != 0 ? (glyph->height - row) / 5 : 0;
            int py = baseline_y + glyph->yoff + row;

            if (py < 0 || py >= (int)graphics_height()) {
                continue;
            }
            if (netsurf_kernel_clip_valid &&
                (py < netsurf_kernel_clip.y0 || py >= netsurf_kernel_clip.y1)) {
                continue;
            }
            for (col = 0; col < glyph->width; ++col) {
                uint32_t alpha = glyph->alpha[row * glyph->width + col];
                int px;
                uint32_t dst;
                uint32_t color;

                if (alpha == 0u) {
                    continue;
                }
                px = x + glyph->xoff + col + shear + pass;
                if (px < 0 || px >= (int)graphics_width()) {
                    continue;
                }
                if (netsurf_kernel_clip_valid &&
                    (px < netsurf_kernel_clip.x0 || px >= netsurf_kernel_clip.x1)) {
                    continue;
                }
                dst = graphics_get_pixel((uint32_t)px, (uint32_t)py);
                color = netsurf_kernel_blend_rgb(dst, fg, alpha);
                graphics_put_pixel((uint32_t)px, (uint32_t)py, color);
            }
        }
    }
    return glyph->advance + (bold != 0 ? 1 : 0);
}

static int netsurf_kernel_font_px(const struct plot_font_style *fstyle) {
    int px = 10;
    if (fstyle != 0 && fstyle->size > 0) {
        px = (int)(fstyle->size / PLOT_STYLE_SCALE);
    }
    if (px < 8) {
        px = 8;
    } else if (px > 48) {
        px = 48;
    }
    return px;
}

static int netsurf_kernel_glyph_width(const struct plot_font_style *fstyle) {
    int px = netsurf_kernel_font_px(fstyle);
    int width = (px * 56 + 50) / 100;
    if (fstyle != 0 && fstyle->family == PLOT_FONT_FAMILY_MONOSPACE) {
        width = (px * 64 + 50) / 100;
    } else if (fstyle != 0 && fstyle->family == PLOT_FONT_FAMILY_SERIF) {
        width = (px * 58 + 50) / 100;
    }
    if (fstyle != 0 && fstyle->weight >= 700) {
        ++width;
    }
    if (width < 4) {
        width = 4;
    }
    return width;
}

static int netsurf_kernel_font_height(const struct plot_font_style *fstyle) {
    int px = netsurf_kernel_font_px(fstyle);
    if (px < 8) {
        px = 8;
    } else if (px > 64) {
        px = 64;
    }
    return px;
}

static int netsurf_kernel_char_width(const struct plot_font_style *fstyle,
                                     const char *string,
                                     size_t offset,
                                     size_t next) {
    int width = netsurf_kernel_glyph_width(fstyle);
    uint32_t ch;

    if (string == 0 || offset >= next) {
        return width;
    }
    ch = netsurf_kernel_utf8_codepoint(string, next, offset);
    width = netsurf_kernel_ttf_char_width(fstyle, ch);
    if (width > 0) {
        return width;
    }
    width = netsurf_kernel_glyph_width(fstyle);
    if (ch == ' ' || ch == '\t') {
        width = (width * 55 + 50) / 100;
    } else if (ch == 'i' || ch == 'l' || ch == 'I' || ch == '!' ||
               ch == '|' || ch == '.' || ch == ',' || ch == ':' ||
               ch == ';' || ch == '\'' || ch == '`') {
        width = (width * 55 + 50) / 100;
    } else if (ch == 'm' || ch == 'w' || ch == 'M' || ch == 'W' ||
               ch == '@' || ch == '%') {
        width = (width * 130 + 50) / 100;
    } else if (ch >= 'A' && ch <= 'Z') {
        width = (width * 108 + 50) / 100;
    }
    if (width < 3) {
        width = 3;
    }
    return width;
}

static int netsurf_kernel_is_space(unsigned char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static nserror netsurf_kernel_layout_width(const struct plot_font_style *fstyle,
                                           const char *string,
                                           size_t length,
                                           int *width) {
    size_t offset = 0;
    int total = 0;

    if (width == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    while (offset < length) {
        size_t next = netsurf_kernel_utf8_next(string, length, offset);
        total += netsurf_kernel_char_width(fstyle, string, offset, next);
        offset = next;
    }
    *width = total;
    return NSERROR_OK;
}

static nserror netsurf_kernel_layout_position(const struct plot_font_style *fstyle,
                                              const char *string,
                                              size_t length,
                                              int x,
                                              size_t *char_offset,
                                              int *actual_x) {
    int glyph_width;
    int current_x = 0;
    size_t offset = 0;

    if (char_offset == 0 || actual_x == 0) {
        return NSERROR_BAD_PARAMETER;
    }

    glyph_width = netsurf_kernel_glyph_width(fstyle);
    if (x <= 0 || string == 0 || length == 0u) {
        *char_offset = 0;
        *actual_x = 0;
        return NSERROR_OK;
    }

    while (offset < length) {
        size_t next = netsurf_kernel_utf8_next(string, length, offset);
        glyph_width = netsurf_kernel_char_width(fstyle, string, offset, next);
        if (x < current_x + (glyph_width / 2)) {
            break;
        }
        offset = next;
        current_x += glyph_width;
    }

    *char_offset = offset;
    *actual_x = current_x;
    return NSERROR_OK;
}

static nserror netsurf_kernel_layout_split(const struct plot_font_style *fstyle,
                                           const char *string,
                                           size_t length,
                                           int x,
                                           size_t *char_offset,
                                           int *actual_x) {
    int glyph_width;
    int current_x = 0;
    size_t offset = 0;
    size_t best_offset = 0;
    int best_x = 0;
    size_t last_space_offset = 0;
    int last_space_x = 0;

    if (char_offset == 0 || actual_x == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    if (string == 0 || length == 0u) {
        *char_offset = 1;
        *actual_x = 0;
        return NSERROR_BAD_PARAMETER;
    }

    while (offset < length) {
        size_t next = netsurf_kernel_utf8_next(string, length, offset);
        glyph_width = netsurf_kernel_char_width(fstyle, string, offset, next);
        int next_x = current_x + glyph_width;
        if (netsurf_kernel_is_space((unsigned char)string[offset])) {
            last_space_offset = next;
            last_space_x = next_x;
        }
        if (next_x > x && best_offset != 0u) {
            break;
        }
        best_offset = next;
        best_x = next_x;
        offset = next;
        current_x = next_x;
    }

    if (last_space_offset != 0u && last_space_offset <= best_offset) {
        best_offset = last_space_offset;
        best_x = last_space_x;
    }
    if (best_offset == 0u) {
        glyph_width = netsurf_kernel_char_width(fstyle, string, 0u,
                                                netsurf_kernel_utf8_next(string, length, 0u));
        best_offset = netsurf_kernel_utf8_next(string, length, 0u);
        best_x = glyph_width;
    }

    *char_offset = best_offset;
    *actual_x = best_x;
    return NSERROR_OK;
}

static netsurf_kernel_plot_stats_t *netsurf_kernel_stats_for_ctx(const struct redraw_context *ctx) {
    if (ctx != 0 && ctx->priv != 0) {
        return (netsurf_kernel_plot_stats_t *)ctx->priv;
    }
    return &netsurf_kernel_global_stats;
}

static nserror netsurf_kernel_plot_clip(const struct redraw_context *ctx, const struct rect *clip) {
    netsurf_kernel_plot_stats_t *stats = netsurf_kernel_stats_for_ctx(ctx);
    ++stats->clips;
    if (clip != 0) {
        stats->clip_x0 = clip->x0;
        stats->clip_y0 = clip->y0;
        stats->clip_x1 = clip->x1;
        stats->clip_y1 = clip->y1;
        netsurf_kernel_clip = *clip;
        netsurf_kernel_clip_valid = true;
    } else {
        netsurf_kernel_clip_valid = false;
    }
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_arc(const struct redraw_context *ctx,
                                       const plot_style_t *pstyle,
                                       int x,
                                       int y,
                                       int radius,
                                       int angle1,
                                       int angle2) {
    (void)pstyle;
    (void)x;
    (void)y;
    (void)radius;
    (void)angle1;
    (void)angle2;
    ++netsurf_kernel_stats_for_ctx(ctx)->arcs;
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_disc(const struct redraw_context *ctx,
                                        const plot_style_t *pstyle,
                                        int x,
                                        int y,
                                        int radius) {
    int yy;
    int xx;
    ++netsurf_kernel_stats_for_ctx(ctx)->discs;
    if (!netsurf_kernel_plot_output_enabled || pstyle == 0 || radius <= 0) {
        return NSERROR_OK;
    }
    if (pstyle->fill_type != PLOT_OP_TYPE_NONE && pstyle->fill_colour != NS_TRANSPARENT) {
        uint32_t fill = netsurf_kernel_colour_to_rgb(pstyle->fill_colour);
        int radius_sq = radius * radius;
        for (yy = -radius; yy <= radius; ++yy) {
            for (xx = -radius; xx <= radius; ++xx) {
                if (xx * xx + yy * yy <= radius_sq) {
                    netsurf_kernel_draw_pixel(x + xx, y + yy, fill);
                }
            }
        }
    }
    if (pstyle->stroke_type != PLOT_OP_TYPE_NONE && pstyle->stroke_colour != NS_TRANSPARENT) {
        uint32_t stroke = netsurf_kernel_colour_to_rgb(pstyle->stroke_colour);
        int last_x = x + radius;
        int last_y = y;
        int step;
        for (step = 1; step <= 32; ++step) {
            int px = x + (radius * netsurf_kernel_iabs(16 - (step % 32))) / 16 - radius;
            int py = y + (step <= 16 ? step : 32 - step) * radius / 16;
            if (step > 16) {
                py = y - (32 - step) * radius / 16;
            }
            netsurf_kernel_draw_line_width(last_x, last_y, px, py, netsurf_kernel_style_width(pstyle), stroke);
            last_x = px;
            last_y = py;
        }
    }
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_line(const struct redraw_context *ctx,
                                        const plot_style_t *pstyle,
                                        const struct rect *line) {
    ++netsurf_kernel_stats_for_ctx(ctx)->lines;
    if (netsurf_kernel_plot_output_enabled &&
        pstyle != 0 &&
        line != 0 &&
        pstyle->stroke_type != PLOT_OP_TYPE_NONE &&
        pstyle->stroke_colour != NS_TRANSPARENT) {
        netsurf_kernel_draw_line_width(line->x0,
                                       line->y0,
                                       line->x1,
                                       line->y1,
                                       netsurf_kernel_style_width(pstyle),
                                       netsurf_kernel_colour_to_rgb(pstyle->stroke_colour));
    }
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_rectangle(const struct redraw_context *ctx,
                                             const plot_style_t *pstyle,
                                             const struct rect *rectangle) {
    ++netsurf_kernel_stats_for_ctx(ctx)->rectangles;
    if (!netsurf_kernel_plot_output_enabled || pstyle == 0 || rectangle == 0) {
        return NSERROR_OK;
    }
    if (pstyle->fill_type != PLOT_OP_TYPE_NONE && pstyle->fill_colour != NS_TRANSPARENT) {
        netsurf_kernel_fill_rect(rectangle->x0,
                                 rectangle->y0,
                                 rectangle->x1,
                                 rectangle->y1,
                                 netsurf_kernel_colour_to_rgb(pstyle->fill_colour));
    }
    if (pstyle->stroke_type != PLOT_OP_TYPE_NONE && pstyle->stroke_colour != NS_TRANSPARENT) {
        netsurf_kernel_draw_rect_outline(rectangle->x0,
                                         rectangle->y0,
                                         rectangle->x1,
                                         rectangle->y1,
                                         netsurf_kernel_style_width(pstyle),
                                         netsurf_kernel_colour_to_rgb(pstyle->stroke_colour));
    }
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_polygon(const struct redraw_context *ctx,
                                           const plot_style_t *pstyle,
                                           const int *p,
                                           unsigned int n) {
    unsigned int i;
    ++netsurf_kernel_stats_for_ctx(ctx)->polygons;
    if (!netsurf_kernel_plot_output_enabled || pstyle == 0 || p == 0 || n < 2u) {
        return NSERROR_OK;
    }
    if (pstyle->fill_type != PLOT_OP_TYPE_NONE && pstyle->fill_colour != NS_TRANSPARENT) {
        netsurf_kernel_fill_polygon_points(p, n, netsurf_kernel_colour_to_rgb(pstyle->fill_colour));
    }
    if (pstyle->stroke_type != PLOT_OP_TYPE_NONE && pstyle->stroke_colour != NS_TRANSPARENT) {
        uint32_t rgb = netsurf_kernel_colour_to_rgb(pstyle->stroke_colour);
        int width = netsurf_kernel_style_width(pstyle);
        for (i = 0; i < n; ++i) {
            unsigned int next = (i + 1u) % n;
            netsurf_kernel_draw_line_width(p[i * 2u],
                                           p[i * 2u + 1u],
                                           p[next * 2u],
                                           p[next * 2u + 1u],
                                           width,
                                           rgb);
        }
    }
    return NSERROR_OK;
}

static void netsurf_kernel_path_transform(const float transform[6],
                                          float x,
                                          float y,
                                          int *out_x,
                                          int *out_y) {
    float tx;
    float ty;

    if (transform != 0) {
        tx = x * transform[0] + y * transform[2] + transform[4];
        ty = x * transform[1] + y * transform[3] + transform[5];
    } else {
        tx = x;
        ty = y;
    }
    *out_x = netsurf_kernel_float_to_int(tx);
    *out_y = netsurf_kernel_float_to_int(ty);
}

static unsigned int netsurf_kernel_path_append_point(int *points,
                                                     unsigned int point_count,
                                                     int x,
                                                     int y) {
    if (point_count >= NETSURF_KERNEL_POLYGON_POINT_LIMIT) {
        return point_count;
    }
    if (point_count > 0u &&
        points[(point_count - 1u) * 2u] == x &&
        points[(point_count - 1u) * 2u + 1u] == y) {
        return point_count;
    }
    points[point_count * 2u] = x;
    points[point_count * 2u + 1u] = y;
    return point_count + 1u;
}

static nserror netsurf_kernel_plot_path(const struct redraw_context *ctx,
                                        const plot_style_t *pstyle,
                                        const float *p,
                                        unsigned int n,
                                        const float transform[6]) {
    int points[NETSURF_KERNEL_POLYGON_POINT_LIMIT * 2u];
    unsigned int point_count = 0u;
    unsigned int moves = 0u;
    unsigned int i = 0u;
    bool valid = true;
    float current_x = 0.0f;
    float current_y = 0.0f;

    ++netsurf_kernel_stats_for_ctx(ctx)->paths;
    if (!netsurf_kernel_plot_output_enabled || pstyle == 0 || p == 0 || n == 0u) {
        return NSERROR_OK;
    }
    if (p[0] != PLOTTER_PATH_MOVE) {
        return NSERROR_OK;
    }

    while (i < n && valid) {
        if (p[i] == PLOTTER_PATH_MOVE) {
            int x;
            int y;
            if (i + 2u >= n) {
                valid = false;
                break;
            }
            netsurf_kernel_path_transform(transform, p[i + 1u], p[i + 2u], &x, &y);
            current_x = p[i + 1u];
            current_y = p[i + 2u];
            ++moves;
            if (moves == 1u) {
                point_count = netsurf_kernel_path_append_point(points, point_count, x, y);
            }
            i += 3u;
        } else if (p[i] == PLOTTER_PATH_CLOSE) {
            i += 1u;
        } else if (p[i] == PLOTTER_PATH_LINE) {
            int x;
            int y;
            if (i + 2u >= n) {
                valid = false;
                break;
            }
            netsurf_kernel_path_transform(transform, p[i + 1u], p[i + 2u], &x, &y);
            current_x = p[i + 1u];
            current_y = p[i + 2u];
            if (moves == 1u) {
                point_count = netsurf_kernel_path_append_point(points, point_count, x, y);
            }
            i += 3u;
        } else if (p[i] == PLOTTER_PATH_BEZIER) {
            unsigned int step;
            float x0 = current_x;
            float y0 = current_y;

            if (i + 6u >= n) {
                valid = false;
                break;
            }
            for (step = 1u; step <= NETSURF_KERNEL_PATH_BEZIER_STEPS; ++step) {
                float t = (float)step / (float)NETSURF_KERNEL_PATH_BEZIER_STEPS;
                float mt = 1.0f - t;
                float bx = mt * mt * mt * x0 +
                    3.0f * mt * mt * t * p[i + 1u] +
                    3.0f * mt * t * t * p[i + 3u] +
                    t * t * t * p[i + 5u];
                float by = mt * mt * mt * y0 +
                    3.0f * mt * mt * t * p[i + 2u] +
                    3.0f * mt * t * t * p[i + 4u] +
                    t * t * t * p[i + 6u];
                int x;
                int y;
                netsurf_kernel_path_transform(transform, bx, by, &x, &y);
                if (moves == 1u) {
                    point_count = netsurf_kernel_path_append_point(points, point_count, x, y);
                }
            }
            current_x = p[i + 5u];
            current_y = p[i + 6u];
            i += 7u;
        } else {
            valid = false;
        }
    }

    if (!valid) {
        return NSERROR_OK;
    }

    (void)point_count;
    (void)moves;

    if (pstyle->stroke_type != PLOT_OP_TYPE_NONE && pstyle->stroke_colour != NS_TRANSPARENT) {
        uint32_t stroke = netsurf_kernel_colour_to_rgb(pstyle->stroke_colour);
        int width = netsurf_kernel_style_width(pstyle);
        int last_x = 0;
        int last_y = 0;
        int start_x = 0;
        int start_y = 0;
        float source_x = 0.0f;
        float source_y = 0.0f;
        bool have_point = false;

        i = 0u;
        while (i < n) {
            if (p[i] == PLOTTER_PATH_MOVE) {
                netsurf_kernel_path_transform(transform, p[i + 1u], p[i + 2u], &last_x, &last_y);
                start_x = last_x;
                start_y = last_y;
                source_x = p[i + 1u];
                source_y = p[i + 2u];
                have_point = true;
                i += 3u;
            } else if (p[i] == PLOTTER_PATH_CLOSE) {
                if (have_point) {
                    netsurf_kernel_draw_line_width(last_x, last_y, start_x, start_y, width, stroke);
                    last_x = start_x;
                    last_y = start_y;
                }
                i += 1u;
            } else if (p[i] == PLOTTER_PATH_LINE) {
                int x;
                int y;
                netsurf_kernel_path_transform(transform, p[i + 1u], p[i + 2u], &x, &y);
                if (have_point) {
                    netsurf_kernel_draw_line_width(last_x, last_y, x, y, width, stroke);
                }
                last_x = x;
                last_y = y;
                source_x = p[i + 1u];
                source_y = p[i + 2u];
                have_point = true;
                i += 3u;
            } else if (p[i] == PLOTTER_PATH_BEZIER) {
                unsigned int step;
                float x0 = source_x;
                float y0 = source_y;
                for (step = 1u; step <= NETSURF_KERNEL_PATH_BEZIER_STEPS; ++step) {
                    float t = (float)step / (float)NETSURF_KERNEL_PATH_BEZIER_STEPS;
                    float mt = 1.0f - t;
                    float bx = mt * mt * mt * x0 +
                        3.0f * mt * mt * t * p[i + 1u] +
                        3.0f * mt * t * t * p[i + 3u] +
                        t * t * t * p[i + 5u];
                    float by = mt * mt * mt * y0 +
                        3.0f * mt * mt * t * p[i + 2u] +
                        3.0f * mt * t * t * p[i + 4u] +
                        t * t * t * p[i + 6u];
                    int x;
                    int y;
                    netsurf_kernel_path_transform(transform, bx, by, &x, &y);
                    if (have_point) {
                        netsurf_kernel_draw_line_width(last_x, last_y, x, y, width, stroke);
                    }
                    last_x = x;
                    last_y = y;
                    have_point = true;
                }
                source_x = p[i + 5u];
                source_y = p[i + 6u];
                i += 7u;
            } else {
                break;
            }
        }
    }
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_bitmap(const struct redraw_context *ctx,
                                          struct bitmap *bitmap,
                                          int x,
                                          int y,
                                          int width,
                                          int height,
                                          colour bg,
                                          bitmap_flags_t flags) {
    int source_width = netsurf_kernel_bitmap_width(bitmap);
    int source_height = netsurf_kernel_bitmap_height(bitmap);
    int rowstride = netsurf_kernel_bitmap_rowstride(bitmap);
    int x0 = x;
    int y0 = y;
    int x1 = x + width;
    int y1 = y + height;
    int px;
    int py;
    uint8_t *pixels = netsurf_kernel_bitmap_buffer(bitmap);
    bool opaque = netsurf_kernel_bitmap_opaque(bitmap) != 0;
    bool scaled = width != source_width || height != source_height;

    ++netsurf_kernel_stats_for_ctx(ctx)->bitmaps;
    if (!netsurf_kernel_plot_output_enabled ||
        pixels == 0 ||
        source_width <= 0 ||
        source_height <= 0 ||
        rowstride < source_width * 4 ||
        width <= 0 ||
        height <= 0) {
        return NSERROR_OK;
    }

    if ((flags & BITMAPF_REPEAT_X) != 0u) {
        x0 = netsurf_kernel_clip_valid ? netsurf_kernel_clip.x0 : 0;
        x1 = netsurf_kernel_clip_valid ? netsurf_kernel_clip.x1 : (int)graphics_width();
    }
    if ((flags & BITMAPF_REPEAT_Y) != 0u) {
        y0 = netsurf_kernel_clip_valid ? netsurf_kernel_clip.y0 : 0;
        y1 = netsurf_kernel_clip_valid ? netsurf_kernel_clip.y1 : (int)graphics_height();
    }
    if (!netsurf_kernel_clip_rect(&x0, &y0, &x1, &y1)) {
        return NSERROR_OK;
    }
    ++netsurf_kernel_stats_for_ctx(ctx)->visible_bitmaps;

    for (py = y0; py < y1; ++py) {
        int rel_y = py - y;
        int tile_y = (flags & BITMAPF_REPEAT_Y) != 0u ?
            netsurf_kernel_positive_mod(rel_y, height) : rel_y;
        int src_y_fp;
        if (tile_y < 0 || tile_y >= height) {
            continue;
        }
        if (scaled && height > 1 && source_height > 1) {
            src_y_fp = (int)(((int64_t)tile_y * (int64_t)(source_height - 1) * 256) / (height - 1));
        } else {
            src_y_fp = (int)(((int64_t)tile_y * source_height * 256) / height);
        }
        for (px = x0; px < x1; ++px) {
            int rel_x = px - x;
            int tile_x = (flags & BITMAPF_REPEAT_X) != 0u ?
                netsurf_kernel_positive_mod(rel_x, width) : rel_x;
            uint32_t color;
            uint32_t alpha;
            uint32_t r;
            uint32_t g;
            uint32_t b;
            uint32_t a;

            if (tile_x < 0 || tile_x >= width) {
                continue;
            }
            if (scaled) {
                int src_x_fp;
                if (width > 1 && source_width > 1) {
                    src_x_fp = (int)(((int64_t)tile_x * (int64_t)(source_width - 1) * 256) / (width - 1));
                } else {
                    src_x_fp = (int)(((int64_t)tile_x * source_width * 256) / width);
                }
                netsurf_kernel_sample_bitmap_rgba(pixels,
                                                  rowstride,
                                                  source_width,
                                                  source_height,
                                                  src_x_fp,
                                                  src_y_fp,
                                                  &r,
                                                  &g,
                                                  &b,
                                                  &a);
            } else {
                const uint8_t *src = pixels +
                    (size_t)tile_y * (size_t)rowstride +
                    (size_t)tile_x * 4u;
                r = (uint32_t)src[0];
                g = (uint32_t)src[1];
                b = (uint32_t)src[2];
                a = (uint32_t)src[3];
            }
            alpha = opaque ? 255u : a;
            color = (r << 16) | (g << 8) | b;
            if (alpha != 255u) {
                uint32_t dst = (bg != NS_TRANSPARENT) ?
                    netsurf_kernel_colour_to_rgb(bg) :
                    graphics_get_pixel((uint32_t)px, (uint32_t)py);
                color = netsurf_kernel_blend_rgb(dst, color, alpha);
            }
            graphics_put_pixel((uint32_t)px, (uint32_t)py, color);
        }
    }
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_text(const struct redraw_context *ctx,
                                        const plot_font_style_t *fstyle,
                                        int x,
                                        int y,
                                        const char *text,
                                        size_t length) {
    int width = 0;
    int draw_width;
    int draw_height;
    int top;
    int font_height;
    int bold;
    int italic;
    netsurf_kernel_plot_stats_t *stats = netsurf_kernel_stats_for_ctx(ctx);

    ++stats->texts;
    stats->text_bytes += (uint32_t)length;
    if (netsurf_kernel_layout_width(fstyle, text, length, &width) == NSERROR_OK && width > 0) {
        stats->text_width += (uint32_t)width;
    }

    font_height = netsurf_kernel_font_height(fstyle);
    draw_width = width > 0 ? width :
                 (int)(netsurf_kernel_utf8_count(text, length) *
                       (size_t)netsurf_kernel_glyph_width(fstyle));
    draw_height = font_height;
    top = y - font_height;
    if (top < 0) {
        top = 0;
    }

    if (text != 0 &&
        length != 0u &&
        (!netsurf_kernel_clip_valid ||
         (x < netsurf_kernel_clip.x1 && top < netsurf_kernel_clip.y1 &&
          x + draw_width > netsurf_kernel_clip.x0 && top + draw_height > netsurf_kernel_clip.y0))) {
        ++stats->visible_texts;
    }

    if (netsurf_kernel_plot_output_enabled &&
        text != 0 &&
        length != 0u) {
        uint32_t fg = 0x000000u;
        size_t offset = 0;
        int px = x;

        if (fstyle != 0 && fstyle->foreground != NS_TRANSPARENT) {
            fg = netsurf_kernel_colour_to_rgb(fstyle->foreground);
        }
        bold = fstyle != 0 && fstyle->weight >= 700;
        italic = fstyle != 0 && (fstyle->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) != 0;
        if (!netsurf_kernel_clip_valid ||
            (x < netsurf_kernel_clip.x1 && top < netsurf_kernel_clip.y1 &&
             x + draw_width > netsurf_kernel_clip.x0 && top + draw_height > netsurf_kernel_clip.y0)) {
            while (offset < length) {
                size_t next = netsurf_kernel_utf8_next(text, length, offset);
                int char_width = netsurf_kernel_char_width(fstyle, text, offset, next);
                uint32_t codepoint = netsurf_kernel_utf8_codepoint(text, length, offset);

                if (codepoint >= 0x20u &&
                    codepoint != 0x7fu &&
                    px + char_width > 0 &&
                    (!netsurf_kernel_clip_valid ||
                     (px < netsurf_kernel_clip.x1 &&
                      px + char_width > netsurf_kernel_clip.x0))) {
                    if (netsurf_kernel_plot_ttf_glyph(fstyle,
                                                      codepoint,
                                                      px,
                                                      y,
                                                      fg,
                                                      bold,
                                                      italic) == 0) {
                        console_draw_codepoint_sized_at_pixel((unsigned int)(px < 0 ? 0 : px),
                                                              (unsigned int)top,
                                                              codepoint,
                                                              fg,
                                                              0xffffffffu,
                                                              (unsigned int)char_width,
                                                              (unsigned int)font_height,
                                                              bold,
                                                              italic);
                    }
                }
                px += char_width;
                offset = next;
            }
        }
    }
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_group_start(const struct redraw_context *ctx, const char *name) {
    (void)name;
    ++netsurf_kernel_stats_for_ctx(ctx)->groups_started;
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_group_end(const struct redraw_context *ctx) {
    ++netsurf_kernel_stats_for_ctx(ctx)->groups_ended;
    return NSERROR_OK;
}

static nserror netsurf_kernel_plot_flush(const struct redraw_context *ctx) {
    ++netsurf_kernel_stats_for_ctx(ctx)->flushes;
    return NSERROR_OK;
}

static struct gui_layout_table netsurf_kernel_layout = {
    .width = netsurf_kernel_layout_width,
    .position = netsurf_kernel_layout_position,
    .split = netsurf_kernel_layout_split,
};

static const struct plotter_table netsurf_kernel_plotters = {
    .clip = netsurf_kernel_plot_clip,
    .arc = netsurf_kernel_plot_arc,
    .disc = netsurf_kernel_plot_disc,
    .line = netsurf_kernel_plot_line,
    .rectangle = netsurf_kernel_plot_rectangle,
    .polygon = netsurf_kernel_plot_polygon,
    .path = netsurf_kernel_plot_path,
    .bitmap = netsurf_kernel_plot_bitmap,
    .text = netsurf_kernel_plot_text,
    .group_start = netsurf_kernel_plot_group_start,
    .group_end = netsurf_kernel_plot_group_end,
    .flush = netsurf_kernel_plot_flush,
    .option_knockout = true,
};

struct gui_layout_table *netsurf_kernel_layout_table(void) {
    return &netsurf_kernel_layout;
}

const struct plotter_table *netsurf_kernel_plotter_table(void) {
    return &netsurf_kernel_plotters;
}

void netsurf_kernel_redraw_context(struct redraw_context *ctx, void *priv) {
    if (ctx == 0) {
        return;
    }
    ctx->interactive = true;
    ctx->background_images = true;
    ctx->plot = &netsurf_kernel_plotters;
    ctx->priv = priv;
}

void netsurf_kernel_plot_stats_reset(void) {
    memset(&netsurf_kernel_global_stats, 0, sizeof(netsurf_kernel_global_stats));
    netsurf_kernel_clip_valid = false;
}

void netsurf_kernel_plot_stats_snapshot(netsurf_kernel_plot_stats_t *out) {
    if (out != 0) {
        *out = netsurf_kernel_global_stats;
    }
}

const char *netsurf_kernel_frontend_status(void) {
    if ((netsurf_kernel_frontend_last_status & NETSURF_KERNEL_FRONTEND_EXPECTED) ==
        NETSURF_KERNEL_FRONTEND_EXPECTED) {
        return "NetSurf kernel frontend ready";
    }
    if ((netsurf_kernel_frontend_last_status & NETSURF_KERNEL_FRONTEND_LAYOUT) == 0u) {
        return "NetSurf kernel frontend missing layout metrics";
    }
    if ((netsurf_kernel_frontend_last_status & NETSURF_KERNEL_FRONTEND_POSITION) == 0u) {
        return "NetSurf kernel frontend missing text position metrics";
    }
    if ((netsurf_kernel_frontend_last_status & NETSURF_KERNEL_FRONTEND_SPLIT) == 0u) {
        return "NetSurf kernel frontend missing text split metrics";
    }
    if ((netsurf_kernel_frontend_last_status & NETSURF_KERNEL_FRONTEND_PLOTTERS) == 0u) {
        return "NetSurf kernel frontend missing plotters";
    }
    if ((netsurf_kernel_frontend_last_status & NETSURF_KERNEL_FRONTEND_CONTEXT) == 0u) {
        return "NetSurf kernel frontend missing redraw context";
    }
    return "NetSurf kernel frontend incomplete";
}

uint32_t netsurf_kernel_frontend_smoke(void) {
    const char sample[] = "hello world";
    const char utf8_sample[] = "z\xce\xbb";
    plot_font_style_t fstyle;
    plot_style_t pstyle;
    struct redraw_context ctx;
    struct rect clip = { 1, 2, 320, 200 };
    netsurf_kernel_plot_stats_t stats;
    int width = 0;
    int utf8_width = 0;
    int prefix_width = 0;
    int actual_x = 0;
    size_t offset = 0;
    uint32_t status = 0;
    bool previous_output_enabled = netsurf_kernel_plot_output_enabled;

    netsurf_kernel_plot_output_enabled = false;
    memset(&fstyle, 0, sizeof(fstyle));
    memset(&pstyle, 0, sizeof(pstyle));
    memset(&stats, 0, sizeof(stats));
    fstyle.family = PLOT_FONT_FAMILY_SANS_SERIF;
    fstyle.size = 10 * PLOT_STYLE_SCALE;

    if (netsurf_kernel_layout.width(&fstyle, sample, sizeof(sample) - 1u, &width) == NSERROR_OK &&
        netsurf_kernel_layout.width(&fstyle, utf8_sample, sizeof(utf8_sample) - 1u, &utf8_width) == NSERROR_OK &&
        width > utf8_width &&
        utf8_width > netsurf_kernel_glyph_width(&fstyle)) {
        status |= NETSURF_KERNEL_FRONTEND_LAYOUT;
    }

    if (netsurf_kernel_layout.width(&fstyle, sample, 5u, &prefix_width) == NSERROR_OK &&
        netsurf_kernel_layout.position(&fstyle, sample, sizeof(sample) - 1u,
                                       prefix_width,
                                       &offset, &actual_x) == NSERROR_OK &&
        offset == 5u && actual_x == prefix_width && actual_x > 0) {
        status |= NETSURF_KERNEL_FRONTEND_POSITION;
    }

    offset = 0;
    actual_x = 0;
    if (netsurf_kernel_layout.split(&fstyle, sample, sizeof(sample) - 1u,
                                    netsurf_kernel_glyph_width(&fstyle) * 7,
                                    &offset, &actual_x) == NSERROR_OK &&
        offset != 0u && offset <= sizeof(sample) - 1u && actual_x > 0) {
        status |= NETSURF_KERNEL_FRONTEND_SPLIT;
    }

    netsurf_kernel_redraw_context(&ctx, &stats);
    if (ctx.interactive && ctx.background_images && ctx.plot == &netsurf_kernel_plotters &&
        ctx.priv == &stats) {
        status |= NETSURF_KERNEL_FRONTEND_CONTEXT;
    }

    if (ctx.plot->clip(&ctx, &clip) == NSERROR_OK &&
        ctx.plot->rectangle(&ctx, &pstyle, &clip) == NSERROR_OK &&
        ctx.plot->line(&ctx, &pstyle, &clip) == NSERROR_OK &&
        ctx.plot->text(&ctx, &fstyle, 3, 4, sample, sizeof(sample) - 1u) == NSERROR_OK &&
        stats.clips == 1u &&
        stats.rectangles == 1u &&
        stats.lines == 1u &&
        stats.texts == 1u &&
        stats.text_width != 0u &&
        stats.visible_texts == 1u &&
        stats.clip_x0 == clip.x0 &&
        stats.clip_y1 == clip.y1) {
        status |= NETSURF_KERNEL_FRONTEND_PLOTTERS;
    }

    netsurf_kernel_frontend_last_status = status;
    netsurf_kernel_plot_output_enabled = previous_output_enabled;
    return status;
}
