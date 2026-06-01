#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <iconv.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include "zlib.h"
#include "utils/errors.h"
#include "content/content_factory.h"
#include "content/content_protected.h"
#include "content/backing_store.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/hlcache.h"
#include "content/llcache.h"
#include "content/textsearch.h"
#include "content/handlers/javascript/js.h"
#include "content/handlers/javascript/content.h"
#include "duktape.h"
#include "desktop/gui_table.h"
#include "desktop/knockout.h"
#include "desktop/save_text.h"
#include "desktop/selection.h"
#include "desktop/textarea.h"
#include "image.h"
#include "netsurf/bitmap.h"
#include "netsurf/browser_window.h"
#include "utils/nsurl.h"
#include "utils/nsoption.h"
#include "netsurf/content.h"
#include "netsurf/fetch.h"
#include "netsurf/layout.h"
#include "netsurf/misc.h"
#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"
#include "dejavu_sans_ttf.h"
#include "netsurf_resource_css.h"

static int zbrowser_lainos_stb_floor(float value);
static int zbrowser_lainos_stb_ceil(float value);
static double zbrowser_lainos_stb_sqrt(double value);
static double zbrowser_lainos_stb_fabs(double value);
static double zbrowser_lainos_stb_fmod(double x, double y);
static double zbrowser_lainos_stb_pow(double x, double y);
static double zbrowser_lainos_stb_cos(double x);
static double zbrowser_lainos_stb_acos(double x);
static nserror zbrowser_lainos_schedule(int t, void (*callback)(void *p), void *p);

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_malloc(size, userdata) ((void)(userdata), malloc((size_t)(size)))
#define STBTT_free(ptr, userdata) ((void)(userdata), free(ptr))
#define STBTT_ifloor(x) zbrowser_lainos_stb_floor((float)(x))
#define STBTT_iceil(x) zbrowser_lainos_stb_ceil((float)(x))
#define STBTT_sqrt(x) zbrowser_lainos_stb_sqrt((double)(x))
#define STBTT_pow(x, y) zbrowser_lainos_stb_pow((double)(x), (double)(y))
#define STBTT_fmod(x, y) zbrowser_lainos_stb_fmod((double)(x), (double)(y))
#define STBTT_cos(x) zbrowser_lainos_stb_cos((double)(x))
#define STBTT_acos(x) zbrowser_lainos_stb_acos((double)(x))
#define STBTT_fabs(x) zbrowser_lainos_stb_fabs((double)(x))
#include "stb_truetype.h"

extern time_t time(time_t *timer);
extern struct nsurl *zbrowser_engine_current_base_url(void);
extern int image_probe(const uint8_t *data, uint32_t size, image_info_t *out_image);
extern int image_decode_rgba32(const uint8_t *data,
                               uint32_t size,
                               uint8_t *rgba,
                               uint32_t rgba_capacity,
                               image_info_t *out_image);

int errno;
FILE *stderr;

static uint32_t zbrowser_lainos_http_fetch_count;
static uint32_t zbrowser_lainos_http_fetch_success_count;
static uint32_t zbrowser_lainos_http_fetch_error_count;
static uint32_t zbrowser_lainos_http_last_status;
static uint32_t zbrowser_lainos_http_last_bytes;
static int32_t zbrowser_lainos_http_last_error;
static uint32_t zbrowser_lainos_resource_cache_hits;
static uint32_t zbrowser_lainos_resource_cache_stores;
static uint32_t zbrowser_lainos_resource_cache_evictions;
static uint32_t zbrowser_lainos_css_compat_count;
static uint32_t zbrowser_lainos_css_compat_var_count;
static uint32_t zbrowser_lainos_css_compat_group_count;
static uint32_t zbrowser_lainos_image_decode_count;
static uint32_t zbrowser_lainos_image_fallback_count;
static uint32_t zbrowser_lainos_image_error_count;
static char zbrowser_lainos_pending_url[1024];
static bool zbrowser_lainos_navigation_pending;

typedef struct zbrowser_lainos_http_info {
    uint32_t status_code;
    uint32_t content_length;
    uint32_t body_size;
    uint32_t header_size;
    uint32_t flags;
    int32_t error;
    uint32_t tls_error;
    char content_type[64];
    char location[1024];
} zbrowser_lainos_http_info_t;

extern int os_http_get_ex(const char *url,
                          char *buffer,
                          uint32_t capacity,
                          zbrowser_lainos_http_info_t *info);
extern void put_pixel(uint32_t x, uint32_t y, uint32_t color);
extern uint32_t gfx_get_pixel(uint32_t x, uint32_t y);
extern uint32_t gfx_width(void);
extern uint32_t gfx_height(void);

#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
static const char *zbrowser_lainos_filetype_for_url(const char *url);
void fetch_free(struct fetch *fetch);
nserror fetch_fdset(fd_set *read_fd_set,
        fd_set *write_fd_set,
        fd_set *except_fd_set,
        int *maxfd);
void fetch_remove_from_queues(struct fetch *fetch);
void fetch_send_callback(const fetch_msg *msg, struct fetch *fetch);
nserror fetch_set_http_code(struct fetch *fetch, http_response_code http_code);
static struct nsurl *zbrowser_lainos_resource_url(const char *path);
static nserror zbrowser_lainos_resource_data(const char *path,
                                             const uint8_t **data,
                                             size_t *data_len);
static nserror zbrowser_lainos_release_resource_data(const uint8_t *data);
static nserror zbrowser_lainos_llcache_initialise(
        const struct llcache_store_parameters *parameters);
static nserror zbrowser_lainos_llcache_finalise(void);
static nserror zbrowser_lainos_llcache_store(struct nsurl *url,
                                             enum backing_store_flags flags,
                                             uint8_t *data,
                                             const size_t datalen);
static nserror zbrowser_lainos_llcache_fetch(struct nsurl *url,
                                             enum backing_store_flags flags,
                                             uint8_t **data_out,
                                             size_t *datalen_out);
static nserror zbrowser_lainos_llcache_release(struct nsurl *url,
                                               enum backing_store_flags flags);
static nserror zbrowser_lainos_llcache_invalidate(struct nsurl *url);
static bool zbrowser_lainos_url_contains(const char *url, const char *needle);
static void zbrowser_lainos_normalise_content_type(const char *in, char *out, size_t out_size);
static bool zbrowser_lainos_is_css_response(const char *content_type, const char *url);
static char *zbrowser_lainos_css_compat_filter(const char *css,
                                               uint32_t len,
                                               uint32_t *out_len);

#ifndef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
struct llcache_handle {
    struct nsurl *url;
    struct content *content;
    const uint8_t *source_data;
    size_t source_len;
    const char *content_type;
    llcache_handle_callback cb;
    void *pw;
    uint8_t *owned_source_data;
};

struct hlcache_handle {
    struct nsurl *url;
    struct content *content;
    struct llcache_handle llcache;
    hlcache_handle_callback cb;
    void *pw;
};

typedef struct zbrowser_lainos_hlcache_deferred_event {
    struct hlcache_handle *handle;
    hlcache_event event;
} zbrowser_lainos_hlcache_deferred_event_t;

static void zbrowser_lainos_hlcache_dispatch_deferred(void *p) {
    zbrowser_lainos_hlcache_deferred_event_t *deferred =
        (zbrowser_lainos_hlcache_deferred_event_t *)p;

    if (deferred == 0) {
        return;
    }
    if (deferred->handle != 0 && deferred->handle->cb != 0) {
        (void)deferred->handle->cb(deferred->handle,
                                   &deferred->event,
                                   deferred->handle->pw);
    }
    free(deferred);
}

static void zbrowser_lainos_hlcache_queue_event(struct hlcache_handle *handle,
        content_msg msg,
        const union content_msg_data *data) {
    zbrowser_lainos_hlcache_deferred_event_t *deferred;

    if (handle == 0 || handle->cb == 0) {
        return;
    }
    deferred = calloc(1, sizeof(*deferred));
    if (deferred == 0) {
        return;
    }
    deferred->handle = handle;
    deferred->event.type = msg;
    if (data != 0) {
        deferred->event.data = *data;
    }
    if (zbrowser_lainos_schedule(0, zbrowser_lainos_hlcache_dispatch_deferred, deferred) != NSERROR_OK) {
        free(deferred);
    }
}

nserror llcache_handle_change_callback(llcache_handle *handle,
        llcache_handle_callback cb, void *pw) {
    if (handle != 0) {
        handle->cb = cb;
        handle->pw = pw;
    }
    return NSERROR_OK;
}

nserror llcache_handle_release(llcache_handle *handle) {
    if (handle != 0 && handle->owned_source_data != 0) {
        free(handle->owned_source_data);
        handle->owned_source_data = 0;
        handle->source_data = 0;
        handle->source_len = 0;
    }
    return NSERROR_OK;
}

nserror llcache_handle_abort(llcache_handle *handle) {
    (void)handle;
    return NSERROR_OK;
}

nsurl *llcache_handle_get_url(const llcache_handle *handle) {
    return handle != 0 && handle->url != 0
        ? handle->url
        : zbrowser_engine_current_base_url();
}

const uint8_t *llcache_handle_get_source_data(const llcache_handle *handle,
        size_t *size) {
    if (size != 0) {
        *size = handle != 0 ? handle->source_len : 0u;
    }
    return handle != 0 ? handle->source_data : 0;
}

const char *llcache_handle_get_header(const llcache_handle *handle,
        const char *key) {
    if (handle == 0 || key == 0) {
        return 0;
    }
    if (strcmp(key, "Content-Type") == 0) {
        return handle->content_type != 0
            ? handle->content_type
            : "text/html; charset=utf-8";
    }
    return 0;
}

bool llcache_handle_references_same_object(const llcache_handle *a,
        const llcache_handle *b) {
    return a == b;
}
#endif
#endif

#define ZBROWSER_LAINOS_SCHEDULE_MAX 256u
#define ZBROWSER_LAINOS_SCHEDULE_PUMP_LIMIT 64u

typedef struct zbrowser_lainos_schedule_item {
    bool active;
    void (*callback)(void *p);
    void *p;
} zbrowser_lainos_schedule_item_t;

static zbrowser_lainos_schedule_item_t zbrowser_lainos_schedule_queue[ZBROWSER_LAINOS_SCHEDULE_MAX];
static int zbrowser_lainos_schedule_draining;
static stbtt_fontinfo zbrowser_lainos_font_info;
static int zbrowser_lainos_font_tried;
static int zbrowser_lainos_font_ready;

#define ZBROWSER_LAINOS_FONT_CACHE_SLOTS 192u

typedef struct zbrowser_lainos_font_cache_entry {
    uint32_t codepoint;
    int px;
    int width;
    int height;
    int xoff;
    int yoff;
    int advance;
    uint32_t age;
    uint8_t *alpha;
} zbrowser_lainos_font_cache_entry_t;

static zbrowser_lainos_font_cache_entry_t zbrowser_lainos_font_cache[ZBROWSER_LAINOS_FONT_CACHE_SLOTS];
static uint32_t zbrowser_lainos_font_cache_age;

typedef struct zbrowser_lainos_bitmap {
    int width;
    int height;
    bool opaque;
    uint8_t *pixels;
} zbrowser_lainos_bitmap_t;

typedef struct zbrowser_lainos_image_content {
    struct content base;
    struct bitmap *bitmap;
} zbrowser_lainos_image_content_t;

extern struct netsurf_table *guit;

static int zbrowser_lainos_float_to_int(float value) {
    return (int)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

static int zbrowser_lainos_stb_floor(float value) {
    int i = (int)value;
    return value < (float)i ? i - 1 : i;
}

static int zbrowser_lainos_stb_ceil(float value) {
    int i = (int)value;
    return value > (float)i ? i + 1 : i;
}

static double zbrowser_lainos_stb_fabs(double value) {
    return value < 0.0 ? -value : value;
}

static double zbrowser_lainos_stb_sqrt(double value) {
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

static double zbrowser_lainos_stb_fmod(double x, double y) {
    int q;
    if (y == 0.0) {
        return 0.0;
    }
    q = (int)(x / y);
    return x - (double)q * y;
}

static double zbrowser_lainos_stb_pow(double x, double y) {
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

static double zbrowser_lainos_stb_cos(double x) {
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

static double zbrowser_lainos_stb_acos(double x) {
    const double half_pi = 1.57079632679489661923;
    if (x <= -1.0) {
        return 2.0 * half_pi;
    }
    if (x >= 1.0) {
        return 0.0;
    }
    return half_pi - x;
}

void zbrowser_lainos_schedule_clear(void) {
    size_t i;

    for (i = 0; i < ZBROWSER_LAINOS_SCHEDULE_MAX; ++i) {
        zbrowser_lainos_schedule_queue[i].active = false;
    }
}

unsigned int zbrowser_lainos_schedule_count(void) {
    unsigned int count = 0;
    size_t i;

    for (i = 0; i < ZBROWSER_LAINOS_SCHEDULE_MAX; ++i) {
        if (zbrowser_lainos_schedule_queue[i].active) {
            ++count;
        }
    }
    return count;
}

static void zbrowser_lainos_schedule_remove(void (*callback)(void *p), void *p) {
    size_t i;

    for (i = 0; i < ZBROWSER_LAINOS_SCHEDULE_MAX; ++i) {
        if (zbrowser_lainos_schedule_queue[i].active &&
            zbrowser_lainos_schedule_queue[i].callback == callback &&
            zbrowser_lainos_schedule_queue[i].p == p) {
            zbrowser_lainos_schedule_queue[i].active = false;
        }
    }
}

unsigned int zbrowser_lainos_run_scheduled_budget(unsigned int callback_limit) {
    unsigned int iterations = 0;
    unsigned int ran = 0;

    if (zbrowser_lainos_schedule_draining != 0) {
        return 0;
    }
    if (callback_limit == 0u) {
        callback_limit = ZBROWSER_LAINOS_SCHEDULE_PUMP_LIMIT;
    }
    zbrowser_lainos_schedule_draining = 1;
    while (iterations++ < callback_limit) {
        size_t i;
        void (*callback)(void *p) = 0;
        void *p = 0;

        for (i = 0; i < ZBROWSER_LAINOS_SCHEDULE_MAX; ++i) {
            if (zbrowser_lainos_schedule_queue[i].active) {
                callback = zbrowser_lainos_schedule_queue[i].callback;
                p = zbrowser_lainos_schedule_queue[i].p;
                zbrowser_lainos_schedule_queue[i].active = false;
                break;
            }
        }
        if (callback == 0) {
            break;
        }
        callback(p);
        ++ran;
    }
    zbrowser_lainos_schedule_draining = 0;
    return ran;
}

static nserror zbrowser_lainos_schedule(int t, void (*callback)(void *p), void *p) {
    size_t i;
    size_t free_slot = ZBROWSER_LAINOS_SCHEDULE_MAX;

    if (callback == 0) {
        return NSERROR_OK;
    }
    if (t < 0) {
        zbrowser_lainos_schedule_remove(callback, p);
        return NSERROR_OK;
    }
    for (i = 0; i < ZBROWSER_LAINOS_SCHEDULE_MAX; ++i) {
        if (zbrowser_lainos_schedule_queue[i].active) {
            if (zbrowser_lainos_schedule_queue[i].callback == callback &&
                zbrowser_lainos_schedule_queue[i].p == p) {
                return NSERROR_OK;
            }
        } else if (free_slot == ZBROWSER_LAINOS_SCHEDULE_MAX) {
            free_slot = i;
        }
    }
    if (free_slot == ZBROWSER_LAINOS_SCHEDULE_MAX) {
        return NSERROR_NOMEM;
    }

    zbrowser_lainos_schedule_queue[free_slot].active = true;
    zbrowser_lainos_schedule_queue[free_slot].callback = callback;
    zbrowser_lainos_schedule_queue[free_slot].p = p;
    return NSERROR_OK;
}

static int zbrowser_lainos_utf8_is_trail(unsigned char ch) {
    return (ch & 0xc0u) == 0x80u;
}

static size_t zbrowser_lainos_utf8_next_offset(const char *string,
        size_t length,
        size_t offset) {
    size_t next = offset + 1u;
    if (string == 0 || offset >= length) {
        return length;
    }
    while (next < length && zbrowser_lainos_utf8_is_trail((unsigned char)string[next])) {
        ++next;
    }
    return next;
}

static size_t zbrowser_lainos_utf8_next(const char *string,
        size_t length,
        size_t pos,
        uint32_t *codepoint) {
    const unsigned char *s = (const unsigned char *)string;
    uint32_t cp;

    if (pos >= length) {
        *codepoint = 0;
        return pos;
    }
    if (s[pos] < 0x80u) {
        *codepoint = s[pos];
        return pos + 1;
    }
    if (pos + 1 < length &&
        (s[pos] & 0xe0u) == 0xc0u &&
        (s[pos + 1] & 0xc0u) == 0x80u) {
        cp = ((uint32_t)(s[pos] & 0x1fu) << 6) |
             (uint32_t)(s[pos + 1] & 0x3fu);
        if (cp >= 0x80u) {
            *codepoint = cp;
            return pos + 2;
        }
    } else if (pos + 2 < length &&
               (s[pos] & 0xf0u) == 0xe0u &&
               (s[pos + 1] & 0xc0u) == 0x80u &&
               (s[pos + 2] & 0xc0u) == 0x80u) {
        cp = ((uint32_t)(s[pos] & 0x0fu) << 12) |
             ((uint32_t)(s[pos + 1] & 0x3fu) << 6) |
             (uint32_t)(s[pos + 2] & 0x3fu);
        if (cp >= 0x800u && (cp < 0xd800u || cp > 0xdfffu)) {
            *codepoint = cp;
            return pos + 3;
        }
    } else if (pos + 3 < length &&
               (s[pos] & 0xf8u) == 0xf0u &&
               (s[pos + 1] & 0xc0u) == 0x80u &&
               (s[pos + 2] & 0xc0u) == 0x80u &&
               (s[pos + 3] & 0xc0u) == 0x80u) {
        cp = ((uint32_t)(s[pos] & 0x07u) << 18) |
             ((uint32_t)(s[pos + 1] & 0x3fu) << 12) |
             ((uint32_t)(s[pos + 2] & 0x3fu) << 6) |
             (uint32_t)(s[pos + 3] & 0x3fu);
        if (cp >= 0x10000u && cp <= 0x10ffffu) {
            *codepoint = cp;
            return pos + 4;
        }
    }

    *codepoint = s[pos];
    return pos + 1;
}

static int zbrowser_lainos_ttf_ready(void) {
    int offset;

    if (zbrowser_lainos_font_tried != 0) {
        return zbrowser_lainos_font_ready;
    }
    zbrowser_lainos_font_tried = 1;
    offset = stbtt_GetFontOffsetForIndex(dejavu_sans_ttf, 0);
    if (offset < 0) {
        return 0;
    }
    zbrowser_lainos_font_ready =
        stbtt_InitFont(&zbrowser_lainos_font_info, dejavu_sans_ttf, offset) != 0;
    return zbrowser_lainos_font_ready;
}

static int zbrowser_lainos_font_px(const struct plot_font_style *fstyle) {
    int font_px = 10;

    if (fstyle != 0) {
        font_px = plot_style_fixed_to_int(fstyle->size);
    }
    if (font_px < 8) {
        font_px = 8;
    } else if (font_px > 48) {
        font_px = 48;
    }
    return font_px;
}

static float zbrowser_lainos_ttf_scale(int px) {
    if (px < 8) {
        px = 8;
    } else if (px > 96) {
        px = 96;
    }
    return stbtt_ScaleForPixelHeight(&zbrowser_lainos_font_info, (float)px);
}

static int zbrowser_lainos_glyph_width(const struct plot_font_style *fstyle) {
    int px = zbrowser_lainos_font_px(fstyle);
    int width = (px * 56 + 50) / 100;

    if (fstyle != 0 && fstyle->family == PLOT_FONT_FAMILY_MONOSPACE) {
        width = (px * 64 + 50) / 100;
    } else if (fstyle != 0 && fstyle->family == PLOT_FONT_FAMILY_SERIF) {
        width = (px * 58 + 50) / 100;
    }
    if (fstyle != 0 && fstyle->weight >= 700) {
        ++width;
    }
    return width < 4 ? 4 : width;
}

static uint32_t zbrowser_lainos_utf8_codepoint(const char *string,
        size_t length,
        size_t offset) {
    uint32_t codepoint = 0xfffdu;
    (void)zbrowser_lainos_utf8_next(string, length, offset, &codepoint);
    return codepoint;
}

static int zbrowser_lainos_char_width(const struct plot_font_style *fstyle,
        const char *string,
        size_t offset,
        size_t next);

static int zbrowser_lainos_ttf_char_width(const struct plot_font_style *fstyle,
        uint32_t codepoint) {
    int advance = 0;
    int lsb = 0;
    int width;

    if (!zbrowser_lainos_ttf_ready()) {
        return 0;
    }
    if (codepoint == '\t') {
        codepoint = ' ';
    }
    stbtt_GetCodepointHMetrics(&zbrowser_lainos_font_info, (int)codepoint, &advance, &lsb);
    width = zbrowser_lainos_float_to_int((float)advance *
        zbrowser_lainos_ttf_scale(zbrowser_lainos_font_px(fstyle)));
    if (fstyle != 0 && fstyle->weight >= 700) {
        ++width;
    }
    if (width < 1 && codepoint != 0u) {
        width = zbrowser_lainos_font_px(fstyle) / 2;
    }
    return width;
}

static uint32_t zbrowser_lainos_blend_rgb(uint32_t dst, uint32_t src, uint32_t alpha) {
    uint32_t inv = 255u - alpha;
    uint32_t r = (((dst >> 16) & 0xffu) * inv + ((src >> 16) & 0xffu) * alpha + 127u) / 255u;
    uint32_t g = (((dst >> 8) & 0xffu) * inv + ((src >> 8) & 0xffu) * alpha + 127u) / 255u;
    uint32_t b = ((dst & 0xffu) * inv + (src & 0xffu) * alpha + 127u) / 255u;

    return (r << 16) | (g << 8) | b;
}

static zbrowser_lainos_font_cache_entry_t *zbrowser_lainos_ttf_glyph(
        const struct plot_font_style *fstyle,
        uint32_t codepoint) {
    zbrowser_lainos_font_cache_entry_t *entry = 0;
    uint32_t oldest_age = 0xffffffffu;
    uint32_t oldest_index = 0u;
    int px = zbrowser_lainos_font_px(fstyle);
    float scale;
    int advance = 0;
    int lsb = 0;

    if (!zbrowser_lainos_ttf_ready()) {
        return 0;
    }
    if (codepoint == '\t') {
        codepoint = ' ';
    }
    ++zbrowser_lainos_font_cache_age;
    if (zbrowser_lainos_font_cache_age == 0u) {
        zbrowser_lainos_font_cache_age = 1u;
    }
    for (uint32_t i = 0; i < ZBROWSER_LAINOS_FONT_CACHE_SLOTS; ++i) {
        entry = &zbrowser_lainos_font_cache[i];
        if (entry->alpha != 0 && entry->codepoint == codepoint && entry->px == px) {
            entry->age = zbrowser_lainos_font_cache_age;
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

    entry = &zbrowser_lainos_font_cache[oldest_index];
    if (entry->alpha != 0) {
        free(entry->alpha);
    }
    memset(entry, 0, sizeof(*entry));
    scale = zbrowser_lainos_ttf_scale(px);
    stbtt_GetCodepointHMetrics(&zbrowser_lainos_font_info, (int)codepoint, &advance, &lsb);
    entry->alpha = stbtt_GetCodepointBitmap(&zbrowser_lainos_font_info,
                                            0.0f,
                                            scale,
                                            (int)codepoint,
                                            &entry->width,
                                            &entry->height,
                                            &entry->xoff,
                                            &entry->yoff);
    entry->codepoint = codepoint;
    entry->px = px;
    entry->advance = zbrowser_lainos_float_to_int((float)advance * scale);
    entry->age = zbrowser_lainos_font_cache_age;
    if (entry->advance < 1) {
        entry->advance = zbrowser_lainos_glyph_width(fstyle);
    }
    if (entry->alpha == 0) {
        memset(entry, 0, sizeof(*entry));
        return 0;
    }
    return entry;
}

static int zbrowser_lainos_plot_ttf_glyph(const struct plot_font_style *fstyle,
        uint32_t codepoint,
        int x,
        int baseline_y,
        uint32_t fg,
        int bold,
        int italic,
        const struct rect *clip) {
    zbrowser_lainos_font_cache_entry_t *glyph =
        zbrowser_lainos_ttf_glyph(fstyle, codepoint);
    int passes = bold != 0 ? 2 : 1;
    int screen_width = (int)gfx_width();
    int screen_height = (int)gfx_height();

    if (glyph == 0 || glyph->alpha == 0) {
        return 0;
    }
    for (int pass = 0; pass < passes; ++pass) {
        for (int row = 0; row < glyph->height; ++row) {
            int shear = italic != 0 ? (glyph->height - row) / 5 : 0;
            int py = baseline_y + glyph->yoff + row;

            if (py < 0 || py >= screen_height) {
                continue;
            }
            if (clip != 0 && (py < clip->y0 || py >= clip->y1)) {
                continue;
            }
            for (int col = 0; col < glyph->width; ++col) {
                uint32_t alpha = glyph->alpha[(size_t)row * (size_t)glyph->width + (size_t)col];
                int px;
                uint32_t dst;
                uint32_t color;

                if (alpha == 0u) {
                    continue;
                }
                px = x + glyph->xoff + col + shear + pass;
                if (px < 0 || px >= screen_width) {
                    continue;
                }
                if (clip != 0 && (px < clip->x0 || px >= clip->x1)) {
                    continue;
                }
                dst = gfx_get_pixel((uint32_t)px, (uint32_t)py);
                color = zbrowser_lainos_blend_rgb(dst, fg, alpha);
                put_pixel((uint32_t)px, (uint32_t)py, color);
            }
        }
    }
    return glyph->advance + (bold != 0 ? 1 : 0);
}

int zbrowser_lainos_plot_text_ttf(const struct plot_font_style *fstyle,
        int x,
        int y,
        const char *text,
        size_t length,
        uint32_t fg,
        int bold,
        int italic,
        const struct rect *clip) {
    size_t offset = 0;
    int pen_x = x;

    if (text == 0 || length == 0u || fstyle == 0) {
        return 0;
    }
    while (offset < length) {
        uint32_t codepoint = 0xfffdu;
        size_t next = zbrowser_lainos_utf8_next(text, length, offset, &codepoint);
        int advance = zbrowser_lainos_plot_ttf_glyph(fstyle,
                                                     codepoint,
                                                     pen_x,
                                                     y,
                                                     fg,
                                                     bold,
                                                     italic,
                                                     clip);
        if (advance <= 0) {
            advance = zbrowser_lainos_char_width(fstyle, text, offset, next);
        }
        pen_x += advance;
        offset = next;
    }
    return pen_x - x;
}

static int zbrowser_lainos_char_width(const struct plot_font_style *fstyle,
        const char *string,
        size_t offset,
        size_t next) {
    uint32_t ch;
    int width;

    if (string == 0 || offset >= next) {
        return zbrowser_lainos_glyph_width(fstyle);
    }
    ch = zbrowser_lainos_utf8_codepoint(string, next, offset);
    width = zbrowser_lainos_ttf_char_width(fstyle, ch);
    if (width > 0) {
        return width;
    }
    width = zbrowser_lainos_glyph_width(fstyle);
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
    return width < 3 ? 3 : width;
}

static nserror zbrowser_lainos_layout_width(const struct plot_font_style *fstyle,
        const char *string, size_t length, int *width) {
    size_t offset = 0;
    int total = 0;

    if (width == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    while (offset < length) {
        size_t next = zbrowser_lainos_utf8_next_offset(string, length, offset);
        total += zbrowser_lainos_char_width(fstyle, string, offset, next);
        offset = next;
    }
    *width = total;
    return NSERROR_OK;
}

static nserror zbrowser_lainos_layout_position(const struct plot_font_style *fstyle,
        const char *string, size_t length, int x, size_t *char_offset, int *actual_x) {
    int current_x = 0;
    size_t offset = 0;

    if (char_offset == 0 || actual_x == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    if (x <= 0 || string == 0 || length == 0u) {
        *char_offset = 0;
        *actual_x = 0;
        return NSERROR_OK;
    }

    while (offset < length) {
        int char_width;
        size_t next = zbrowser_lainos_utf8_next_offset(string, length, offset);
        char_width = zbrowser_lainos_char_width(fstyle, string, offset, next);
        if (x < current_x + (char_width / 2)) {
            break;
        }
        offset = next;
        current_x += char_width;
    }

    *char_offset = offset;
    *actual_x = current_x;
    return NSERROR_OK;
}

static nserror zbrowser_lainos_layout_split(const struct plot_font_style *fstyle,
        const char *string, size_t length, int x, size_t *char_offset, int *actual_x) {
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
        size_t next = zbrowser_lainos_utf8_next_offset(string, length, offset);
        int glyph_width = zbrowser_lainos_char_width(fstyle, string, offset, next);
        int next_x = current_x + glyph_width;
        if (string[offset] == ' ' || string[offset] == '\t' ||
            string[offset] == '\n' || string[offset] == '\r') {
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
        best_offset = zbrowser_lainos_utf8_next_offset(string, length, 0u);
        best_x = zbrowser_lainos_char_width(fstyle, string, 0u, best_offset);
    }
    *char_offset = best_offset;
    *actual_x = best_x;
    return NSERROR_OK;
}

const struct gui_layout_table zbrowser_lainos_layout_table = {
    .width = zbrowser_lainos_layout_width,
    .position = zbrowser_lainos_layout_position,
    .split = zbrowser_lainos_layout_split
};

static void *zbrowser_lainos_bitmap_create(int width,
        int height,
        enum gui_bitmap_flags flags) {
    zbrowser_lainos_bitmap_t *bitmap;
    size_t bytes;

    if (width <= 0 || height <= 0 ||
        width > 8192 || height > 8192 ||
        (size_t)width > ((size_t)-1) / (size_t)height / 4u) {
        return 0;
    }
    bytes = (size_t)width * (size_t)height * 4u;
    bitmap = calloc(1, sizeof(*bitmap));
    if (bitmap == 0) {
        return 0;
    }
    bitmap->pixels = malloc(bytes);
    if (bitmap->pixels == 0) {
        free(bitmap);
        return 0;
    }
    bitmap->width = width;
    bitmap->height = height;
    bitmap->opaque = (flags & BITMAP_OPAQUE) != 0;
    if ((flags & BITMAP_CLEAR) != 0) {
        memset(bitmap->pixels, 0, bytes);
    }
    return bitmap;
}

static void zbrowser_lainos_bitmap_destroy(void *bitmap) {
    zbrowser_lainos_bitmap_t *b = (zbrowser_lainos_bitmap_t *)bitmap;
    if (b != 0) {
        free(b->pixels);
        free(b);
    }
}

static void zbrowser_lainos_bitmap_set_opaque(void *bitmap, bool opaque) {
    if (bitmap != 0) {
        ((zbrowser_lainos_bitmap_t *)bitmap)->opaque = opaque;
    }
}

static bool zbrowser_lainos_bitmap_get_opaque(void *bitmap) {
    return bitmap != 0 ? ((zbrowser_lainos_bitmap_t *)bitmap)->opaque : false;
}

static unsigned char *zbrowser_lainos_bitmap_get_buffer(void *bitmap) {
    return bitmap != 0 ? ((zbrowser_lainos_bitmap_t *)bitmap)->pixels : 0;
}

static size_t zbrowser_lainos_bitmap_get_rowstride(void *bitmap) {
    return bitmap != 0 ? (size_t)((zbrowser_lainos_bitmap_t *)bitmap)->width * 4u : 0u;
}

static int zbrowser_lainos_bitmap_get_width(void *bitmap) {
    return bitmap != 0 ? ((zbrowser_lainos_bitmap_t *)bitmap)->width : 0;
}

static int zbrowser_lainos_bitmap_get_height(void *bitmap) {
    return bitmap != 0 ? ((zbrowser_lainos_bitmap_t *)bitmap)->height : 0;
}

static void zbrowser_lainos_bitmap_modified(void *bitmap) {
    (void)bitmap;
}

static nserror zbrowser_lainos_bitmap_render(struct bitmap *bitmap,
        struct hlcache_handle *content) {
    (void)bitmap;
    (void)content;
    return NSERROR_NOT_IMPLEMENTED;
}

int zbrowser_lainos_bitmap_width(struct bitmap *bitmap) {
    return zbrowser_lainos_bitmap_get_width(bitmap);
}

int zbrowser_lainos_bitmap_height(struct bitmap *bitmap) {
    return zbrowser_lainos_bitmap_get_height(bitmap);
}

int zbrowser_lainos_bitmap_rowstride(struct bitmap *bitmap) {
    return (int)zbrowser_lainos_bitmap_get_rowstride(bitmap);
}

int zbrowser_lainos_bitmap_opaque(struct bitmap *bitmap) {
    return zbrowser_lainos_bitmap_get_opaque(bitmap) ? 1 : 0;
}

uint8_t *zbrowser_lainos_bitmap_buffer(struct bitmap *bitmap) {
    return zbrowser_lainos_bitmap_get_buffer(bitmap);
}

static struct gui_bitmap_table zbrowser_lainos_bitmap_table = {
    .create = zbrowser_lainos_bitmap_create,
    .destroy = zbrowser_lainos_bitmap_destroy,
    .set_opaque = zbrowser_lainos_bitmap_set_opaque,
    .get_opaque = zbrowser_lainos_bitmap_get_opaque,
    .get_buffer = zbrowser_lainos_bitmap_get_buffer,
    .get_rowstride = zbrowser_lainos_bitmap_get_rowstride,
    .get_width = zbrowser_lainos_bitmap_get_width,
    .get_height = zbrowser_lainos_bitmap_get_height,
    .modified = zbrowser_lainos_bitmap_modified,
    .render = zbrowser_lainos_bitmap_render
};

static bool zbrowser_lainos_image_placeholder(zbrowser_lainos_image_content_t *image,
        int width,
        int height,
        uint32_t fill,
        uint32_t border) {
    uint8_t *dst;
    int rowstride;

    if (image == 0) {
        return false;
    }
    ++zbrowser_lainos_image_fallback_count;
    image->bitmap = (struct bitmap *)guit->bitmap->create(width, height, BITMAP_OPAQUE | BITMAP_CLEAR);
    if (image->bitmap == 0) {
        return false;
    }
    dst = guit->bitmap->get_buffer(image->bitmap);
    rowstride = (int)guit->bitmap->get_rowstride(image->bitmap);
    if (dst == 0 || rowstride <= 0) {
        guit->bitmap->destroy(image->bitmap);
        image->bitmap = 0;
        return false;
    }
    for (int y = 0; y < height; ++y) {
        uint8_t *row = dst + (size_t)y * (size_t)rowstride;
        for (int x = 0; x < width; ++x) {
            bool edge = y == 0 || x == 0 || y == height - 1 || x == width - 1;
            uint32_t c = edge ? border : fill;
            row[x * 4 + 0] = (uint8_t)((c >> 16) & 0xffu);
            row[x * 4 + 1] = (uint8_t)((c >> 8) & 0xffu);
            row[x * 4 + 2] = (uint8_t)(c & 0xffu);
            row[x * 4 + 3] = 0xffu;
        }
    }
    guit->bitmap->set_opaque(image->bitmap, true);
    guit->bitmap->modified(image->bitmap);
    image->base.width = width;
    image->base.height = height;
    image->base.status = CONTENT_STATUS_DONE;
    return true;
}

static bool zbrowser_lainos_image_decode(zbrowser_lainos_image_content_t *image) {
    const uint8_t *source;
    uint8_t *rgba;
    uint8_t *dst;
    image_info_t info;
    size_t source_size;
    size_t rgba_bytes;
    int rowstride;
    bool opaque = true;

    source = content__get_source_data(&image->base, &source_size);
    if (source == 0 || source_size == 0u || source_size > 0xffffffffu ||
        image_probe(source, (uint32_t)source_size, &info) != IMAGE_OK ||
        info.width == 0u || info.height == 0u ||
        info.width > 4096u || info.height > 4096u ||
        info.width > 0xffffffffu / info.height) {
        return zbrowser_lainos_image_placeholder(image, 96, 64, 0xf0f3f5u, 0x7b8794u);
    }
    rgba_bytes = (size_t)info.width * (size_t)info.height * 4u;
    if (rgba_bytes > 32u * 1024u * 1024u) {
        ++zbrowser_lainos_image_error_count;
        content_broadcast_error(&image->base, NSERROR_NOMEM, "image too large");
        return false;
    }
    rgba = malloc(rgba_bytes);
    if (rgba == 0) {
        ++zbrowser_lainos_image_error_count;
        content_broadcast_error(&image->base, NSERROR_NOMEM, "image decode memory");
        return false;
    }
    if (image_decode_rgba32(source,
                            (uint32_t)source_size,
                            rgba,
                            (uint32_t)rgba_bytes,
                            &info) != IMAGE_OK) {
        free(rgba);
        return zbrowser_lainos_image_placeholder(image, 96, 64, 0xf0f3f5u, 0x7b8794u);
    }
    image->bitmap = (struct bitmap *)guit->bitmap->create((int)info.width,
                                                          (int)info.height,
                                                          BITMAP_CLEAR);
    if (image->bitmap == 0) {
        free(rgba);
        ++zbrowser_lainos_image_error_count;
        content_broadcast_error(&image->base, NSERROR_NOMEM, "image bitmap memory");
        return false;
    }
    dst = guit->bitmap->get_buffer(image->bitmap);
    rowstride = (int)guit->bitmap->get_rowstride(image->bitmap);
    if (dst == 0 || rowstride <= 0) {
        guit->bitmap->destroy(image->bitmap);
        image->bitmap = 0;
        free(rgba);
        ++zbrowser_lainos_image_error_count;
        content_broadcast_error(&image->base, NSERROR_NOMEM, "image bitmap unavailable");
        return false;
    }
    for (uint32_t y = 0; y < info.height; ++y) {
        uint8_t *row = dst + (size_t)y * (size_t)rowstride;
        const uint8_t *src = rgba + (size_t)y * (size_t)info.width * 4u;
        for (uint32_t x = 0; x < info.width; ++x) {
            row[x * 4u + 0u] = src[x * 4u + 0u];
            row[x * 4u + 1u] = src[x * 4u + 1u];
            row[x * 4u + 2u] = src[x * 4u + 2u];
            row[x * 4u + 3u] = src[x * 4u + 3u];
            if (src[x * 4u + 3u] != 0xffu) {
                opaque = false;
            }
        }
    }
    free(rgba);
    guit->bitmap->set_opaque(image->bitmap, opaque);
    guit->bitmap->modified(image->bitmap);
    image->base.width = (int)info.width;
    image->base.height = (int)info.height;
    image->base.size += (unsigned int)rgba_bytes;
    image->base.status = CONTENT_STATUS_DONE;
    content_set_ready(&image->base);
    content_set_done(&image->base);
    content_set_status(&image->base, "");
    ++zbrowser_lainos_image_decode_count;
    return true;
}

static nserror zbrowser_lainos_image_create(const content_handler *handler,
        lwc_string *imime_type,
        const struct http_parameter *params,
        struct llcache_handle *llcache,
        const char *fallback_charset,
        bool quirks,
        struct content **c) {
    zbrowser_lainos_image_content_t *image = calloc(1, sizeof(*image));
    nserror err;

    if (image == 0) {
        return NSERROR_NOMEM;
    }
    err = content__init(&image->base, handler, imime_type, params, llcache, fallback_charset, quirks);
    if (err != NSERROR_OK) {
        free(image);
        return err;
    }
    *c = &image->base;
    return NSERROR_OK;
}

static bool zbrowser_lainos_image_complete(struct content *c) {
    return zbrowser_lainos_image_decode((zbrowser_lainos_image_content_t *)c);
}

static bool zbrowser_lainos_image_redraw(struct content *c,
        struct content_redraw_data *data,
        const struct rect *clip,
        const struct redraw_context *ctx) {
    zbrowser_lainos_image_content_t *image = (zbrowser_lainos_image_content_t *)c;
    bitmap_flags_t flags = BITMAPF_NONE;

    (void)clip;
    if (image == 0 || image->bitmap == 0 || data == 0 ||
        ctx == 0 || ctx->plot == 0 || ctx->plot->bitmap == 0) {
        return false;
    }
    if (data->repeat_x) {
        flags |= BITMAPF_REPEAT_X;
    }
    if (data->repeat_y) {
        flags |= BITMAPF_REPEAT_Y;
    }
    return ctx->plot->bitmap(ctx,
                             image->bitmap,
                             data->x,
                             data->y,
                             data->width,
                             data->height,
                             data->background_colour,
                             flags) == NSERROR_OK;
}

static void zbrowser_lainos_image_destroy(struct content *c) {
    zbrowser_lainos_image_content_t *image = (zbrowser_lainos_image_content_t *)c;
    if (image != 0 && image->bitmap != 0) {
        guit->bitmap->destroy(image->bitmap);
        image->bitmap = 0;
    }
}

static nserror zbrowser_lainos_image_clone(const struct content *old,
        struct content **newc) {
    const zbrowser_lainos_image_content_t *old_image = (const zbrowser_lainos_image_content_t *)old;
    zbrowser_lainos_image_content_t *image = calloc(1, sizeof(*image));
    nserror err;

    if (image == 0) {
        return NSERROR_NOMEM;
    }
    err = content__clone(old, &image->base);
    if (err != NSERROR_OK) {
        free(image);
        return err;
    }
    if (old_image->bitmap != 0) {
        int width = guit->bitmap->get_width(old_image->bitmap);
        int height = guit->bitmap->get_height(old_image->bitmap);
        int rowstride = (int)guit->bitmap->get_rowstride(old_image->bitmap);
        const uint8_t *src = guit->bitmap->get_buffer(old_image->bitmap);
        uint8_t *dst;

        image->bitmap = (struct bitmap *)guit->bitmap->create(width, height, BITMAP_CLEAR);
        dst = image->bitmap != 0 ? guit->bitmap->get_buffer(image->bitmap) : 0;
        if (dst == 0 || src == 0 || rowstride <= 0) {
            content_destroy(&image->base);
            return NSERROR_NOMEM;
        }
        for (int y = 0; y < height; ++y) {
            memcpy(dst + (size_t)y * (size_t)rowstride,
                   src + (size_t)y * (size_t)rowstride,
                   (size_t)rowstride);
        }
        guit->bitmap->set_opaque(image->bitmap, guit->bitmap->get_opaque(old_image->bitmap));
        guit->bitmap->modified(image->bitmap);
    } else if (!zbrowser_lainos_image_decode(image)) {
        content_destroy(&image->base);
        return NSERROR_UNKNOWN;
    }
    *newc = &image->base;
    return NSERROR_OK;
}

static content_type zbrowser_lainos_image_type(void) {
    return CONTENT_IMAGE;
}

static void *zbrowser_lainos_image_internal(const struct content *c, void *context) {
    (void)context;
    return ((const zbrowser_lainos_image_content_t *)c)->bitmap;
}

static bool zbrowser_lainos_image_opaque(struct content *c) {
    const zbrowser_lainos_image_content_t *image = (const zbrowser_lainos_image_content_t *)c;
    return image != 0 && image->bitmap != 0 && guit->bitmap->get_opaque(image->bitmap);
}

static const content_handler zbrowser_lainos_image_handler = {
    .create = zbrowser_lainos_image_create,
    .data_complete = zbrowser_lainos_image_complete,
    .destroy = zbrowser_lainos_image_destroy,
    .redraw = zbrowser_lainos_image_redraw,
    .clone = zbrowser_lainos_image_clone,
    .get_internal = zbrowser_lainos_image_internal,
    .type = zbrowser_lainos_image_type,
    .is_opaque = zbrowser_lainos_image_opaque
};

nserror zbrowser_lainos_image_init(void) {
    static const char *types[] = {
        "image/png",
        "image/x-png",
        "image/jpeg",
        "image/pjpeg",
        "image/gif",
        "image/bmp",
        "image/x-ms-bmp",
        "image/webp",
        "image/x-icon",
        "image/vnd.microsoft.icon",
        "image/svg+xml"
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        nserror err = content_factory_register_handler(types[i], &zbrowser_lainos_image_handler);
        if (err != NSERROR_OK) {
            return err;
        }
    }
    return NSERROR_OK;
}

static nserror zbrowser_lainos_launch_url(struct nsurl *url) {
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
    if (url == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    snprintf(zbrowser_lainos_pending_url,
             sizeof(zbrowser_lainos_pending_url),
             "%s",
             nsurl_access(url));
    zbrowser_lainos_navigation_pending = zbrowser_lainos_pending_url[0] != 0;
    return zbrowser_lainos_navigation_pending ? NSERROR_OK : NSERROR_BAD_PARAMETER;
#else
    (void)url;
    return NSERROR_NOT_IMPLEMENTED;
#endif
}

static struct gui_misc_table zbrowser_lainos_misc_table = {
    .schedule = zbrowser_lainos_schedule,
    .launch_url = zbrowser_lainos_launch_url
};

static struct gui_fetch_table zbrowser_lainos_fetch_table = {
    .filetype = zbrowser_lainos_filetype_for_url,
    .get_resource_url = zbrowser_lainos_resource_url,
    .get_resource_data = zbrowser_lainos_resource_data,
    .release_resource_data = zbrowser_lainos_release_resource_data
};

static struct gui_llcache_table zbrowser_lainos_llcache_table = {
    .initialise = zbrowser_lainos_llcache_initialise,
    .finalise = zbrowser_lainos_llcache_finalise,
    .store = zbrowser_lainos_llcache_store,
    .fetch = zbrowser_lainos_llcache_fetch,
    .release = zbrowser_lainos_llcache_release,
    .invalidate = zbrowser_lainos_llcache_invalidate
};

static struct netsurf_table zbrowser_lainos_gui_table = {
    .misc = &zbrowser_lainos_misc_table,
    .fetch = &zbrowser_lainos_fetch_table,
    .llcache = &zbrowser_lainos_llcache_table,
    .layout = (struct gui_layout_table *)&zbrowser_lainos_layout_table,
    .bitmap = &zbrowser_lainos_bitmap_table
};

struct netsurf_table *guit = &zbrowser_lainos_gui_table;

#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
nserror fetch_data_register(void) {
    return NSERROR_OK;
}

nserror fetch_file_register(void) {
    return NSERROR_OK;
}

nserror fetch_resource_register(void) {
    return NSERROR_OK;
}

nserror fetch_about_register(void) {
    return NSERROR_OK;
}
#endif

int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz;
    if (tv == 0) {
        errno = 22;
        return -1;
    }
    tv->tv_sec = time(0);
    tv->tv_usec = 0;
    return 0;
}

int atexit(void (*function)(void)) {
    (void)function;
    return 0;
}

char *getenv(const char *name) {
    (void)name;
    return 0;
}

int atoi(const char *nptr) {
    int value = 0;
    int sign = 1;

    if (nptr == 0) {
        return 0;
    }
    while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n' || *nptr == '\r') {
        ++nptr;
    }
    if (*nptr == '-') {
        sign = -1;
        ++nptr;
    } else if (*nptr == '+') {
        ++nptr;
    }
    while (*nptr >= '0' && *nptr <= '9') {
        value = value * 10 + (*nptr - '0');
        ++nptr;
    }
    return value * sign;
}

int access(const char *path, int mode) {
    (void)path;
    (void)mode;
    errno = 2;
    return -1;
}

char *realpath(const char *path, char *resolved_path) {
    (void)path;
    (void)resolved_path;
    errno = 2;
    return 0;
}

char *strerror(int errnum) {
    (void)errnum;
    return "error";
}

long long strtoll(const char *nptr, char **endptr, int base) {
    long value = strtol(nptr, endptr, base);
    return (long long)value;
}

float strtof(const char *nptr, char **endptr) {
    float value = 0.0f;
    float place = 0.1f;
    int sign = 1;
    int saw_digit = 0;

    if (nptr == 0) {
        if (endptr != 0) {
            *endptr = 0;
        }
        return 0.0f;
    }
    while (*nptr == ' ' || *nptr == '\t' || *nptr == '\n' || *nptr == '\r') {
        ++nptr;
    }
    if (*nptr == '-') {
        sign = -1;
        ++nptr;
    } else if (*nptr == '+') {
        ++nptr;
    }
    while (*nptr >= '0' && *nptr <= '9') {
        value = value * 10.0f + (float)(*nptr - '0');
        saw_digit = 1;
        ++nptr;
    }
    if (*nptr == '.') {
        ++nptr;
        while (*nptr >= '0' && *nptr <= '9') {
            value += (float)(*nptr - '0') * place;
            place *= 0.1f;
            saw_digit = 1;
            ++nptr;
        }
    }
    if (endptr != 0) {
        *endptr = (char *)nptr;
    }
    return saw_digit ? value * (float)sign : 0.0f;
}

double ceil(double value) {
    long whole = (long)value;

    if (value > (double)whole) {
        return (double)(whole + 1);
    }
    return (double)whole;
}

float ceilf(float value) {
    return (float)ceil((double)value);
}

int isascii(int c) {
    return (c & ~0x7f) == 0;
}

size_t strcspn(const char *text, const char *reject) {
    size_t count = 0;

    if (text == 0 || reject == 0) {
        return 0;
    }
    while (text[count] != '\0') {
        const char *scan = reject;
        while (*scan != '\0') {
            if (text[count] == *scan) {
                return count;
            }
            ++scan;
        }
        ++count;
    }
    return count;
}

size_t strspn(const char *text, const char *accept) {
    size_t count = 0;

    if (text == 0 || accept == 0) {
        return 0;
    }
    while (text[count] != '\0') {
        const char *scan = accept;
        int matched = 0;
        while (*scan != '\0') {
            if (text[count] == *scan) {
                matched = 1;
                break;
            }
            ++scan;
        }
        if (!matched) {
            return count;
        }
        ++count;
    }
    return count;
}

char *strtok(char *str, const char *delim) {
    static char *next;
    char *start;

    if (delim == 0) {
        return 0;
    }
    if (str != 0) {
        next = str;
    }
    if (next == 0) {
        return 0;
    }
    next += strspn(next, delim);
    if (*next == '\0') {
        next = 0;
        return 0;
    }
    start = next;
    next += strcspn(next, delim);
    if (*next != '\0') {
        *next = '\0';
        ++next;
    } else {
        next = 0;
    }
    return start;
}

FILE *fopen(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    errno = 2;
    return 0;
}

int fclose(FILE *stream) {
    (void)stream;
    return 0;
}

int feof(FILE *stream) {
    (void)stream;
    return 1;
}

int fseek(FILE *stream, long offset, int whence) {
    (void)stream;
    (void)offset;
    (void)whence;
    errno = 22;
    return -1;
}

long ftell(FILE *stream) {
    (void)stream;
    errno = 22;
    return -1;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr;
    (void)size;
    (void)nmemb;
    (void)stream;
    return 0;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

char *fgets(char *buffer, int size, FILE *stream) {
    (void)buffer;
    (void)size;
    (void)stream;
    return 0;
}

int fputc(int ch, FILE *stream) {
    (void)stream;
    return ch;
}

int fputs(const char *text, FILE *stream) {
    int count = 0;
    (void)stream;
    if (text == 0) {
        return -1;
    }
    while (text[count] != 0) {
        ++count;
    }
    return count;
}

int fprintf(FILE *stream, const char *format, ...) {
    (void)stream;
    (void)format;
    return 0;
}

int printf(const char *format, ...) {
    (void)format;
    return 0;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    (void)stream;
    (void)format;
    (void)ap;
    return 0;
}

int sscanf(const char *buffer, const char *format, ...) {
    (void)buffer;
    (void)format;
    return 0;
}

void *opendir(const char *name) {
    (void)name;
    errno = 2;
    return 0;
}

int closedir(void *dirp) {
    (void)dirp;
    return 0;
}

void *readdir(void *dirp) {
    (void)dirp;
    return 0;
}

int dirfd(void *dirp) {
    (void)dirp;
    errno = 22;
    return -1;
}

int mkdir(const char *path, mode_t mode) {
    (void)path;
    (void)mode;
    errno = 22;
    return -1;
}

int rmdir(const char *path) {
    (void)path;
    errno = 22;
    return -1;
}

long pread(int fd, void *buf, unsigned long count, long offset) {
    (void)fd;
    (void)buf;
    (void)count;
    (void)offset;
    errno = 22;
    return -1;
}

long pwrite(int fd, const void *buf, unsigned long count, long offset) {
    (void)fd;
    (void)buf;
    (void)count;
    (void)offset;
    errno = 22;
    return -1;
}

int fstatat(int dirfd_value, const char *path, struct stat *statbuf, int flags) {
    (void)dirfd_value;
    (void)path;
    (void)statbuf;
    (void)flags;
    errno = 2;
    return -1;
}

int stat(const char *path, struct stat *statbuf) {
    (void)path;
    (void)statbuf;
    errno = 2;
    return -1;
}

int unlinkat(int dirfd_value, const char *path, int flags) {
    (void)dirfd_value;
    (void)path;
    (void)flags;
    errno = 2;
    return -1;
}

int uname(struct utsname *buf) {
    uint32_t i;
    if (buf == 0) {
        errno = 22;
        return -1;
    }
    for (i = 0; i < sizeof(*buf); ++i) {
        ((uint8_t *)buf)[i] = 0;
    }
    buf->sysname[0] = 'L';
    buf->sysname[1] = 'a';
    buf->sysname[2] = 'i';
    buf->sysname[3] = 'n';
    return 0;
}

struct tm *gmtime(const time_t *timep) {
    static struct tm tm_value;
    (void)timep;
    return &tm_value;
}

iconv_t iconv_open(const char *tocode, const char *fromcode) {
    (void)tocode;
    (void)fromcode;
    errno = 22;
    return (iconv_t)-1;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft) {
    (void)cd;
    (void)inbuf;
    (void)inbytesleft;
    (void)outbuf;
    (void)outbytesleft;
    errno = 22;
    return (size_t)-1;
}

int iconv_close(iconv_t cd) {
    (void)cd;
    return 0;
}

int inflateInit2(z_stream *stream, int window_bits) {
    (void)stream;
    (void)window_bits;
    return -1;
}

int inflate(z_stream *stream, int flush) {
    (void)stream;
    (void)flush;
    return -1;
}

int inflateEnd(z_stream *stream) {
    (void)stream;
    return 0;
}

gzFile gzopen(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    return 0;
}

char *gzgets(gzFile file, char *buffer, int length) {
    (void)file;
    (void)buffer;
    (void)length;
    return 0;
}

int gzclose(gzFile file) {
    (void)file;
    return 0;
}

#ifndef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
struct bitmap *content_get_bitmap(struct hlcache_handle *handle) {
    (void)handle;
    return 0;
}

const char *content_get_encoding(struct hlcache_handle *handle,
        enum content_encoding_type op) {
    (void)handle;
    (void)op;
    return 0;
}

lwc_string *content_get_mime_type(struct hlcache_handle *handle) {
    (void)handle;
    return 0;
}

const uint8_t *content_get_source_data(struct hlcache_handle *handle,
        size_t *size) {
    (void)handle;
    if (size != 0) {
        *size = 0;
    }
    return 0;
}

const char *content_get_title(struct hlcache_handle *handle) {
    (void)handle;
    return 0;
}

nserror content_textsearch(struct hlcache_handle *handle, void *context,
        search_flags_t flags, const char *string) {
    (void)handle;
    (void)context;
    (void)flags;
    (void)string;
    return NSERROR_OK;
}

nserror content_textsearch_destroy(struct textsearch_context *textsearch) {
    (void)textsearch;
    return NSERROR_OK;
}

nserror content_textsearch_clear(struct hlcache_handle *handle) {
    (void)handle;
    return NSERROR_OK;
}

bool content_textsearch_ishighlighted(struct textsearch_context *textsearch,
        unsigned start, unsigned end, unsigned *start_idx, unsigned *end_idx) {
    (void)textsearch;
    (void)start;
    (void)end;
    if (start_idx != 0) {
        *start_idx = 0;
    }
    if (end_idx != 0) {
        *end_idx = 0;
    }
    return false;
}

struct nsurl *hlcache_handle_get_url(const struct hlcache_handle *handle) {
    (void)handle;
    return 0;
}

struct content *hlcache_handle_get_content(const struct hlcache_handle *handle) {
    (void)handle;
    return 0;
}

struct nsurl *content_get_url(struct content *content) {
    if (content == 0) {
        return 0;
    }
    if (content->llcache != 0) {
        return 0;
    }
    return zbrowser_engine_current_base_url();
}

nserror hlcache_handle_release(struct hlcache_handle *handle) {
    (void)handle;
    return NSERROR_OK;
}

nserror hlcache_handle_abort(struct hlcache_handle *handle) {
    (void)handle;
    return NSERROR_OK;
}

nserror hlcache_handle_replace_callback(struct hlcache_handle *handle,
        hlcache_handle_callback cb, void *pw) {
    if (handle != 0) {
        handle->cb = cb;
        handle->pw = pw;
    }
    return NSERROR_OK;
}

typedef struct zbrowser_lainos_fetch_buffer {
    uint8_t *data;
    size_t len;
    size_t cap;
    char content_type[96];
    bool done;
    nserror error;
} zbrowser_lainos_fetch_buffer_t;

static bool zbrowser_lainos_fetch_buffer_append(zbrowser_lainos_fetch_buffer_t *buffer,
        const uint8_t *data,
        size_t len) {
    uint8_t *next;
    size_t next_cap;

    if (buffer == 0 || data == 0 || len == 0u) {
        return true;
    }
    if (buffer->len + len < buffer->len) {
        return false;
    }
    if (buffer->len + len + 1u > buffer->cap) {
        next_cap = buffer->cap != 0u ? buffer->cap : 4096u;
        while (next_cap < buffer->len + len + 1u) {
            if (next_cap > ((size_t)-1) / 2u) {
                return false;
            }
            next_cap *= 2u;
        }
        next = realloc(buffer->data, next_cap);
        if (next == 0) {
            return false;
        }
        buffer->data = next;
        buffer->cap = next_cap;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = 0;
    return true;
}

static void zbrowser_lainos_fetch_collect_callback(const fetch_msg *msg, void *p) {
    zbrowser_lainos_fetch_buffer_t *buffer = (zbrowser_lainos_fetch_buffer_t *)p;
    static const char content_type_prefix[] = "Content-Type:";
    size_t prefix_len = sizeof(content_type_prefix) - 1u;

    if (buffer == 0 || msg == 0) {
        return;
    }
    if (msg->type == FETCH_HEADER &&
        msg->data.header_or_data.buf != 0 &&
        msg->data.header_or_data.len >= prefix_len &&
        strncasecmp((const char *)msg->data.header_or_data.buf,
                    content_type_prefix,
                    prefix_len) == 0) {
        size_t pos = prefix_len;
        size_t out = 0;
        while (pos < msg->data.header_or_data.len &&
               (msg->data.header_or_data.buf[pos] == ' ' ||
                msg->data.header_or_data.buf[pos] == '\t')) {
            ++pos;
        }
        while (pos < msg->data.header_or_data.len &&
               out + 1u < sizeof(buffer->content_type)) {
            char ch = (char)msg->data.header_or_data.buf[pos++];
            if (ch == '\r' || ch == '\n') {
                break;
            }
            buffer->content_type[out++] = ch;
        }
        buffer->content_type[out] = 0;
        return;
    }
    if (msg->type == FETCH_DATA) {
        if (!zbrowser_lainos_fetch_buffer_append(buffer,
                                                 msg->data.header_or_data.buf,
                                                 msg->data.header_or_data.len)) {
            buffer->error = NSERROR_NOMEM;
            buffer->done = true;
        }
        return;
    }
    if (msg->type == FETCH_FINISHED) {
        buffer->done = true;
        return;
    }
    if (msg->type >= FETCH_TIMEDOUT) {
        buffer->error = NSERROR_NOT_FOUND;
        buffer->done = true;
    }
}

static void zbrowser_lainos_hlcache_content_user(struct content *content,
        content_msg msg,
        const union content_msg_data *data,
        void *pw) {
    struct hlcache_handle *handle = (struct hlcache_handle *)pw;

    (void)content;
    zbrowser_lainos_hlcache_queue_event(handle, msg, data);
}

nserror hlcache_handle_retrieve(nsurl *url, uint32_t flags, nsurl *referer,
        llcache_post_data *post, hlcache_handle_callback cb, void *pw,
        hlcache_child_context *child, content_type accepted_types,
        hlcache_handle **result) {
    struct fetch *fetch = 0;
    zbrowser_lainos_fetch_buffer_t buffer;
    struct hlcache_handle *handle;
    lwc_string *mime = 0;
    const char *content_type;
    const char *charset;
    char *filtered_css = 0;
    uint32_t filtered_len = 0;
    bool quirks;
    nserror err;
    int maxfd = -1;
    fd_set read_set;
    fd_set write_set;
    fd_set error_set;

    (void)flags;
    (void)post;
    (void)accepted_types;
    if (result != 0) {
        *result = 0;
    }
    if (url == 0 || cb == 0 || result == 0) {
        return NSERROR_BAD_PARAMETER;
    }

    memset(&buffer, 0, sizeof(buffer));
    err = fetch_start(url,
                      referer,
                      zbrowser_lainos_fetch_collect_callback,
                      &buffer,
                      false,
                      0,
                      0,
                      false,
                      false,
                      0,
                      &fetch);
    if (err != NSERROR_OK) {
        free(buffer.data);
        return err;
    }
    for (unsigned int i = 0; i < 16u && !buffer.done; ++i) {
        (void)fetch_fdset(&read_set, &write_set, &error_set, &maxfd);
    }
    if (!buffer.done) {
        fetch_abort(fetch);
        fetch_free(fetch);
        free(buffer.data);
        return NSERROR_TIMEOUT;
    }
    if (buffer.error != NSERROR_OK || buffer.data == 0) {
        fetch_free(fetch);
        free(buffer.data);
        return buffer.error != NSERROR_OK ? buffer.error : NSERROR_NOT_FOUND;
    }
    fetch_free(fetch);

    handle = calloc(1, sizeof(*handle));
    if (handle == 0) {
        free(buffer.data);
        return NSERROR_NOMEM;
    }
    handle->url = nsurl_ref(url);
    handle->cb = cb;
    handle->pw = pw;
    handle->llcache.content_type = buffer.content_type[0] != 0
        ? buffer.content_type
        : "text/css; charset=utf-8";
    if (zbrowser_lainos_is_css_response(handle->llcache.content_type, nsurl_access(url))) {
        filtered_css = zbrowser_lainos_css_compat_filter((const char *)buffer.data,
                                                         (uint32_t)buffer.len,
                                                         &filtered_len);
        if (filtered_css != 0) {
            free(buffer.data);
            buffer.data = (uint8_t *)filtered_css;
            buffer.len = filtered_len;
        }
    }
    handle->llcache.url = handle->url;
    handle->llcache.source_data = buffer.data;
    handle->llcache.source_len = buffer.len;
    handle->llcache.owned_source_data = buffer.data;
    {
        static char normalised_content_type[96];
        zbrowser_lainos_normalise_content_type(handle->llcache.content_type,
                                               normalised_content_type,
                                               sizeof(normalised_content_type));
        content_type = normalised_content_type[0] != 0
            ? normalised_content_type
            : handle->llcache.content_type;
    }
    if (lwc_intern_string(content_type, strlen(content_type), &mime) != lwc_error_ok) {
        hlcache_handle_release(handle);
        return NSERROR_NOMEM;
    }
    charset = child != 0 && child->charset != 0 ? child->charset : "UTF-8";
    quirks = child != 0 ? child->quirks : false;
    handle->content = content_factory_create_content(&handle->llcache,
                                                     charset,
                                                     quirks,
                                                     mime);
    lwc_string_unref(mime);
    if (handle->content == 0) {
        hlcache_handle_release(handle);
        return NSERROR_NOMEM;
    }
    handle->llcache.content = handle->content;
    if (handle->content->handler == 0 ||
        handle->content->handler->data_complete == 0 ||
        (handle->content->handler->process_data != 0 &&
         !handle->content->handler->process_data(handle->content,
                                                 (const char *)buffer.data,
                                                 (unsigned int)buffer.len)) ||
        !handle->content->handler->data_complete(handle->content)) {
        hlcache_handle_release(handle);
        return NSERROR_UNKNOWN;
    }
    *result = handle;
    zbrowser_lainos_hlcache_queue_event(handle, CONTENT_MSG_DONE, 0);
    return NSERROR_OK;
}

content_type content_factory_type_from_mime_type(lwc_string *mime_type) {
    (void)mime_type;
    return CONTENT_NONE;
}

content_type content_get_type(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0 && handle->content->handler != 0 &&
        handle->content->handler->type != 0
        ? handle->content->handler->type()
        : CONTENT_NONE;
}

int content_get_width(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0 ? handle->content->width : 0;
}

int content_get_height(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0 ? handle->content->height : 0;
}

int content_get_available_width(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0 ? handle->content->available_width : 0;
}

bool content_get_opaque(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0 &&
        handle->content->handler != 0 &&
        handle->content->handler->is_opaque != 0 &&
        handle->content->handler->is_opaque(handle->content);
}

bool content_get_quirks(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0 && handle->content->quirks;
}

bool content_is_locked(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0 && handle->content->locked;
}

bool content_exec(struct hlcache_handle *handle, const char *src, size_t srclen) {
    (void)handle;
    (void)src;
    (void)srclen;
    return false;
}

bool content_saw_insecure_objects(struct hlcache_handle *handle) {
    (void)handle;
    return false;
}

content_status content_get_status(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0
        ? handle->content->status
        : CONTENT_STATUS_ERROR;
}

const char *content_get_status_message(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0
        ? content__get_status_message(handle->content)
        : "";
}

struct nsurl *content_get_refresh_url(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0 ? handle->content->refresh : 0;
}

bool content_can_reformat(struct hlcache_handle *handle) {
    return handle != 0 && handle->content != 0;
}

void content_reformat(struct hlcache_handle *handle, bool background,
        int width, int height) {
    if (handle != 0 && handle->content != 0) {
        content__reformat(handle->content, background, width, height);
    }
}

bool content_redraw(struct hlcache_handle *handle, struct content_redraw_data *data,
        const struct rect *clip, const struct redraw_context *ctx) {
    return handle != 0 && handle->content != 0 &&
        handle->content->handler != 0 &&
        handle->content->handler->redraw != 0 &&
        handle->content->handler->redraw(handle->content, data, clip, ctx);
}

bool content_scaled_redraw(struct hlcache_handle *handle, int width, int height,
        const struct redraw_context *ctx) {
    (void)handle;
    (void)width;
    (void)height;
    (void)ctx;
    return false;
}

void content__request_redraw(struct content *content, int x, int y,
        int width, int height) {
    union content_msg_data data;

    if (content == 0) {
        return;
    }
    memset(&data, 0, sizeof(data));
    data.redraw.x = x;
    data.redraw.y = y;
    data.redraw.width = width;
    data.redraw.height = height;
    content_broadcast(content, CONTENT_MSG_REDRAW, &data);
}

int content__get_width(struct content *content) {
    if (content == 0) {
        return 0;
    }
    return content->width;
}

int content__get_height(struct content *content) {
    if (content == 0) {
        return 0;
    }
    return content->height;
}

int content__get_available_width(struct content *content) {
    if (content == 0) {
        return 0;
    }
    return content->available_width;
}

void content__reformat(struct content *content, bool background,
        int width, int height) {
    union content_msg_data data;

    if (content == 0 ||
        (content->status != CONTENT_STATUS_READY &&
         content->status != CONTENT_STATUS_DONE)) {
        return;
    }
    content->available_width = width;
    content->available_height = height;
    if (content->handler != 0 && content->handler->reformat != 0) {
        content->locked = true;
        content->handler->reformat(content, width, height);
        content->locked = false;
        memset(&data, 0, sizeof(data));
        data.background = background;
        content_broadcast(content, CONTENT_MSG_REFORMAT, &data);
    }
}

void content_broadcast(struct content *content, content_msg msg,
        const union content_msg_data *data) {
    struct content_user *user;
    struct content_user *next;

    if (content == 0 || content->user_list == 0) {
        return;
    }
    for (user = content->user_list->next; user != 0; user = next) {
        next = user->next;
        if (user->callback != 0) {
            user->callback(content, msg, data, user->pw);
        }
    }
}

nserror content_open(struct hlcache_handle *handle, struct browser_window *bw,
        struct content *page, struct object_params *params) {
    (void)handle;
    (void)bw;
    (void)page;
    (void)params;
    return NSERROR_OK;
}

nserror content_close(struct hlcache_handle *handle) {
    (void)handle;
    return NSERROR_OK;
}

void content_set_done(struct content *content) {
    if (content == 0) {
        return;
    }
    content->status = CONTENT_STATUS_DONE;
    content->active = 0u;
    content_broadcast(content, CONTENT_MSG_DONE, 0);
}

void content_invalidate_reuse_data(struct hlcache_handle *handle) {
    (void)handle;
}
#endif

#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
typedef struct zbrowser_lainos_fetch_buffer_active {
    uint8_t *data;
    size_t len;
    size_t cap;
    char content_type[96];
    bool done;
    nserror error;
} zbrowser_lainos_fetch_buffer_active_t;

static bool zbrowser_lainos_ascii_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

static bool zbrowser_lainos_ascii_match_ci(const char *text, const char *word) {
    size_t i = 0;

    if (text == 0 || word == 0) {
        return false;
    }
    while (word[i] != 0) {
        char a = text[i];
        char b = word[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
        ++i;
    }
    return true;
}

static bool zbrowser_lainos_url_contains(const char *url, const char *needle) {
    size_t needle_len;

    if (url == 0 || needle == 0) {
        return false;
    }
    needle_len = strlen(needle);
    if (needle_len == 0u) {
        return true;
    }
    for (size_t i = 0; url[i] != 0; ++i) {
        size_t j = 0;
        while (needle[j] != 0 && url[i + j] != 0 && url[i + j] == needle[j]) {
            ++j;
        }
        if (j == needle_len) {
            return true;
        }
    }
    return false;
}

static void zbrowser_lainos_normalise_content_type(const char *in,
        char *out,
        size_t out_size) {
    size_t i = 0;
    size_t start = 0;
    size_t end;

    if (out_size == 0u) {
        return;
    }
    out[0] = 0;
    if (in == 0) {
        return;
    }
    while (in[start] == ' ' || in[start] == '\t') {
        ++start;
    }
    while (in[start + i] != 0 &&
           in[start + i] != ';' &&
           i + 1u < out_size) {
        out[i] = (char)tolower((unsigned char)in[start + i]);
        ++i;
    }
    end = i;
    while (end > 0u && (out[end - 1u] == ' ' || out[end - 1u] == '\t')) {
        --end;
    }
    out[end] = 0;
}

static bool zbrowser_lainos_extension_is(const char *dot, const char *extension) {
    size_t i = 0;

    if (dot == 0 || extension == 0) {
        return false;
    }
    while (extension[i] != 0) {
        char a = dot[i];
        char b = extension[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
        ++i;
    }
    return dot[i] == 0 || dot[i] == '?' || dot[i] == '#' || dot[i] == '&';
}

static unsigned int zbrowser_lainos_css_group_rule_kind(const char *css, size_t len, size_t at) {
    size_t i = at + 1u;

    while (i < len && zbrowser_lainos_ascii_is_space(css[i])) {
        ++i;
    }
    if (zbrowser_lainos_ascii_match_ci(css + i, "layer")) {
        return 1u;
    }
    if (zbrowser_lainos_ascii_match_ci(css + i, "supports") ||
        zbrowser_lainos_ascii_match_ci(css + i, "container")) {
        return 2u;
    }
    return 0u;
}

static size_t zbrowser_lainos_css_find_rule_body(const char *css, size_t len, size_t at) {
    size_t i = at;
    unsigned int parens = 0u;
    char quote = 0;

    while (i < len) {
        char ch = css[i];
        if (quote != 0) {
            if (ch == '\\' && i + 1u < len) {
                i += 2u;
                continue;
            }
            if (ch == quote) {
                quote = 0;
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '(') {
            ++parens;
        } else if (ch == ')' && parens != 0u) {
            --parens;
        } else if (parens == 0u && (ch == '{' || ch == ';')) {
            return i;
        }
        ++i;
    }
    return len;
}

static size_t zbrowser_lainos_css_find_matching_paren(const char *css, size_t len, size_t open) {
    size_t i = open + 1u;
    unsigned int parens = 1u;
    char quote = 0;

    while (i < len) {
        char ch = css[i];
        if (quote != 0) {
            if (ch == '\\' && i + 1u < len) {
                i += 2u;
                continue;
            }
            if (ch == quote) {
                quote = 0;
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '(') {
            ++parens;
        } else if (ch == ')' && --parens == 0u) {
            return i;
        }
        ++i;
    }
    return len;
}

static size_t zbrowser_lainos_css_find_matching_brace(const char *css, size_t len, size_t open) {
    size_t i = open + 1u;
    unsigned int braces = 1u;
    char quote = 0;

    while (i < len) {
        char ch = css[i];
        if (quote != 0) {
            if (ch == '\\' && i + 1u < len) {
                i += 2u;
                continue;
            }
            if (ch == quote) {
                quote = 0;
            }
        } else if (ch == '/' && i + 1u < len && css[i + 1u] == '*') {
            i += 2u;
            while (i + 1u < len && !(css[i] == '*' && css[i + 1u] == '/')) {
                ++i;
            }
            if (i + 1u < len) {
                i += 2u;
                continue;
            }
            return len;
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '{') {
            ++braces;
        } else if (ch == '}' && --braces == 0u) {
            return i;
        }
        ++i;
    }
    return len;
}

static size_t zbrowser_lainos_css_find_var_fallback(const char *css, size_t start, size_t end) {
    size_t i = start;
    unsigned int parens = 0u;
    char quote = 0;

    while (i < end) {
        char ch = css[i];
        if (quote != 0) {
            if (ch == '\\' && i + 1u < end) {
                i += 2u;
                continue;
            }
            if (ch == quote) {
                quote = 0;
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '(') {
            ++parens;
        } else if (ch == ')' && parens != 0u) {
            --parens;
        } else if (ch == ',' && parens == 0u) {
            return i;
        }
        ++i;
    }
    return end;
}

static bool zbrowser_lainos_ascii_equal_ci_n(const char *text, size_t len, const char *word) {
    size_t i = 0;

    if (text == 0 || word == 0) {
        return false;
    }
    while (i < len && word[i] != 0) {
        char a = text[i];
        char b = word[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b + ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
        ++i;
    }
    return i == len && word[i] == 0;
}

static bool zbrowser_lainos_ascii_ends_ci_n(const char *text, size_t len, const char *suffix) {
    size_t suffix_len;

    if (text == 0 || suffix == 0) {
        return false;
    }
    suffix_len = strlen(suffix);
    if (suffix_len > len) {
        return false;
    }
    return zbrowser_lainos_ascii_equal_ci_n(text + len - suffix_len, suffix_len, suffix);
}

static bool zbrowser_lainos_css_property_allows_var_fallback(const char *css,
        size_t len,
        size_t var_at) {
    size_t start = var_at;
    size_t colon = var_at;
    size_t name_start;
    size_t name_end;

    if (css == 0 || var_at >= len) {
        return false;
    }
    while (start > 0u) {
        char ch = css[start - 1u];
        if (ch == ';' || ch == '{' || ch == '}') {
            break;
        }
        --start;
    }
    while (start < var_at && zbrowser_lainos_ascii_is_space(css[start])) {
        ++start;
    }
    colon = start;
    while (colon < var_at && css[colon] != ':') {
        ++colon;
    }
    if (colon >= var_at || css[colon] != ':') {
        return false;
    }
    name_start = start;
    name_end = colon;
    while (name_end > name_start && zbrowser_lainos_ascii_is_space(css[name_end - 1u])) {
        --name_end;
    }
    if (name_end <= name_start) {
        return false;
    }

    return zbrowser_lainos_ascii_equal_ci_n(css + name_start, name_end - name_start, "color") ||
           zbrowser_lainos_ascii_equal_ci_n(css + name_start, name_end - name_start, "background") ||
           zbrowser_lainos_ascii_equal_ci_n(css + name_start, name_end - name_start, "fill") ||
           zbrowser_lainos_ascii_equal_ci_n(css + name_start, name_end - name_start, "stroke") ||
           zbrowser_lainos_ascii_equal_ci_n(css + name_start, name_end - name_start, "box-shadow") ||
           zbrowser_lainos_ascii_equal_ci_n(css + name_start, name_end - name_start, "text-shadow") ||
           zbrowser_lainos_ascii_equal_ci_n(css + name_start, name_end - name_start, "accent-color") ||
           zbrowser_lainos_ascii_ends_ci_n(css + name_start, name_end - name_start, "-color");
}

static bool zbrowser_lainos_is_css_response(const char *content_type, const char *url) {
    return zbrowser_lainos_url_contains(content_type, "text/css") ||
           zbrowser_lainos_url_contains(url, ".css") ||
           zbrowser_lainos_url_contains(url, "only=styles") ||
           zbrowser_lainos_url_contains(url, "type=text/css");
}

static char *zbrowser_lainos_css_compat_filter(const char *css,
        uint32_t len,
        uint32_t *out_len) {
    size_t i = 0;
    size_t out = 0;
    unsigned int depth = 0u;
    unsigned int skip_depths[16];
    unsigned int skip_count = 0u;
    uint32_t vars = 0u;
    uint32_t groups = 0u;
    size_t filtered_cap;
    char *filtered;

    if (out_len != 0) {
        *out_len = len;
    }
    if (css == 0 || len == 0u) {
        return 0;
    }
    filtered_cap = (size_t)len + 1u;
    filtered = malloc(filtered_cap);
    if (filtered == 0) {
        return 0;
    }
    while (i < len) {
        if (css[i] == '/' && i + 1u < len && css[i + 1u] == '*') {
            while (i < len) {
                filtered[out++] = css[i];
                if (css[i] == '*' && i + 1u < len && css[i + 1u] == '/') {
                    filtered[out++] = css[i + 1u];
                    i += 2u;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (css[i] == '@') {
            unsigned int group_kind = zbrowser_lainos_css_group_rule_kind(css, len, i);
            size_t body = zbrowser_lainos_css_find_rule_body(css, len, i);
            if (group_kind == 1u && body < len && css[body] == '{') {
                if (skip_count < sizeof(skip_depths) / sizeof(skip_depths[0])) {
                    skip_depths[skip_count++] = depth;
                }
                ++groups;
                i = body + 1u;
                continue;
            }
            if (group_kind == 1u && body < len && css[body] == ';') {
                ++groups;
                i = body + 1u;
                continue;
            }
            if (group_kind == 2u && body < len && css[body] == '{') {
                size_t close = zbrowser_lainos_css_find_matching_brace(css, len, body);
                ++groups;
                i = close < len ? close + 1u : len;
                continue;
            }
            if (group_kind == 2u && body < len && css[body] == ';') {
                ++groups;
                i = body + 1u;
                continue;
            }
        }
        if (skip_count != 0u && css[i] == '}' && skip_depths[skip_count - 1u] == depth) {
            --skip_count;
            ++groups;
            ++i;
            continue;
        }
        if (i + 4u < len &&
            (css[i] == 'v' || css[i] == 'V') &&
            zbrowser_lainos_ascii_match_ci(css + i, "var(")) {
            size_t close = zbrowser_lainos_css_find_matching_paren(css, len, i + 3u);
            if (close < len) {
                size_t comma = zbrowser_lainos_css_find_var_fallback(css, i + 4u, close);
                if (comma < close && zbrowser_lainos_css_property_allows_var_fallback(css, len, i)) {
                    size_t start = comma + 1u;
                    size_t end = close;
                    while (start < end && zbrowser_lainos_ascii_is_space(css[start])) {
                        ++start;
                    }
                    while (end > start && zbrowser_lainos_ascii_is_space(css[end - 1u])) {
                        --end;
                    }
                    while (start < end) {
                        filtered[out++] = css[start++];
                    }
                    ++vars;
                } else {
                    size_t start = i;
                    while (start <= close) {
                        filtered[out++] = css[start++];
                    }
                }
                i = close + 1u;
                continue;
            }
        }
        filtered[out++] = css[i];
        if (css[i] == '{') {
            ++depth;
        } else if (css[i] == '}' && depth != 0u) {
            --depth;
        }
        ++i;
    }
    filtered[out] = 0;
    if (out_len != 0) {
        *out_len = (uint32_t)out;
    }
    if (vars == 0u && groups == 0u) {
        free(filtered);
        return 0;
    }
    ++zbrowser_lainos_css_compat_count;
    zbrowser_lainos_css_compat_var_count += vars;
    zbrowser_lainos_css_compat_group_count += groups;
    return filtered;
}

#ifndef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
static bool zbrowser_lainos_fetch_buffer_active_append(
        zbrowser_lainos_fetch_buffer_active_t *buffer,
        const uint8_t *data,
        size_t len) {
    uint8_t *next;
    size_t next_cap;

    if (buffer == 0 || data == 0 || len == 0u) {
        return true;
    }
    if (buffer->len + len < buffer->len) {
        return false;
    }
    if (buffer->len + len + 1u > buffer->cap) {
        next_cap = buffer->cap != 0u ? buffer->cap : 4096u;
        while (next_cap < buffer->len + len + 1u) {
            if (next_cap > ((size_t)-1) / 2u) {
                return false;
            }
            next_cap *= 2u;
        }
        next = realloc(buffer->data, next_cap);
        if (next == 0) {
            return false;
        }
        buffer->data = next;
        buffer->cap = next_cap;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    buffer->data[buffer->len] = 0;
    return true;
}

static void zbrowser_lainos_fetch_collect_active_callback(const fetch_msg *msg, void *p) {
    zbrowser_lainos_fetch_buffer_active_t *buffer = (zbrowser_lainos_fetch_buffer_active_t *)p;
    static const char content_type_prefix[] = "Content-Type:";
    size_t prefix_len = sizeof(content_type_prefix) - 1u;

    if (buffer == 0 || msg == 0) {
        return;
    }
    if (msg->type == FETCH_HEADER &&
        msg->data.header_or_data.buf != 0 &&
        msg->data.header_or_data.len >= prefix_len &&
        strncasecmp((const char *)msg->data.header_or_data.buf,
                    content_type_prefix,
                    prefix_len) == 0) {
        size_t pos = prefix_len;
        size_t out = 0;
        while (pos < msg->data.header_or_data.len &&
               (msg->data.header_or_data.buf[pos] == ' ' ||
                msg->data.header_or_data.buf[pos] == '\t')) {
            ++pos;
        }
        while (pos < msg->data.header_or_data.len &&
               out + 1u < sizeof(buffer->content_type)) {
            char ch = (char)msg->data.header_or_data.buf[pos++];
            if (ch == '\r' || ch == '\n') {
                break;
            }
            buffer->content_type[out++] = ch;
        }
        buffer->content_type[out] = 0;
        return;
    }
    if (msg->type == FETCH_DATA) {
        if (!zbrowser_lainos_fetch_buffer_active_append(buffer,
                                                        msg->data.header_or_data.buf,
                                                        msg->data.header_or_data.len)) {
            buffer->error = NSERROR_NOMEM;
            buffer->done = true;
        }
        return;
    }
    if (msg->type == FETCH_FINISHED) {
        buffer->done = true;
        return;
    }
    if (msg->type >= FETCH_TIMEDOUT) {
        buffer->error = NSERROR_NOT_FOUND;
        buffer->done = true;
    }
}

static void zbrowser_lainos_hlcache_content_active_user(struct content *content,
        content_msg msg,
        const union content_msg_data *data,
        void *pw) {
    struct hlcache_handle *handle = (struct hlcache_handle *)pw;

    (void)content;
    zbrowser_lainos_hlcache_queue_event(handle, msg, data);
}

struct nsurl *hlcache_handle_get_url(const struct hlcache_handle *handle) {
    return handle != 0 ? handle->url : 0;
}

struct content *hlcache_handle_get_content(const struct hlcache_handle *handle) {
    return handle != 0 ? handle->content : 0;
}

nserror hlcache_handle_release(struct hlcache_handle *handle) {
    if (handle != 0) {
        if (handle->content != 0) {
            content_destroy(handle->content);
            handle->content = 0;
        }
        (void)llcache_handle_release(&handle->llcache);
        if (handle->url != 0) {
            nsurl_unref(handle->url);
        }
        free(handle);
    }
    return NSERROR_OK;
}

nserror hlcache_handle_abort(struct hlcache_handle *handle) {
    (void)handle;
    return NSERROR_OK;
}

nserror hlcache_handle_replace_callback(struct hlcache_handle *handle,
        hlcache_handle_callback cb, void *pw) {
    if (handle != 0) {
        handle->cb = cb;
        handle->pw = pw;
    }
    return NSERROR_OK;
}

nserror hlcache_handle_retrieve(nsurl *url, uint32_t flags, nsurl *referer,
        llcache_post_data *post, hlcache_handle_callback cb, void *pw,
        hlcache_child_context *child, content_type accepted_types,
        hlcache_handle **result) {
    struct fetch *fetch = 0;
    zbrowser_lainos_fetch_buffer_active_t buffer;
    struct hlcache_handle *handle;
    lwc_string *mime = 0;
    const char *content_type;
    const char *charset;
    char *filtered_css = 0;
    uint32_t filtered_len = 0;
    bool quirks;
    nserror err;
    int maxfd = -1;
    fd_set read_set;
    fd_set write_set;
    fd_set error_set;

    (void)flags;
    (void)post;
    (void)accepted_types;
    if (result != 0) {
        *result = 0;
    }
    if (url == 0 || cb == 0 || result == 0) {
        return NSERROR_BAD_PARAMETER;
    }

    memset(&buffer, 0, sizeof(buffer));
    err = fetch_start(url,
                      referer,
                      zbrowser_lainos_fetch_collect_active_callback,
                      &buffer,
                      false,
                      0,
                      0,
                      false,
                      false,
                      0,
                      &fetch);
    if (err != NSERROR_OK) {
        free(buffer.data);
        return err;
    }
    for (unsigned int i = 0; i < 16u && !buffer.done; ++i) {
        (void)fetch_fdset(&read_set, &write_set, &error_set, &maxfd);
    }
    if (!buffer.done) {
        fetch_abort(fetch);
        fetch_free(fetch);
        free(buffer.data);
        return NSERROR_TIMEOUT;
    }
    if (buffer.error != NSERROR_OK || buffer.data == 0) {
        fetch_free(fetch);
        free(buffer.data);
        return buffer.error != NSERROR_OK ? buffer.error : NSERROR_NOT_FOUND;
    }
    fetch_free(fetch);

    handle = calloc(1, sizeof(*handle));
    if (handle == 0) {
        free(buffer.data);
        return NSERROR_NOMEM;
    }
    handle->url = nsurl_ref(url);
    handle->cb = cb;
    handle->pw = pw;
    handle->llcache.content_type = buffer.content_type[0] != 0
        ? buffer.content_type
        : "text/css; charset=utf-8";
    if (zbrowser_lainos_is_css_response(handle->llcache.content_type, nsurl_access(url))) {
        filtered_css = zbrowser_lainos_css_compat_filter((const char *)buffer.data,
                                                         (uint32_t)buffer.len,
                                                         &filtered_len);
        if (filtered_css != 0) {
            free(buffer.data);
            buffer.data = (uint8_t *)filtered_css;
            buffer.len = filtered_len;
        }
    }
    handle->llcache.url = handle->url;
    handle->llcache.source_data = buffer.data;
    handle->llcache.source_len = buffer.len;
    handle->llcache.owned_source_data = buffer.data;
    {
        static char normalised_content_type[96];
        zbrowser_lainos_normalise_content_type(handle->llcache.content_type,
                                               normalised_content_type,
                                               sizeof(normalised_content_type));
        content_type = normalised_content_type[0] != 0
            ? normalised_content_type
            : handle->llcache.content_type;
    }
    if (lwc_intern_string(content_type, strlen(content_type), &mime) != lwc_error_ok) {
        hlcache_handle_release(handle);
        return NSERROR_NOMEM;
    }
    charset = child != 0 && child->charset != 0 ? child->charset : "UTF-8";
    quirks = child != 0 ? child->quirks : false;
    handle->content = content_factory_create_content(&handle->llcache,
                                                     charset,
                                                     quirks,
                                                     mime);
    lwc_string_unref(mime);
    if (handle->content == 0) {
        hlcache_handle_release(handle);
        return NSERROR_NOMEM;
    }
    handle->llcache.content = handle->content;
    if (handle->content->handler == 0 ||
        handle->content->handler->data_complete == 0 ||
        (handle->content->handler->process_data != 0 &&
         !handle->content->handler->process_data(handle->content,
                                                 (const char *)buffer.data,
                                                 (unsigned int)buffer.len)) ||
        !handle->content->handler->data_complete(handle->content)) {
        hlcache_handle_release(handle);
        return NSERROR_UNKNOWN;
    }
    *result = handle;
    zbrowser_lainos_hlcache_queue_event(handle, CONTENT_MSG_DONE, 0);
    return NSERROR_OK;
}

nserror llcache_handle_clone(llcache_handle *handle, llcache_handle **result) {
    if (result != 0) {
        *result = handle;
    }
    return NSERROR_OK;
}

nserror llcache_handle_invalidate_cache_data(llcache_handle *handle) {
    (void)handle;
    return NSERROR_OK;
}
#endif

#ifndef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
typedef struct zbrowser_lainos_fetcher {
    lwc_string *scheme;
    struct fetcher_operation_table ops;
} zbrowser_lainos_fetcher_t;

struct fetch {
    struct fetch *next;
    zbrowser_lainos_fetcher_t *fetcher;
    void *fetcher_handle;
    nsurl *url;
    fetch_callback callback;
    void *pw;
    http_response_code http_code;
    bool removed;
    bool auto_free;
};

#define ZBROWSER_LAINOS_FETCHER_MAX 8u
static zbrowser_lainos_fetcher_t zbrowser_lainos_fetchers[ZBROWSER_LAINOS_FETCHER_MAX];
static struct fetch *zbrowser_lainos_fetches;

unsigned int zbrowser_lainos_fetch_pending_count(void) {
    unsigned int count = 0;
    struct fetch *fetch = zbrowser_lainos_fetches;

    while (fetch != 0) {
        ++count;
        fetch = fetch->next;
    }
    return count;
}

static zbrowser_lainos_fetcher_t *zbrowser_lainos_fetcher_for_url(nsurl *url) {
    lwc_string *scheme;
    zbrowser_lainos_fetcher_t *result = 0;

    if (url == 0) {
        return 0;
    }
    scheme = nsurl_get_component(url, NSURL_SCHEME);
    if (scheme == 0) {
        return 0;
    }
    for (unsigned int i = 0; i < ZBROWSER_LAINOS_FETCHER_MAX; ++i) {
        if (zbrowser_lainos_fetchers[i].scheme != 0 &&
            strcmp(lwc_string_data(zbrowser_lainos_fetchers[i].scheme),
                   lwc_string_data(scheme)) == 0) {
            result = &zbrowser_lainos_fetchers[i];
            break;
        }
    }
    lwc_string_unref(scheme);
    return result;
}

nserror fetcher_add(lwc_string *scheme, const struct fetcher_operation_table *ops) {
    if (scheme == 0 || ops == 0 || ops->setup == 0 || ops->poll == 0) {
        if (scheme != 0) {
            lwc_string_unref(scheme);
        }
        return NSERROR_BAD_PARAMETER;
    }
    for (unsigned int i = 0; i < ZBROWSER_LAINOS_FETCHER_MAX; ++i) {
        if (zbrowser_lainos_fetchers[i].scheme == 0) {
            if (ops->initialise != 0 && !ops->initialise(scheme)) {
                lwc_string_unref(scheme);
                return NSERROR_INIT_FAILED;
            }
            zbrowser_lainos_fetchers[i].scheme = scheme;
            zbrowser_lainos_fetchers[i].ops = *ops;
            return NSERROR_OK;
        }
    }
    lwc_string_unref(scheme);
    return NSERROR_NOMEM;
}

nserror fetch_start(nsurl *url, nsurl *referer, fetch_callback callback,
        void *p, bool only_2xx, const char *post_urlenc,
        const struct fetch_multipart_data *post_multipart,
        bool verifiable, bool downgrade_tls, const char *headers[],
        struct fetch **fetch_out) {
    zbrowser_lainos_fetcher_t *fetcher;
    struct fetch *fetch;

    (void)referer;
    (void)verifiable;
    if (fetch_out != 0) {
        *fetch_out = 0;
    }
    if (url == 0 || callback == 0 || fetch_out == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    fetcher = zbrowser_lainos_fetcher_for_url(url);
    if (fetcher == 0) {
        return NSERROR_NO_FETCH_HANDLER;
    }
    fetch = calloc(1, sizeof(*fetch));
    if (fetch == 0) {
        return NSERROR_NOMEM;
    }
    fetch->fetcher = fetcher;
    fetch->url = nsurl_ref(url);
    fetch->callback = callback;
    fetch->pw = p;
    fetch->auto_free = callback != zbrowser_lainos_fetch_collect_active_callback;
    fetch->fetcher_handle = fetcher->ops.setup(fetch,
                                               url,
                                               only_2xx,
                                               downgrade_tls,
                                               post_urlenc,
                                               post_multipart,
                                               headers);
    if (fetch->fetcher_handle == 0) {
        nsurl_unref(fetch->url);
        free(fetch);
        return NSERROR_BAD_URL;
    }
    fetch->next = zbrowser_lainos_fetches;
    zbrowser_lainos_fetches = fetch;
    if (fetcher->ops.start != 0 && !fetcher->ops.start(fetch->fetcher_handle)) {
        fetch_remove_from_queues(fetch);
        fetch_free(fetch);
        return NSERROR_UNKNOWN;
    }
    *fetch_out = fetch;
    return NSERROR_OK;
}

void fetch_abort(struct fetch *fetch) {
    if (fetch != 0 && fetch->fetcher != 0 && fetch->fetcher->ops.abort != 0) {
        fetch->fetcher->ops.abort(fetch->fetcher_handle);
    }
}

nserror fetch_fdset(fd_set *read_fd_set,
        fd_set *write_fd_set,
        fd_set *except_fd_set,
        int *maxfd) {
    (void)read_fd_set;
    (void)write_fd_set;
    (void)except_fd_set;
    if (maxfd != 0) {
        *maxfd = -1;
    }
    for (unsigned int i = 0; i < ZBROWSER_LAINOS_FETCHER_MAX; ++i) {
        if (zbrowser_lainos_fetchers[i].scheme != 0 &&
            zbrowser_lainos_fetchers[i].ops.poll != 0) {
            zbrowser_lainos_fetchers[i].ops.poll(zbrowser_lainos_fetchers[i].scheme);
        }
    }
    return NSERROR_OK;
}

unsigned int zbrowser_lainos_pump_fetchers(void) {
    fd_set read_set;
    fd_set write_set;
    fd_set error_set;
    int maxfd = -1;
    unsigned int before = zbrowser_lainos_fetch_pending_count();

    (void)fetch_fdset(&read_set, &write_set, &error_set, &maxfd);
    return before != zbrowser_lainos_fetch_pending_count() ? 1u : 0u;
}

void fetch_send_callback(const fetch_msg *msg, struct fetch *fetch) {
    if (fetch != 0 && fetch->callback != 0 && msg != 0) {
        fetch->callback(msg, fetch->pw);
    }
}

void fetch_remove_from_queues(struct fetch *fetch) {
    struct fetch **slot = &zbrowser_lainos_fetches;

    while (*slot != 0) {
        if (*slot == fetch) {
            *slot = fetch->next;
            fetch->removed = true;
            return;
        }
        slot = &(*slot)->next;
    }
}

void fetch_free(struct fetch *fetch) {
    if (fetch == 0) {
        return;
    }
    if (!fetch->removed) {
        fetch_remove_from_queues(fetch);
    }
    if (fetch->fetcher != 0 && fetch->fetcher->ops.free != 0) {
        fetch->fetcher->ops.free(fetch->fetcher_handle);
    }
    if (fetch->url != 0) {
        nsurl_unref(fetch->url);
    }
    free(fetch);
}

nserror fetch_set_http_code(struct fetch *fetch, http_response_code http_code) {
    if (fetch != 0) {
        fetch->http_code = http_code;
    }
    return NSERROR_OK;
}

http_response_code fetch_http_code(struct fetch *fetch) {
    return fetch != 0 ? fetch->http_code : 0;
}

bool fetch_can_fetch(const nsurl *url) {
    return zbrowser_lainos_fetcher_for_url((nsurl *)url) != 0;
}

void fetch_change_callback(struct fetch *fetch, fetch_callback callback, void *p) {
    if (fetch != 0) {
        fetch->callback = callback;
        fetch->pw = p;
    }
}
#else
static unsigned int zbrowser_lainos_real_fetch_pending_count;

unsigned int zbrowser_lainos_fetch_pending_count(void) {
    return fetch_active_count() + fetch_queued_count();
}

unsigned int zbrowser_lainos_pump_fetchers(void) {
    fd_set read_set;
    fd_set write_set;
    fd_set error_set;
    int maxfd = -1;
    unsigned int before = zbrowser_lainos_fetch_pending_count() +
                          zbrowser_lainos_real_fetch_pending_count;

    (void)fetch_fdset(&read_set, &write_set, &error_set, &maxfd);
    return before != 0u ||
           zbrowser_lainos_fetch_pending_count() != 0u ||
           zbrowser_lainos_real_fetch_pending_count != 0u ||
           maxfd >= 0 ? 1u : 0u;
}
#endif

static struct nsurl *zbrowser_lainos_resource_url(const char *path) {
    (void)path;
    return 0;
}

static nserror zbrowser_lainos_resource_data(const char *path,
        const uint8_t **data,
        size_t *data_len) {
    if (data == 0 || data_len == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    if (path != 0 && strcmp(path, "default.css") == 0) {
        *data = netsurf_resource_default_css;
        *data_len = netsurf_resource_default_css_len;
        return NSERROR_OK;
    }
    if (path != 0 && strcmp(path, "quirks.css") == 0) {
        *data = netsurf_resource_quirks_css;
        *data_len = netsurf_resource_quirks_css_len;
        return NSERROR_OK;
    }
    if (path != 0 &&
        (strcmp(path, "user.css") == 0 ||
         strcmp(path, "adblock.css") == 0)) {
        *data = (const uint8_t *)"";
        *data_len = 0u;
        return NSERROR_OK;
    }
    *data = 0;
    *data_len = 0u;
    return NSERROR_NOT_FOUND;
}

static nserror zbrowser_lainos_release_resource_data(const uint8_t *data) {
    (void)data;
    return NSERROR_OK;
}

static nserror zbrowser_lainos_llcache_initialise(
        const struct llcache_store_parameters *parameters) {
    (void)parameters;
    return NSERROR_OK;
}

static nserror zbrowser_lainos_llcache_finalise(void) {
    return NSERROR_OK;
}

static nserror zbrowser_lainos_llcache_store(struct nsurl *url,
        enum backing_store_flags flags,
        uint8_t *data,
        const size_t datalen) {
    (void)url;
    (void)flags;
    (void)data;
    (void)datalen;
    return NSERROR_SAVE_FAILED;
}

static nserror zbrowser_lainos_llcache_fetch(struct nsurl *url,
        enum backing_store_flags flags,
        uint8_t **data_out,
        size_t *datalen_out) {
    (void)url;
    (void)flags;
    if (data_out != 0) {
        *data_out = 0;
    }
    if (datalen_out != 0) {
        *datalen_out = 0u;
    }
    return NSERROR_NOT_FOUND;
}

static nserror zbrowser_lainos_llcache_release(struct nsurl *url,
        enum backing_store_flags flags) {
    (void)url;
    (void)flags;
    return NSERROR_NOT_FOUND;
}

static nserror zbrowser_lainos_llcache_invalidate(struct nsurl *url) {
    (void)url;
    return NSERROR_NOT_FOUND;
}

typedef struct zbrowser_lainos_resource_fetch {
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    struct zbrowser_lainos_resource_fetch *next;
#endif
    struct fetch *fetch;
    const uint8_t *data;
    size_t len;
    const char *content_type;
    bool sent;
    bool aborted;
} zbrowser_lainos_resource_fetch_t;

static const uint8_t zbrowser_lainos_empty_css[] = "";
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
static zbrowser_lainos_resource_fetch_t *zbrowser_lainos_resource_fetches;
#endif

static bool zbrowser_lainos_resource_lookup(nsurl *url,
        const uint8_t **data,
        size_t *len,
        const char **content_type) {
    lwc_string *path;
    const char *name;
    bool found = false;

    if (url == 0 || data == 0 || len == 0 || content_type == 0) {
        return false;
    }
    path = nsurl_get_component(url, NSURL_PATH);
    if (path == 0) {
        return false;
    }
    name = lwc_string_data(path);
    if (strcmp(name, "default.css") == 0) {
        *data = netsurf_resource_default_css;
        *len = netsurf_resource_default_css_len;
        *content_type = "text/css; charset=utf-8";
        found = true;
    } else if (strcmp(name, "quirks.css") == 0) {
        *data = netsurf_resource_quirks_css;
        *len = netsurf_resource_quirks_css_len;
        *content_type = "text/css; charset=utf-8";
        found = true;
    } else if (strcmp(name, "adblock.css") == 0 ||
               strcmp(name, "user.css") == 0 ||
               strcmp(name, "internal.css") == 0) {
        *data = zbrowser_lainos_empty_css;
        *len = 0u;
        *content_type = "text/css; charset=utf-8";
        found = true;
    }
    lwc_string_unref(path);
    return found;
}

static bool zbrowser_lainos_resource_initialise(lwc_string *scheme) {
    (void)scheme;
    return true;
}

static bool zbrowser_lainos_resource_acceptable(const nsurl *url) {
    const uint8_t *data = 0;
    size_t len = 0;
    const char *content_type = 0;
    return zbrowser_lainos_resource_lookup((nsurl *)url, &data, &len, &content_type);
}

static void *zbrowser_lainos_resource_setup(struct fetch *parent_fetch,
        nsurl *url,
        bool only_2xx,
        bool downgrade_tls,
        const char *post_urlenc,
        const struct fetch_multipart_data *post_multipart,
        const char **headers) {
    zbrowser_lainos_resource_fetch_t *resource;

    (void)only_2xx;
    (void)downgrade_tls;
    (void)post_urlenc;
    (void)post_multipart;
    (void)headers;
    resource = calloc(1, sizeof(*resource));
    if (resource == 0) {
        return 0;
    }
    resource->fetch = parent_fetch;
    if (!zbrowser_lainos_resource_lookup(url,
                                         &resource->data,
                                         &resource->len,
                                         &resource->content_type)) {
        free(resource);
        return 0;
    }
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    resource->next = zbrowser_lainos_resource_fetches;
    zbrowser_lainos_resource_fetches = resource;
    ++zbrowser_lainos_real_fetch_pending_count;
#endif
    return resource;
}

static bool zbrowser_lainos_resource_start(void *fetch) {
    (void)fetch;
    return true;
}

static void zbrowser_lainos_resource_abort(void *fetch) {
    zbrowser_lainos_resource_fetch_t *resource =
        (zbrowser_lainos_resource_fetch_t *)fetch;
    if (resource != 0) {
        resource->aborted = true;
    }
}

static void zbrowser_lainos_resource_free(void *fetch) {
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    zbrowser_lainos_resource_fetch_t *resource =
        (zbrowser_lainos_resource_fetch_t *)fetch;
    zbrowser_lainos_resource_fetch_t **slot = &zbrowser_lainos_resource_fetches;
    while (*slot != 0) {
        if (*slot == resource) {
            *slot = resource->next;
            if (zbrowser_lainos_real_fetch_pending_count != 0u) {
                --zbrowser_lainos_real_fetch_pending_count;
            }
            break;
        }
        slot = &(*slot)->next;
    }
#endif
    free(fetch);
}

static void zbrowser_lainos_resource_send_header(
        zbrowser_lainos_resource_fetch_t *resource,
        const char *header,
        size_t len) {
    fetch_msg msg;

    memset(&msg, 0, sizeof(msg));
    msg.type = FETCH_HEADER;
    msg.data.header_or_data.buf = (const uint8_t *)header;
    msg.data.header_or_data.len = len;
    fetch_send_callback(&msg, resource->fetch);
}

static void zbrowser_lainos_resource_poll(lwc_string *scheme) {
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    zbrowser_lainos_resource_fetch_t *resource;

    (void)scheme;
    resource = zbrowser_lainos_resource_fetches;
    while (resource != 0) {
        zbrowser_lainos_resource_fetch_t *next = resource->next;
        if (!resource->sent && !resource->aborted) {
            fetch_msg msg;
            char length_header[64];
            int length_len;

            resource->sent = true;
            (void)fetch_set_http_code(resource->fetch, 200);
            zbrowser_lainos_resource_send_header(resource,
                                                 "Content-Type: text/css; charset=utf-8",
                                                 strlen("Content-Type: text/css; charset=utf-8"));
            length_len = snprintf(length_header,
                                  sizeof(length_header),
                                  "Content-Length: %u",
                                  (unsigned int)resource->len);
            if (length_len > 0 && length_len < (int)sizeof(length_header)) {
                zbrowser_lainos_resource_send_header(resource,
                                                     length_header,
                                                     (size_t)length_len);
            }
            memset(&msg, 0, sizeof(msg));
            msg.type = FETCH_DATA;
            msg.data.header_or_data.buf = resource->data;
            msg.data.header_or_data.len = resource->len;
            fetch_send_callback(&msg, resource->fetch);
            memset(&msg, 0, sizeof(msg));
            msg.type = FETCH_FINISHED;
            fetch_send_callback(&msg, resource->fetch);
            fetch_remove_from_queues(resource->fetch);
            fetch_free(resource->fetch);
        }
        resource = next;
    }
#else
    struct fetch *fetch;

    (void)scheme;
    fetch = zbrowser_lainos_fetches;
    while (fetch != 0) {
        struct fetch *next = fetch->next;
        zbrowser_lainos_resource_fetch_t *resource =
            (zbrowser_lainos_resource_fetch_t *)fetch->fetcher_handle;
        if (fetch->fetcher != 0 &&
            fetch->fetcher->ops.poll == zbrowser_lainos_resource_poll &&
            resource != 0 &&
            !resource->sent &&
            !resource->aborted) {
            fetch_msg msg;
            char length_header[64];
            int length_len;

            resource->sent = true;
            (void)fetch_set_http_code(fetch, 200);
            zbrowser_lainos_resource_send_header(resource,
                                                 "Content-Type: text/css; charset=utf-8",
                                                 strlen("Content-Type: text/css; charset=utf-8"));
            length_len = snprintf(length_header,
                                  sizeof(length_header),
                                  "Content-Length: %u",
                                  (unsigned int)resource->len);
            if (length_len > 0 && length_len < (int)sizeof(length_header)) {
                zbrowser_lainos_resource_send_header(resource,
                                                     length_header,
                                                     (size_t)length_len);
            }
            memset(&msg, 0, sizeof(msg));
            msg.type = FETCH_DATA;
            msg.data.header_or_data.buf = resource->data;
            msg.data.header_or_data.len = resource->len;
            fetch_send_callback(&msg, fetch);
            memset(&msg, 0, sizeof(msg));
            msg.type = FETCH_FINISHED;
            fetch_send_callback(&msg, fetch);
            if (fetch->auto_free) {
                fetch_remove_from_queues(fetch);
                fetch_free(fetch);
            }
        }
        fetch = next;
    }
#endif
}

nserror zbrowser_lainos_resource_fetcher_register(void) {
    lwc_string *scheme = 0;
    const struct fetcher_operation_table ops = {
        .initialise = zbrowser_lainos_resource_initialise,
        .acceptable = zbrowser_lainos_resource_acceptable,
        .setup = zbrowser_lainos_resource_setup,
        .start = zbrowser_lainos_resource_start,
        .abort = zbrowser_lainos_resource_abort,
        .free = zbrowser_lainos_resource_free,
        .poll = zbrowser_lainos_resource_poll,
        .finalise = 0
    };

    if (lwc_intern_string("resource", 8, &scheme) != lwc_error_ok) {
        return NSERROR_NOMEM;
    }
    return fetcher_add(scheme, &ops);
}

typedef struct zbrowser_lainos_http_fetch {
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    struct zbrowser_lainos_http_fetch *next;
#endif
    struct fetch *fetch;
    nsurl *url;
    bool cacheable;
    bool sent;
    bool aborted;
} zbrowser_lainos_http_fetch_t;

#define ZBROWSER_LAINOS_HTTP_MAX_BYTES (8u * 1024u * 1024u)
#define ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS 96u
#define ZBROWSER_LAINOS_RESOURCE_CACHE_MAX_OBJECT ZBROWSER_LAINOS_HTTP_MAX_BYTES
#define ZBROWSER_LAINOS_RESOURCE_CACHE_MAX_TOTAL (16u * 1024u * 1024u)

typedef struct zbrowser_lainos_resource_cache_entry {
    char *url;
    char *content_type;
    uint8_t *data;
    uint32_t size;
    uint32_t age;
} zbrowser_lainos_resource_cache_entry_t;

static zbrowser_lainos_resource_cache_entry_t zbrowser_lainos_resource_cache[ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS];
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
static zbrowser_lainos_http_fetch_t *zbrowser_lainos_http_fetches;
#endif
static uint32_t zbrowser_lainos_resource_cache_age;
static uint32_t zbrowser_lainos_resource_cache_total;

static char *zbrowser_lainos_strdup(const char *text) {
    size_t len;
    char *copy;

    if (text == 0) {
        return 0;
    }
    len = strlen(text);
    copy = malloc(len + 1u);
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, len + 1u);
    return copy;
}

static bool zbrowser_lainos_resource_cache_allowed(const char *url,
        const char *content_type,
        uint32_t size) {
    if (url == 0 || content_type == 0 || size == 0u ||
        size > ZBROWSER_LAINOS_RESOURCE_CACHE_MAX_OBJECT) {
        return false;
    }
    return zbrowser_lainos_is_css_response(content_type, url) ||
           zbrowser_lainos_url_contains(content_type, "javascript") ||
           zbrowser_lainos_url_contains(content_type, "ecmascript") ||
           zbrowser_lainos_url_contains(url, ".js") ||
           strncmp(content_type, "image/", 6u) == 0;
}

static void zbrowser_lainos_resource_cache_drop(unsigned int index) {
    if (index >= ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS) {
        return;
    }
    if (zbrowser_lainos_resource_cache[index].url != 0) {
        ++zbrowser_lainos_resource_cache_evictions;
    }
    free(zbrowser_lainos_resource_cache[index].url);
    free(zbrowser_lainos_resource_cache[index].content_type);
    free(zbrowser_lainos_resource_cache[index].data);
    if (zbrowser_lainos_resource_cache_total >= zbrowser_lainos_resource_cache[index].size) {
        zbrowser_lainos_resource_cache_total -= zbrowser_lainos_resource_cache[index].size;
    } else {
        zbrowser_lainos_resource_cache_total = 0;
    }
    memset(&zbrowser_lainos_resource_cache[index],
           0,
           sizeof(zbrowser_lainos_resource_cache[index]));
}

static zbrowser_lainos_resource_cache_entry_t *zbrowser_lainos_resource_cache_find(
        const char *url) {
    if (url == 0) {
        return 0;
    }
    for (unsigned int i = 0; i < ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS; ++i) {
        if (zbrowser_lainos_resource_cache[i].url != 0 &&
            strcmp(zbrowser_lainos_resource_cache[i].url, url) == 0) {
            if (++zbrowser_lainos_resource_cache_age == 0u) {
                zbrowser_lainos_resource_cache_age = 1u;
            }
            zbrowser_lainos_resource_cache[i].age = zbrowser_lainos_resource_cache_age;
            return &zbrowser_lainos_resource_cache[i];
        }
    }
    return 0;
}

static void zbrowser_lainos_resource_cache_store(const char *url,
        const char *content_type,
        const uint8_t *data,
        uint32_t size) {
    unsigned int slot = ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS;
    uint32_t oldest_age = 0xffffffffu;
    char *url_copy;
    char *type_copy;
    uint8_t *data_copy;

    if (!zbrowser_lainos_resource_cache_allowed(url, content_type, size) || data == 0) {
        return;
    }
    for (unsigned int i = 0; i < ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS; ++i) {
        if (zbrowser_lainos_resource_cache[i].url != 0 &&
            strcmp(zbrowser_lainos_resource_cache[i].url, url) == 0) {
            zbrowser_lainos_resource_cache_drop(i);
            slot = i;
            break;
        }
        if (slot == ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS &&
            zbrowser_lainos_resource_cache[i].url == 0) {
            slot = i;
        }
        if (zbrowser_lainos_resource_cache[i].url != 0 &&
            zbrowser_lainos_resource_cache[i].age < oldest_age) {
            oldest_age = zbrowser_lainos_resource_cache[i].age;
        }
    }
    while (zbrowser_lainos_resource_cache_total + size > ZBROWSER_LAINOS_RESOURCE_CACHE_MAX_TOTAL) {
        unsigned int evict = ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS;
        oldest_age = 0xffffffffu;
        for (unsigned int i = 0; i < ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS; ++i) {
            if (zbrowser_lainos_resource_cache[i].url != 0 &&
                zbrowser_lainos_resource_cache[i].age < oldest_age) {
                oldest_age = zbrowser_lainos_resource_cache[i].age;
                evict = i;
            }
        }
        if (evict == ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS) {
            break;
        }
        if (slot == evict) {
            slot = ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS;
        }
        zbrowser_lainos_resource_cache_drop(evict);
    }
    if (slot == ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS) {
        for (unsigned int i = 0; i < ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS; ++i) {
            if (zbrowser_lainos_resource_cache[i].url != 0 &&
                zbrowser_lainos_resource_cache[i].age == oldest_age) {
                slot = i;
                break;
            }
        }
        if (slot != ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS) {
            zbrowser_lainos_resource_cache_drop(slot);
        }
    }
    if (slot == ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS) {
        return;
    }
    url_copy = zbrowser_lainos_strdup(url);
    type_copy = zbrowser_lainos_strdup(content_type);
    data_copy = malloc(size);
    if (url_copy == 0 || type_copy == 0 || data_copy == 0) {
        free(url_copy);
        free(type_copy);
        free(data_copy);
        return;
    }
    memcpy(data_copy, data, size);
    if (++zbrowser_lainos_resource_cache_age == 0u) {
        zbrowser_lainos_resource_cache_age = 1u;
    }
    zbrowser_lainos_resource_cache[slot].url = url_copy;
    zbrowser_lainos_resource_cache[slot].content_type = type_copy;
    zbrowser_lainos_resource_cache[slot].data = data_copy;
    zbrowser_lainos_resource_cache[slot].size = size;
    zbrowser_lainos_resource_cache[slot].age = zbrowser_lainos_resource_cache_age;
    zbrowser_lainos_resource_cache_total += size;
    ++zbrowser_lainos_resource_cache_stores;
}

void zbrowser_lainos_net_stats_reset(void) {
    zbrowser_lainos_http_fetch_count = 0;
    zbrowser_lainos_http_fetch_success_count = 0;
    zbrowser_lainos_http_fetch_error_count = 0;
    zbrowser_lainos_http_last_status = 0;
    zbrowser_lainos_http_last_bytes = 0;
    zbrowser_lainos_http_last_error = 0;
    zbrowser_lainos_resource_cache_hits = 0;
    zbrowser_lainos_resource_cache_stores = 0;
    zbrowser_lainos_resource_cache_evictions = 0;
    zbrowser_lainos_css_compat_count = 0;
    zbrowser_lainos_css_compat_var_count = 0;
    zbrowser_lainos_css_compat_group_count = 0;
    zbrowser_lainos_image_decode_count = 0;
    zbrowser_lainos_image_fallback_count = 0;
    zbrowser_lainos_image_error_count = 0;
}

uint32_t zbrowser_lainos_net_http_fetches(void) {
    return zbrowser_lainos_http_fetch_count;
}

uint32_t zbrowser_lainos_net_http_successes(void) {
    return zbrowser_lainos_http_fetch_success_count;
}

uint32_t zbrowser_lainos_net_http_errors(void) {
    return zbrowser_lainos_http_fetch_error_count;
}

uint32_t zbrowser_lainos_net_http_last_status(void) {
    return zbrowser_lainos_http_last_status;
}

uint32_t zbrowser_lainos_net_http_last_bytes(void) {
    return zbrowser_lainos_http_last_bytes;
}

int32_t zbrowser_lainos_net_http_last_error(void) {
    return zbrowser_lainos_http_last_error;
}

uint32_t zbrowser_lainos_net_cache_hits(void) {
    return zbrowser_lainos_resource_cache_hits;
}

uint32_t zbrowser_lainos_net_cache_entries(void) {
    uint32_t count = 0;

    for (unsigned int i = 0; i < ZBROWSER_LAINOS_RESOURCE_CACHE_SLOTS; ++i) {
        if (zbrowser_lainos_resource_cache[i].url != 0) {
            ++count;
        }
    }
    return count;
}

uint32_t zbrowser_lainos_net_cache_kib(void) {
    return zbrowser_lainos_resource_cache_total / 1024u;
}

uint32_t zbrowser_lainos_net_cache_stores(void) {
    return zbrowser_lainos_resource_cache_stores;
}

uint32_t zbrowser_lainos_net_cache_evictions(void) {
    return zbrowser_lainos_resource_cache_evictions;
}

uint32_t zbrowser_lainos_net_css_compat_transforms(void) {
    return zbrowser_lainos_css_compat_count;
}

uint32_t zbrowser_lainos_net_css_compat_vars(void) {
    return zbrowser_lainos_css_compat_var_count;
}

uint32_t zbrowser_lainos_net_css_compat_groups(void) {
    return zbrowser_lainos_css_compat_group_count;
}

uint32_t zbrowser_lainos_net_image_decodes(void) {
    return zbrowser_lainos_image_decode_count;
}

uint32_t zbrowser_lainos_net_image_fallbacks(void) {
    return zbrowser_lainos_image_fallback_count;
}

uint32_t zbrowser_lainos_net_image_errors(void) {
    return zbrowser_lainos_image_error_count;
}

static const char *zbrowser_lainos_filetype_for_url(const char *url) {
    const char *scan = url != 0 ? url : "";
    const char *dot = 0;

    if (zbrowser_lainos_url_contains(url, "only=styles") ||
        zbrowser_lainos_url_contains(url, "type=text/css")) {
        return "text/css";
    }
    while (*scan != 0 && *scan != '?' && *scan != '#') {
        if (*scan == '.') {
            dot = scan;
        } else if (*scan == '/') {
            dot = 0;
        }
        ++scan;
    }
    if (zbrowser_lainos_extension_is(dot, ".css")) {
        return "text/css";
    }
    if (zbrowser_lainos_extension_is(dot, ".html") ||
        zbrowser_lainos_extension_is(dot, ".htm")) {
        return "text/html";
    }
    if (zbrowser_lainos_extension_is(dot, ".png")) {
        return "image/png";
    }
    if (zbrowser_lainos_extension_is(dot, ".jpg") ||
        zbrowser_lainos_extension_is(dot, ".jpeg")) {
        return "image/jpeg";
    }
    if (zbrowser_lainos_extension_is(dot, ".gif")) {
        return "image/gif";
    }
    if (zbrowser_lainos_extension_is(dot, ".bmp")) {
        return "image/bmp";
    }
    if (zbrowser_lainos_extension_is(dot, ".webp")) {
        return "image/webp";
    }
    if (zbrowser_lainos_extension_is(dot, ".ico")) {
        return "image/x-icon";
    }
    if (zbrowser_lainos_extension_is(dot, ".svg")) {
        return "image/svg+xml";
    }
    return "text/plain";
}

static bool zbrowser_lainos_http_initialise(lwc_string *scheme) {
    (void)scheme;
    return true;
}

static bool zbrowser_lainos_http_acceptable(const nsurl *url) {
    (void)url;
    return true;
}

static void *zbrowser_lainos_http_setup(struct fetch *parent_fetch,
        nsurl *url,
        bool only_2xx,
        bool downgrade_tls,
        const char *post_urlenc,
        const struct fetch_multipart_data *post_multipart,
        const char **headers) {
    zbrowser_lainos_http_fetch_t *http;

    (void)only_2xx;
    (void)downgrade_tls;
    (void)headers;
    http = calloc(1, sizeof(*http));
    if (http == 0) {
        return 0;
    }
    http->fetch = parent_fetch;
    http->url = nsurl_ref(url);
    http->cacheable = post_urlenc == 0 && post_multipart == 0;
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    {
        zbrowser_lainos_http_fetch_t **slot = &zbrowser_lainos_http_fetches;
        while (*slot != 0) {
            slot = &(*slot)->next;
        }
        *slot = http;
    }
    ++zbrowser_lainos_real_fetch_pending_count;
#endif
    return http;
}

static bool zbrowser_lainos_http_start(void *fetch) {
    (void)fetch;
    return true;
}

static void zbrowser_lainos_http_abort(void *fetch) {
    zbrowser_lainos_http_fetch_t *http = (zbrowser_lainos_http_fetch_t *)fetch;
    if (http != 0) {
        http->aborted = true;
    }
}

static void zbrowser_lainos_http_free(void *fetch) {
    zbrowser_lainos_http_fetch_t *http = (zbrowser_lainos_http_fetch_t *)fetch;
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    zbrowser_lainos_http_fetch_t **slot = &zbrowser_lainos_http_fetches;
    while (*slot != 0) {
        if (*slot == http) {
            *slot = http->next;
            if (zbrowser_lainos_real_fetch_pending_count != 0u) {
                --zbrowser_lainos_real_fetch_pending_count;
            }
            break;
        }
        slot = &(*slot)->next;
    }
#endif
    if (http != 0) {
        if (http->url != 0) {
            nsurl_unref(http->url);
        }
        free(http);
    }
}

static void zbrowser_lainos_http_send_header(zbrowser_lainos_http_fetch_t *http,
        const char *header,
        size_t len) {
    fetch_msg msg;

    memset(&msg, 0, sizeof(msg));
    msg.type = FETCH_HEADER;
    msg.data.header_or_data.buf = (const uint8_t *)header;
    msg.data.header_or_data.len = len;
    fetch_send_callback(&msg, http->fetch);
}

#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
static unsigned int zbrowser_lainos_http_priority(const zbrowser_lainos_http_fetch_t *http) {
    const char *url;

    if (http == 0 || http->url == 0 || http->sent || http->aborted) {
        return 0u;
    }
    url = nsurl_access(http->url);
    if (zbrowser_lainos_url_contains(url, ".css") ||
        zbrowser_lainos_url_contains(url, "only=styles") ||
        zbrowser_lainos_url_contains(url, "type=text/css") ||
        zbrowser_lainos_url_contains(url, "styles")) {
        return 100u;
    }
    if (zbrowser_lainos_url_contains(url, ".js") ||
        zbrowser_lainos_url_contains(url, "javascript") ||
        zbrowser_lainos_url_contains(url, "load.php")) {
        return 80u;
    }
    if (zbrowser_lainos_url_contains(url, "favicon") ||
        zbrowser_lainos_url_contains(url, "apple-touch-icon")) {
        return 10u;
    }
    if (zbrowser_lainos_url_contains(url, ".png") ||
        zbrowser_lainos_url_contains(url, ".jpg") ||
        zbrowser_lainos_url_contains(url, ".jpeg") ||
        zbrowser_lainos_url_contains(url, ".gif") ||
        zbrowser_lainos_url_contains(url, ".webp") ||
        zbrowser_lainos_url_contains(url, ".svg") ||
        zbrowser_lainos_url_contains(url, ".ico")) {
        return 30u;
    }
    return 20u;
}

static zbrowser_lainos_http_fetch_t *zbrowser_lainos_http_select_fetch(void) {
    zbrowser_lainos_http_fetch_t *http = zbrowser_lainos_http_fetches;
    zbrowser_lainos_http_fetch_t *best = 0;
    unsigned int best_priority = 0u;

    while (http != 0) {
        unsigned int priority = zbrowser_lainos_http_priority(http);
        if (priority > best_priority) {
            best = http;
            best_priority = priority;
        }
        http = http->next;
    }
    return best;
}
#endif

static void zbrowser_lainos_http_poll(lwc_string *scheme) {
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    zbrowser_lainos_http_fetch_t *http;

    (void)scheme;
    http = zbrowser_lainos_http_select_fetch();
    if (http != 0) {
        zbrowser_lainos_http_fetch_t *next = http->next;
        if (!http->sent && !http->aborted) {
            fetch_msg msg;
            zbrowser_lainos_http_info_t info;
            char *buffer;
            char *payload;
            char *filtered;
            const char *content_type;
            const char *url;
            uint32_t body_size;
            uint32_t payload_size;
            uint32_t filtered_len;
            char header[128];
            int header_len;
            int rc;

            http->sent = true;
            url = nsurl_access(http->url);
            {
                zbrowser_lainos_resource_cache_entry_t *entry =
                    zbrowser_lainos_resource_cache_find(url);
                if (http->cacheable && entry != 0 && entry->data != 0 && entry->content_type != 0) {
                    ++zbrowser_lainos_resource_cache_hits;
                    ++zbrowser_lainos_http_fetch_success_count;
                    zbrowser_lainos_http_last_error = 0;
                    zbrowser_lainos_http_last_status = 200;
                    zbrowser_lainos_http_last_bytes = entry->size;
                    (void)fetch_set_http_code(http->fetch, 200);
                    header_len = snprintf(header,
                                          sizeof(header),
                                          "Content-Type: %s",
                                          entry->content_type);
                    if (header_len > 0 && header_len < (int)sizeof(header)) {
                        zbrowser_lainos_http_send_header(http, header, (size_t)header_len);
                    }
                    header_len = snprintf(header,
                                          sizeof(header),
                                          "Content-Length: %u",
                                          entry->size);
                    if (!http->aborted && header_len > 0 && header_len < (int)sizeof(header)) {
                        zbrowser_lainos_http_send_header(http, header, (size_t)header_len);
                    }
                    if (!http->aborted) {
                        memset(&msg, 0, sizeof(msg));
                        msg.type = FETCH_DATA;
                        msg.data.header_or_data.buf = entry->data;
                        msg.data.header_or_data.len = entry->size;
                        fetch_send_callback(&msg, http->fetch);
                    }
                    if (!http->aborted) {
                        memset(&msg, 0, sizeof(msg));
                        msg.type = FETCH_FINISHED;
                        fetch_send_callback(&msg, http->fetch);
                    }
                    fetch_remove_from_queues(http->fetch);
                    fetch_free(http->fetch);
                    (void)next;
                    return;
                }
            }
            buffer = malloc(ZBROWSER_LAINOS_HTTP_MAX_BYTES + 1u);
            if (buffer == 0) {
                ++zbrowser_lainos_http_fetch_error_count;
                zbrowser_lainos_http_last_error = -11;
                memset(&msg, 0, sizeof(msg));
                msg.type = FETCH_ERROR;
                msg.data.error = "HTTP fetch allocation failed";
                fetch_send_callback(&msg, http->fetch);
                fetch_remove_from_queues(http->fetch);
                fetch_free(http->fetch);
                (void)next;
                return;
            }
            memset(&info, 0, sizeof(info));
            ++zbrowser_lainos_http_fetch_count;
            rc = os_http_get_ex(url, buffer, ZBROWSER_LAINOS_HTTP_MAX_BYTES, &info);
            zbrowser_lainos_http_last_error = info.error;
            zbrowser_lainos_http_last_status = info.status_code;
            body_size = info.body_size != 0u ? info.body_size : (rc > 0 ? (uint32_t)rc : 0u);
            zbrowser_lainos_http_last_bytes = body_size;
            if (body_size > ZBROWSER_LAINOS_HTTP_MAX_BYTES) {
                body_size = ZBROWSER_LAINOS_HTTP_MAX_BYTES;
            }
            if (rc < 0 || info.status_code >= 400u) {
                ++zbrowser_lainos_http_fetch_error_count;
                free(buffer);
                memset(&msg, 0, sizeof(msg));
                msg.type = FETCH_ERROR;
                msg.data.error = "HTTP fetch failed";
                fetch_send_callback(&msg, http->fetch);
                fetch_remove_from_queues(http->fetch);
                fetch_free(http->fetch);
                (void)next;
                return;
            }
            buffer[body_size] = 0;
            content_type = info.content_type[0] != 0
                ? info.content_type
                : zbrowser_lainos_filetype_for_url(url);
            payload = buffer;
            payload_size = body_size;
            filtered = 0;
            if (zbrowser_lainos_is_css_response(content_type, url)) {
                filtered = zbrowser_lainos_css_compat_filter(buffer, body_size, &filtered_len);
                if (filtered != 0) {
                    payload = filtered;
                    payload_size = filtered_len;
                    content_type = "text/css; charset=utf-8";
                }
            }
            (void)fetch_set_http_code(http->fetch, info.status_code != 0u ? info.status_code : 200u);
            header_len = snprintf(header, sizeof(header), "Content-Type: %s", content_type);
            if (header_len > 0 && header_len < (int)sizeof(header)) {
                zbrowser_lainos_http_send_header(http, header, (size_t)header_len);
            }
            header_len = snprintf(header, sizeof(header), "Content-Length: %u", payload_size);
            if (!http->aborted && header_len > 0 && header_len < (int)sizeof(header)) {
                zbrowser_lainos_http_send_header(http, header, (size_t)header_len);
            }
            if (!http->aborted) {
                memset(&msg, 0, sizeof(msg));
                msg.type = FETCH_DATA;
                msg.data.header_or_data.buf = (const uint8_t *)payload;
                msg.data.header_or_data.len = payload_size;
                fetch_send_callback(&msg, http->fetch);
            }
            if (!http->aborted) {
                memset(&msg, 0, sizeof(msg));
                msg.type = FETCH_FINISHED;
                fetch_send_callback(&msg, http->fetch);
            }
            if (!http->aborted && http->cacheable) {
                zbrowser_lainos_resource_cache_store(url,
                                                     content_type,
                                                     (const uint8_t *)payload,
                                                     payload_size);
            }
            if (!http->aborted) {
                ++zbrowser_lainos_http_fetch_success_count;
                zbrowser_lainos_http_last_error = 0;
                zbrowser_lainos_http_last_status = info.status_code != 0u
                    ? info.status_code
                    : 200u;
                zbrowser_lainos_http_last_bytes = payload_size;
            }
            free(filtered);
            free(buffer);
            fetch_remove_from_queues(http->fetch);
            fetch_free(http->fetch);
        }
        (void)next;
    }
#else
    struct fetch *fetch;

    (void)scheme;
    fetch = zbrowser_lainos_fetches;
    while (fetch != 0) {
        struct fetch *next = fetch->next;
        zbrowser_lainos_http_fetch_t *http =
            (zbrowser_lainos_http_fetch_t *)fetch->fetcher_handle;
        if (fetch->fetcher != 0 &&
            fetch->fetcher->ops.poll == zbrowser_lainos_http_poll &&
            http != 0 &&
            !http->sent &&
            !http->aborted) {
            fetch_msg msg;
            zbrowser_lainos_http_info_t info;
            char *buffer;
            char *payload;
            char *filtered;
            const char *content_type;
            const char *url;
            uint32_t body_size;
            uint32_t payload_size;
            uint32_t filtered_len;
            char header[128];
            int header_len;
            int rc;

            http->sent = true;
            url = nsurl_access(http->url);
            {
                zbrowser_lainos_resource_cache_entry_t *entry =
                    zbrowser_lainos_resource_cache_find(url);
                if (http->cacheable && entry != 0 && entry->data != 0 && entry->content_type != 0) {
                    ++zbrowser_lainos_resource_cache_hits;
                    ++zbrowser_lainos_http_fetch_success_count;
                    zbrowser_lainos_http_last_error = 0;
                    zbrowser_lainos_http_last_status = 200;
                    zbrowser_lainos_http_last_bytes = entry->size;
                    (void)fetch_set_http_code(fetch, 200);
                    header_len = snprintf(header,
                                          sizeof(header),
                                          "Content-Type: %s",
                                          entry->content_type);
                    if (header_len > 0 && header_len < (int)sizeof(header)) {
                        zbrowser_lainos_http_send_header(http, header, (size_t)header_len);
                    }
                    header_len = snprintf(header,
                                          sizeof(header),
                                          "Content-Length: %u",
                                          entry->size);
                    if (!http->aborted && header_len > 0 && header_len < (int)sizeof(header)) {
                        zbrowser_lainos_http_send_header(http, header, (size_t)header_len);
                    }
                    if (!http->aborted) {
                        memset(&msg, 0, sizeof(msg));
                        msg.type = FETCH_DATA;
                        msg.data.header_or_data.buf = entry->data;
                        msg.data.header_or_data.len = entry->size;
                        fetch_send_callback(&msg, fetch);
                    }
                    if (!http->aborted) {
                        memset(&msg, 0, sizeof(msg));
                        msg.type = FETCH_FINISHED;
                        fetch_send_callback(&msg, fetch);
                    }
                    if (fetch->auto_free) {
                        fetch_remove_from_queues(fetch);
                        fetch_free(fetch);
                    }
                    fetch = next;
                    continue;
                }
            }
            buffer = malloc(ZBROWSER_LAINOS_HTTP_MAX_BYTES + 1u);
            if (buffer == 0) {
                ++zbrowser_lainos_http_fetch_error_count;
                zbrowser_lainos_http_last_error = -11;
                memset(&msg, 0, sizeof(msg));
                msg.type = FETCH_ERROR;
                msg.data.error = "HTTP fetch allocation failed";
                fetch_send_callback(&msg, fetch);
                if (fetch->auto_free) {
                    fetch_remove_from_queues(fetch);
                    fetch_free(fetch);
                }
                fetch = next;
                continue;
            }
            memset(&info, 0, sizeof(info));
            ++zbrowser_lainos_http_fetch_count;
            rc = os_http_get_ex(url, buffer, ZBROWSER_LAINOS_HTTP_MAX_BYTES, &info);
            zbrowser_lainos_http_last_error = info.error;
            zbrowser_lainos_http_last_status = info.status_code;
            body_size = info.body_size != 0u ? info.body_size : (rc > 0 ? (uint32_t)rc : 0u);
            zbrowser_lainos_http_last_bytes = body_size;
            if (body_size > ZBROWSER_LAINOS_HTTP_MAX_BYTES) {
                body_size = ZBROWSER_LAINOS_HTTP_MAX_BYTES;
            }
            if (rc < 0 || info.status_code >= 400u) {
                ++zbrowser_lainos_http_fetch_error_count;
                free(buffer);
                memset(&msg, 0, sizeof(msg));
                msg.type = FETCH_ERROR;
                msg.data.error = "HTTP fetch failed";
                fetch_send_callback(&msg, fetch);
                if (fetch->auto_free) {
                    fetch_remove_from_queues(fetch);
                    fetch_free(fetch);
                }
                fetch = next;
                continue;
            }
            buffer[body_size] = 0;
            content_type = info.content_type[0] != 0
                ? info.content_type
                : zbrowser_lainos_filetype_for_url(url);
            payload = buffer;
            payload_size = body_size;
            filtered = 0;
            if (zbrowser_lainos_is_css_response(content_type, url)) {
                filtered = zbrowser_lainos_css_compat_filter(buffer, body_size, &filtered_len);
                if (filtered != 0) {
                    payload = filtered;
                    payload_size = filtered_len;
                    content_type = "text/css; charset=utf-8";
                }
            }
            (void)fetch_set_http_code(fetch, info.status_code != 0u ? info.status_code : 200u);
            header_len = snprintf(header, sizeof(header), "Content-Type: %s", content_type);
            if (header_len > 0 && header_len < (int)sizeof(header)) {
                zbrowser_lainos_http_send_header(http, header, (size_t)header_len);
            }
            header_len = snprintf(header, sizeof(header), "Content-Length: %u", payload_size);
            if (!http->aborted && header_len > 0 && header_len < (int)sizeof(header)) {
                zbrowser_lainos_http_send_header(http, header, (size_t)header_len);
            }
            if (!http->aborted) {
                memset(&msg, 0, sizeof(msg));
                msg.type = FETCH_DATA;
                msg.data.header_or_data.buf = (const uint8_t *)payload;
                msg.data.header_or_data.len = payload_size;
                fetch_send_callback(&msg, fetch);
            }
            if (!http->aborted) {
                memset(&msg, 0, sizeof(msg));
                msg.type = FETCH_FINISHED;
                fetch_send_callback(&msg, fetch);
            }
            if (!http->aborted && http->cacheable) {
                zbrowser_lainos_resource_cache_store(url,
                                                     content_type,
                                                     (const uint8_t *)payload,
                                                     payload_size);
            }
            if (!http->aborted) {
                ++zbrowser_lainos_http_fetch_success_count;
                zbrowser_lainos_http_last_error = 0;
                zbrowser_lainos_http_last_status = info.status_code != 0u
                    ? info.status_code
                    : 200u;
                zbrowser_lainos_http_last_bytes = payload_size;
            }
            free(filtered);
            free(buffer);
            if (fetch->auto_free) {
                fetch_remove_from_queues(fetch);
                fetch_free(fetch);
            }
        }
        fetch = next;
    }
#endif
}

static nserror zbrowser_lainos_http_fetcher_register_one(const char *name) {
    lwc_string *scheme = 0;
    const struct fetcher_operation_table ops = {
        .initialise = zbrowser_lainos_http_initialise,
        .acceptable = zbrowser_lainos_http_acceptable,
        .setup = zbrowser_lainos_http_setup,
        .start = zbrowser_lainos_http_start,
        .abort = zbrowser_lainos_http_abort,
        .free = zbrowser_lainos_http_free,
        .poll = zbrowser_lainos_http_poll,
        .finalise = 0
    };

    if (lwc_intern_string(name, strlen(name), &scheme) != lwc_error_ok) {
        return NSERROR_NOMEM;
    }
    return fetcher_add(scheme, &ops);
}

nserror zbrowser_lainos_http_fetcher_register(void) {
    nserror err = zbrowser_lainos_http_fetcher_register_one("http");
    if (err != NSERROR_OK) {
        return err;
    }
    return zbrowser_lainos_http_fetcher_register_one("https");
}

bool urldb_get_cert_permissions(struct nsurl *url) {
    (void)url;
    return false;
}

const char *urldb_get_auth_details(struct nsurl *url, const char *realm) {
    (void)url;
    (void)realm;
    return 0;
}

bool urldb_get_hsts_enabled(struct nsurl *url) {
    (void)url;
    return false;
}

bool urldb_set_cookie(const char *header, struct nsurl *url, struct nsurl *referrer) {
    (void)header;
    (void)url;
    (void)referrer;
    return false;
}

bool urldb_set_hsts_policy(struct nsurl *url, const char *header) {
    (void)url;
    (void)header;
    return false;
}

nserror content_textsearch(struct hlcache_handle *handle, void *context,
        search_flags_t flags, const char *string) {
    (void)handle;
    (void)context;
    (void)flags;
    (void)string;
    return NSERROR_OK;
}

nserror content_textsearch_destroy(struct textsearch_context *textsearch) {
    (void)textsearch;
    return NSERROR_OK;
}

nserror content_textsearch_clear(struct hlcache_handle *handle) {
    (void)handle;
    return NSERROR_OK;
}

bool content_textsearch_ishighlighted(struct textsearch_context *textsearch,
        unsigned start, unsigned end, unsigned *start_idx, unsigned *end_idx) {
    (void)textsearch;
    (void)start;
    (void)end;
    if (start_idx != 0) {
        *start_idx = 0;
    }
    if (end_idx != 0) {
        *end_idx = 0;
    }
    return false;
}

nserror content_textsearch_add_match(struct textsearch_context *context,
        unsigned start_idx, unsigned end_idx, struct box *start_ptr,
        struct box *end_ptr) {
    (void)context;
    (void)start_idx;
    (void)end_idx;
    (void)start_ptr;
    (void)end_ptr;
    return NSERROR_OK;
}

const char *content_textsearch_find_pattern(const char *string, int s_len,
        const char *pattern, int p_len, bool case_sens, unsigned int *m_len) {
    (void)string;
    (void)s_len;
    (void)pattern;
    (void)p_len;
    (void)case_sens;
    if (m_len != 0) {
        *m_len = 0;
    }
    return 0;
}

#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
nserror browser_window_navigate(struct browser_window *bw,
                                struct nsurl *url,
                                struct nsurl *referrer,
                                enum browser_window_nav_flags flags,
                                char *post_urlenc,
                                struct fetch_multipart_data *post_multipart,
                                struct hlcache_handle *parent) {
    (void)bw;
    (void)referrer;
    (void)flags;
    (void)parent;
    if (url == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    if (post_urlenc != 0 || post_multipart != 0) {
        return NSERROR_NOT_IMPLEMENTED;
    }
    snprintf(zbrowser_lainos_pending_url,
             sizeof(zbrowser_lainos_pending_url),
             "%s",
             nsurl_access(url));
    zbrowser_lainos_navigation_pending = zbrowser_lainos_pending_url[0] != 0;
    return zbrowser_lainos_navigation_pending ? NSERROR_OK : NSERROR_BAD_PARAMETER;
}

int zbrowser_lainos_consume_navigation(uint8_t *out, uint32_t capacity) {
    uint32_t i = 0;

    if (!zbrowser_lainos_navigation_pending ||
        out == 0 ||
        capacity == 0u ||
        zbrowser_lainos_pending_url[0] == 0) {
        if (out != 0 && capacity != 0u) {
            out[0] = 0;
        }
        return 0;
    }
    while (zbrowser_lainos_pending_url[i] != 0 && i + 1u < capacity) {
        out[i] = (uint8_t)zbrowser_lainos_pending_url[i];
        ++i;
    }
    out[i] = 0;
    zbrowser_lainos_pending_url[0] = 0;
    zbrowser_lainos_navigation_pending = false;
    return i != 0u ? 1 : 0;
}

struct hlcache_handle *browser_window_get_content(struct browser_window *bw) {
    (void)bw;
    return 0;
}

void browser_window_set_dimensions(struct browser_window *bw, int width, int height) {
    (void)bw;
    (void)width;
    (void)height;
}

void browser_window_reformat(struct browser_window *bw, bool background, int width, int height) {
    (void)bw;
    (void)background;
    (void)width;
    (void)height;
}

float browser_window_get_scale(struct browser_window *bw) {
    (void)bw;
    return 1.0f;
}

nserror browser_window_get_features(struct browser_window *bw,
                                    int x,
                                    int y,
                                    struct browser_window_features *data) {
    (void)bw;
    (void)x;
    (void)y;
    if (data != 0) {
        memset(data, 0, sizeof(*data));
    }
    return NSERROR_OK;
}

bool browser_window_scroll_at_point(struct browser_window *bw, int x, int y, int scrx, int scry) {
    (void)bw;
    (void)x;
    (void)y;
    (void)scrx;
    (void)scry;
    return false;
}

bool browser_window_drop_file_at_point(struct browser_window *bw, int x, int y, char *file) {
    (void)bw;
    (void)x;
    (void)y;
    (void)file;
    return false;
}

void browser_window_mouse_click(struct browser_window *bw,
                                browser_mouse_state mouse,
                                int x,
                                int y) {
    (void)bw;
    (void)mouse;
    (void)x;
    (void)y;
}

void browser_window_mouse_track(struct browser_window *bw,
                                browser_mouse_state mouse,
                                int x,
                                int y) {
    (void)bw;
    (void)mouse;
    (void)x;
    (void)y;
}

struct browser_window *browser_window_find_target(struct browser_window *bw,
                                                  const char *target,
                                                  browser_mouse_state mouse) {
    (void)target;
    (void)mouse;
    return bw;
}

void browser_window_page_drag_start(struct browser_window *bw, int x, int y) {
    (void)bw;
    (void)x;
    (void)y;
}

bool browser_window_redraw(struct browser_window *bw,
                           int x,
                           int y,
                           const struct rect *clip,
                           const struct redraw_context *ctx) {
    (void)bw;
    (void)x;
    (void)y;
    (void)clip;
    (void)ctx;
    return true;
}

void browser_window_get_position(struct browser_window *bw, bool root, int *pos_x, int *pos_y) {
    (void)bw;
    (void)root;
    if (pos_x != 0) {
        *pos_x = 0;
    }
    if (pos_y != 0) {
        *pos_y = 0;
    }
}

void browser_window_set_position(struct browser_window *bw, int x, int y) {
    (void)bw;
    (void)x;
    (void)y;
}

void browser_window_set_drag_type(struct browser_window *bw,
                                  browser_drag_type type,
                                  const struct rect *rect) {
    (void)bw;
    (void)type;
    (void)rect;
}

browser_drag_type browser_window_get_drag_type(struct browser_window *bw) {
    (void)bw;
    return DRAGGING_NONE;
}

nserror browser_window_history_back(struct browser_window *bw, bool new_window) {
    (void)bw;
    (void)new_window;
    return NSERROR_NOT_IMPLEMENTED;
}

nserror browser_window_history_forward(struct browser_window *bw, bool new_window) {
    (void)bw;
    (void)new_window;
    return NSERROR_NOT_IMPLEMENTED;
}

bool browser_window_frame_resize_start(struct browser_window *bw,
                                       browser_mouse_state mouse,
                                       int x,
                                       int y,
                                       browser_pointer_shape *pointer) {
    (void)bw;
    (void)mouse;
    (void)x;
    (void)y;
    if (pointer != 0) {
        *pointer = BROWSER_POINTER_AUTO;
    }
    return false;
}
#else
void browser_window_navigate(void) {}
void browser_window_redraw(void) {}
void browser_window_reformat(void) {}
void browser_window_set_dimensions(void) {}
void browser_window_set_drag_type(void) {}
void browser_window_set_position(void) {}
void browser_window_mouse_click(void) {}
void browser_window_mouse_track(void) {}
void browser_window_page_drag_start(void) {}
void browser_window_find_target(void) {}
void browser_window_get_content(void) {}
void browser_window_get_drag_type(void) {}
void browser_window_get_features(void) {}
void browser_window_get_position(void) {}
void browser_window_get_scale(void) {}
void browser_window_history_back(void) {}
void browser_window_history_forward(void) {}
void browser_window_scroll_at_point(void) {}
void browser_window_drop_file_at_point(void) {}
void browser_window_frame_resize_start(void) {}
#endif

struct jsheap {
    duk_context *ctx;
    uint64_t exec_start_ms;
    uint32_t timeout_ms;
};

struct jsthread {
    struct jsheap *heap;
    bool closed;
};

static void *zbrowser_lainos_js_alloc(void *udata, duk_size_t size) {
    (void)udata;
    return malloc(size);
}

static void *zbrowser_lainos_js_realloc(void *udata, void *ptr, duk_size_t size) {
    (void)udata;
    if (size == 0u) {
        free(ptr);
        return 0;
    }
    return realloc(ptr, size);
}

static void zbrowser_lainos_js_free(void *udata, void *ptr) {
    (void)udata;
    free(ptr);
}

duk_bool_t dukky_check_timeout(void *udata) {
    struct jsheap *heap = (struct jsheap *)udata;
    uint64_t now = (uint64_t)time(0) * 1000u;

    if (heap == 0 || heap->exec_start_ms == 0u || heap->timeout_ms == 0u) {
        return 0;
    }
    return now - heap->exec_start_ms > heap->timeout_ms;
}

static void zbrowser_lainos_js_install_browser_stubs(duk_context *ctx) {
    static const char bootstrap[] =
        "var window=this,self=this,top=this,parent=this;"
        "var location=location||{href:'about:blank'};"
        "var navigator=navigator||{userAgent:'NetSurf LainOS Duktape'};"
        "var console=console||{log:function(){},warn:function(){},error:function(){}};"
        "function __lainNode(){return {style:{},children:[],"
        "appendChild:function(n){this.children.push(n);return n;},"
        "removeChild:function(n){return n;},"
        "setAttribute:function(k,v){this[k]=String(v);},"
        "getAttribute:function(k){return this[k]||null;},"
        "addEventListener:function(){},removeEventListener:function(){},"
        "querySelector:function(){return null;},querySelectorAll:function(){return []}};}"
        "var document=document||{documentElement:__lainNode(),body:__lainNode(),"
        "createElement:function(){return __lainNode();},"
        "createTextNode:function(t){return {nodeValue:String(t||'')};},"
        "getElementById:function(){return null;},"
        "getElementsByTagName:function(){return []},"
        "querySelector:function(){return null;},querySelectorAll:function(){return []},"
        "addEventListener:function(){},removeEventListener:function(){}};"
        "function addEventListener(){} function removeEventListener(){}"
        "function setTimeout(fn){if(typeof fn==='function')fn();return 0;}"
        "function clearTimeout(){} function setInterval(){return 0;} function clearInterval(){}";

    duk_push_lstring(ctx, "lainos-js-bootstrap", sizeof("lainos-js-bootstrap") - 1u);
    if (duk_pcompile_lstring_filename(ctx,
                                      DUK_COMPILE_EVAL,
                                      bootstrap,
                                      sizeof(bootstrap) - 1u) == 0) {
        (void)duk_pcall(ctx, 0);
    }
    duk_pop(ctx);
}

void js_initialise(void) {
    javascript_init();
}

void js_finalise(void) {}

nserror js_newheap(int timeout, jsheap **heap) {
    struct jsheap *ret;

    if (heap == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    *heap = 0;
    ret = calloc(1, sizeof(*ret));
    if (ret == 0) {
        return NSERROR_NOMEM;
    }
    ret->timeout_ms = timeout > 0 ? (uint32_t)timeout * 1000u : 3000u;
    ret->ctx = duk_create_heap(zbrowser_lainos_js_alloc,
                               zbrowser_lainos_js_realloc,
                               zbrowser_lainos_js_free,
                               ret,
                               0);
    if (ret->ctx == 0) {
        free(ret);
        return NSERROR_NOMEM;
    }
    zbrowser_lainos_js_install_browser_stubs(ret->ctx);
    *heap = ret;
    return NSERROR_OK;
}

void js_destroyheap(jsheap *heap) {
    if (heap == 0) {
        return;
    }
    if (heap->ctx != 0) {
        duk_destroy_heap(heap->ctx);
    }
    free(heap);
}

nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv, jsthread **thread) {
    struct jsthread *ret;
    (void)win_priv;
    (void)doc_priv;

    if (heap == 0 || thread == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    *thread = 0;
    ret = calloc(1, sizeof(*ret));
    if (ret == 0) {
        return NSERROR_NOMEM;
    }
    ret->heap = heap;
    *thread = ret;
    return NSERROR_OK;
}

nserror js_closethread(jsthread *thread) {
    if (thread != 0) {
        thread->closed = true;
    }
    return NSERROR_OK;
}

void js_destroythread(jsthread *thread) {
    free(thread);
}

bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name) {
    duk_context *ctx;
    bool ok = false;

    if (thread == 0 || thread->heap == 0 || thread->closed ||
        txt == 0 || txtlen == 0u) {
        return false;
    }
    ctx = thread->heap->ctx;
    if (ctx == 0) {
        return false;
    }
    duk_set_top(ctx, 0);
    thread->heap->exec_start_ms = (uint64_t)time(0) * 1000u;
    if (thread->heap->exec_start_ms == 0u) {
        thread->heap->exec_start_ms = 1u;
    }
    duk_push_string(ctx, name != 0 ? name : "?javascript?");
    if (duk_pcompile_lstring_filename(ctx,
                                      DUK_COMPILE_EVAL,
                                      (const char *)txt,
                                      (duk_size_t)txtlen) == 0 &&
        duk_pcall(ctx, 0) == 0) {
        ok = true;
    }
    thread->heap->exec_start_ms = 0u;
    duk_set_top(ctx, 0);
    return ok;
}

bool js_fire_event(jsthread *thread,
        const char *type,
        struct dom_document *doc,
        struct dom_node *target) {
    (void)thread;
    (void)type;
    (void)doc;
    (void)target;
    return true;
}

void js_handle_new_element(jsthread *thread, struct dom_element *node) {
    (void)thread;
    (void)node;
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt) {
    (void)thread;
    (void)evt;
}

bool knockout_plot_start(const struct redraw_context *ctx,
                         struct redraw_context *knk_ctx) {
    if (ctx != 0 && knk_ctx != 0) {
        *knk_ctx = *ctx;
    }
    return true;
}

bool knockout_plot_end(const struct redraw_context *ctx) {
    (void)ctx;
    return true;
}

const struct plotter_table knockout_plotters;

void save_text_solve_whitespace(struct box *box,
                                bool *first,
                                save_text_whitespace *before,
                                const char **whitespace_text,
                                size_t *whitespace_length) {
    static const char one_space[] = " ";
    (void)box;
    if (first != 0) {
        *first = false;
    }
    if (before != 0) {
        *before = WHITESPACE_ONE_NEW_LINE;
    }
    if (whitespace_text != 0) {
        *whitespace_text = one_space;
    }
    if (whitespace_length != 0) {
        *whitespace_length = 1;
    }
}

struct selection *selection_create(struct content *content) {
    (void)content;
    return (struct selection *)calloc(1u, 1u);
}

void selection_destroy(struct selection *selection) {
    free(selection);
}

void selection_init(struct selection *selection) {
    (void)selection;
}

void selection_reinit(struct selection *selection) {
    (void)selection;
}

bool selection_clear(struct selection *selection, bool redraw) {
    (void)selection;
    (void)redraw;
    return false;
}

void selection_select_all(struct selection *selection) {
    (void)selection;
}

void selection_set_position(struct selection *selection,
                            unsigned start,
                            unsigned end) {
    (void)selection;
    (void)start;
    (void)end;
}

bool selection_click(struct selection *selection,
                     struct browser_window *top,
                     browser_mouse_state mouse,
                     unsigned idx) {
    (void)selection;
    (void)top;
    (void)mouse;
    (void)idx;
    return false;
}

void selection_track(struct selection *selection,
                     browser_mouse_state mouse,
                     unsigned idx) {
    (void)selection;
    (void)mouse;
    (void)idx;
}

bool selection_copy_to_clipboard(struct selection *selection) {
    (void)selection;
    return false;
}

char *selection_get_copy(struct selection *selection) {
    (void)selection;
    return 0;
}

bool selection_active(struct selection *selection) {
    (void)selection;
    return false;
}

bool selection_dragging(struct selection *selection) {
    (void)selection;
    return false;
}

bool selection_dragging_start(struct selection *selection) {
    (void)selection;
    return false;
}

void selection_drag_end(struct selection *selection) {
    (void)selection;
}

bool selection_string_append(const char *text,
                             size_t length,
                             bool space,
                             struct plot_font_style *style,
                             struct selection_string *sel_string) {
    (void)text;
    (void)length;
    (void)space;
    (void)style;
    (void)sel_string;
    return true;
}
#endif

#ifndef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
void fetch_multipart_data_destroy(struct fetch_multipart_data *data) {
    (void)data;
}
#endif

#ifndef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
void browser_window_navigate(void) {}
void browser_window_redraw(void) {}
void browser_window_reformat(void) {}
void browser_window_set_dimensions(void) {}
void browser_window_set_drag_type(void) {}
void browser_window_set_position(void) {}
#endif

bool selection_highlighted(const struct selection *selection,
                           unsigned start,
                           unsigned end,
                           unsigned *start_idx,
                           unsigned *end_idx) {
    (void)selection;
    (void)start;
    (void)end;
    if (start_idx != 0) {
        *start_idx = 0;
    }
    if (end_idx != 0) {
        *end_idx = 0;
    }
    return false;
}

struct textarea {
    textarea_flags flags;
    textarea_setup setup;
    textarea_client_callback callback;
    void *data;
    char *text;
    unsigned int text_len;
    int width;
    int height;
    int caret;
};

struct textarea *textarea_create(const textarea_flags flags,
                                 const textarea_setup *setup,
                                 textarea_client_callback callback,
                                 void *data) {
    struct textarea *textarea = calloc(1, sizeof(*textarea));

    if (textarea == 0) {
        return 0;
    }
    textarea->flags = flags;
    if (setup != 0) {
        textarea->setup = *setup;
        textarea->width = setup->width;
        textarea->height = setup->height;
    }
    textarea->callback = callback;
    textarea->data = data;
    textarea->caret = -1;
    textarea->text = strdup("");
    if (textarea->text == 0) {
        free(textarea);
        return 0;
    }
    return textarea;
}

void textarea_destroy(struct textarea *textarea) {
    if (textarea != 0) {
        free(textarea->text);
        free(textarea);
    }
}

bool textarea_keypress(struct textarea *textarea, uint32_t key) {
    (void)textarea;
    (void)key;
    return false;
}

void textarea_redraw(struct textarea *textarea,
                     int x,
                     int y,
                     colour bg,
                     float scale,
                     const struct rect *clip,
                     const struct redraw_context *ctx) {
    (void)textarea;
    (void)x;
    (void)y;
    (void)bg;
    (void)scale;
    (void)clip;
    (void)ctx;
}

bool textarea_set_caret(struct textarea *textarea, int caret) {
    if (textarea == 0) {
        return false;
    }
    textarea->caret = caret;
    return true;
}

void textarea_set_layout(struct textarea *textarea,
                         const plot_font_style_t *fstyle,
                         int width,
                         int height,
                         int top,
                         int right,
                         int bottom,
                         int left) {
    if (textarea == 0) {
        return;
    }
    if (fstyle != 0) {
        textarea->setup.text = *fstyle;
    }
    textarea->width = width;
    textarea->height = height;
    textarea->setup.width = width;
    textarea->setup.height = height;
    textarea->setup.pad_top = top;
    textarea->setup.pad_right = right;
    textarea->setup.pad_bottom = bottom;
    textarea->setup.pad_left = left;
}

bool textarea_set_text(struct textarea *textarea, const char *text) {
    char *copy;

    if (textarea == 0) {
        return false;
    }
    copy = strdup(text != 0 ? text : "");
    if (copy == 0) {
        return false;
    }
    free(textarea->text);
    textarea->text = copy;
    textarea->text_len = (unsigned int)strlen(copy);
    return true;
}

bool textarea_drop_text(struct textarea *textarea,
                        const char *text,
                        size_t text_length) {
    char *next;
    size_t old_len;

    if (textarea == 0 || text == 0) {
        return false;
    }
    old_len = textarea->text != 0 ? strlen(textarea->text) : 0u;
    next = malloc(old_len + text_length + 1u);
    if (next == 0) {
        return false;
    }
    if (old_len != 0u) {
        memcpy(next, textarea->text, old_len);
    }
    memcpy(next + old_len, text, text_length);
    next[old_len + text_length] = 0;
    free(textarea->text);
    textarea->text = next;
    textarea->text_len = (unsigned int)(old_len + text_length);
    return true;
}

int textarea_get_text(struct textarea *textarea, char *buf, unsigned int len) {
    unsigned int text_len = textarea != 0 ? textarea->text_len : 0u;

    if (buf == 0 || len == 0u) {
        return (int)(text_len + 1u);
    }
    if (textarea == 0 || textarea->text == 0) {
        buf[0] = 0;
        return 1;
    }
    if (len <= text_len) {
        memcpy(buf, textarea->text, len - 1u);
        buf[len - 1u] = 0;
        return (int)len;
    }
    memcpy(buf, textarea->text, text_len + 1u);
    return (int)(text_len + 1u);
}

const char *textarea_data(struct textarea *textarea, unsigned int *len) {
    if (len != 0) {
        *len = textarea != 0 ? textarea->text_len : 0u;
    }
    return textarea != 0 && textarea->text != 0 ? textarea->text : "";
}

textarea_mouse_status textarea_mouse_action(struct textarea *textarea,
                                            browser_mouse_state mouse,
                                            int x,
                                            int y) {
    (void)textarea;
    (void)mouse;
    (void)x;
    (void)y;
    return TEXTAREA_MOUSE_NONE;
}

bool textarea_clear_selection(struct textarea *textarea) {
    (void)textarea;
    return false;
}

char *textarea_get_selection(struct textarea *textarea) {
    (void)textarea;
    return 0;
}

void textarea_get_dimensions(struct textarea *textarea, int *width, int *height) {
    if (width != 0) {
        *width = textarea != 0 ? textarea->width : 0;
    }
    if (height != 0) {
        *height = textarea != 0 ? textarea->height : 0;
    }
}

void textarea_set_dimensions(struct textarea *textarea, int width, int height) {
    if (textarea != 0) {
        textarea->width = width;
        textarea->height = height;
        textarea->setup.width = width;
        textarea->setup.height = height;
    }
}

bool textarea_scroll(struct textarea *textarea, int scrx, int scry) {
    (void)textarea;
    (void)scrx;
    (void)scry;
    return false;
}

const void *urldb_get_url_data(void *url) {
    (void)url;
    return 0;
}

#ifndef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
void html_overflow_scroll_callback(void *client_data, void *scrollbar, int scroll) {
    (void)client_data;
    (void)scrollbar;
    (void)scroll;
}

void html_set_drag_type(void *html, int drag_type, void *drag_owner, const void *rect) {
    (void)html;
    (void)drag_type;
    (void)drag_owner;
    (void)rect;
}

void html_set_focus(void *html, int focus_type, void *focus_owner, int hide_caret,
                    int x, int y, int height, const void *clip) {
    (void)html;
    (void)focus_type;
    (void)focus_owner;
    (void)hide_caret;
    (void)x;
    (void)y;
    (void)height;
    (void)clip;
}
#endif

#ifndef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
void html_set_selection(void *html, int selection_type, void *selection_owner, int read_only) {
    (void)html;
    (void)selection_type;
    (void)selection_owner;
    (void)read_only;
}

void html__redraw_a_box(void *html, void *box) {
    (void)html;
    (void)box;
}

void *html_get_box_tree(void *handle) {
    (void)handle;
    return 0;
}
#endif

void nscss_dump_computed_style(FILE *stream, const void *style) {
    (void)stream;
    (void)style;
}

int html_redraw_printing;
int html_redraw_printing_border;
int html_redraw_printing_top_cropped;
