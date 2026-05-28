#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>

#include "kernel.h"
#include "image.h"
#include "kmem.h"
#include "libc.h"
#include "netsurf_browser.h"
#include "netsurf_frontend.h"
#include "shell.h"
#include "netsurf_resource_css.h"

#include <libwapcaplet/libwapcaplet.h>
#include "content/content_factory.h"
#include "content/content.h"
#include "content/content_protected.h"
#include "content/backing_store.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/hlcache.h"
#include "content/llcache.h"
#include "content/urldb.h"
#include "content/handlers/css/css.h"
#include "content/handlers/image/svg.h"
#include "html/box.h"
#include "html/box_inspect.h"
#include "html/form_internal.h"
#include "html/html.h"
#include "html/private.h"
#include "javascript/js.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "netsurf/fetch.h"
#include "netsurf/keypress.h"
#include "netsurf/mouse.h"
#include "netsurf/misc.h"
#include "netsurf/plotters.h"
#include "netsurf/ssl_certs.h"
#include "netsurf/url_db.h"
#include "desktop/browser_history.h"
#include "desktop/frames.h"
#include "desktop/gui_internal.h"
#include "desktop/gui_table.h"
#include "desktop/knockout.h"
#include "desktop/save_text.h"
#include "desktop/selection.h"
#include "utils/corestrings.h"
#include "utils/messages.h"
#include "utils/nscolour.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "nsutils/time.h"

typedef enum {
    LLCACHE_STATE_RAM = 0,
    LLCACHE_STATE_DISC
} netsurf_bridge_llcache_store_state_t;

typedef enum {
    NETSURF_BRIDGE_LLCACHE_FETCH_INIT,
    NETSURF_BRIDGE_LLCACHE_FETCH_HEADERS,
    NETSURF_BRIDGE_LLCACHE_FETCH_DATA,
    NETSURF_BRIDGE_LLCACHE_FETCH_COMPLETE
} netsurf_bridge_llcache_fetch_state_t;

typedef struct {
    char *name;
    char *value;
} netsurf_bridge_llcache_header_t;

typedef struct {
    uint32_t flags;
    nsurl *referer;
    llcache_post_data *post;
    struct fetch *fetch;
    netsurf_bridge_llcache_fetch_state_t state;
    uint32_t redirect_count;
    uint32_t retries_remaining;
    bool hsts_in_use;
    bool tried_with_auth;
    bool tried_with_tls_downgrade;
    bool tainted_tls;
} netsurf_bridge_llcache_fetch_t;

typedef enum {
    LLCACHE_VALIDATE_FRESH,
    LLCACHE_VALIDATE_ALWAYS,
    LLCACHE_VALIDATE_ONCE
} netsurf_bridge_llcache_validate_t;

typedef struct {
    time_t req_time;
    time_t res_time;
    time_t fin_time;
    time_t date;
    time_t expires;
    int age;
    int max_age;
    netsurf_bridge_llcache_validate_t no_cache;
    char *etag;
    time_t last_modified;
} netsurf_bridge_llcache_cache_control_t;

typedef struct llcache_object llcache_object;
struct llcache_object {
    llcache_object *prev;
    llcache_object *next;
    nsurl *url;
    uint8_t *source_data;
    size_t source_len;
    size_t source_alloc;
    struct cert_chain *chain;
    netsurf_bridge_llcache_store_state_t store_state;
    void *users;
    netsurf_bridge_llcache_fetch_t fetch;
    netsurf_bridge_llcache_cache_control_t cache;
    llcache_object *candidate;
    uint32_t candidate_count;
    netsurf_bridge_llcache_header_t *headers;
    size_t num_headers;
    time_t last_used;
};

struct llcache_handle {
    llcache_object *object;
    llcache_handle_callback cb;
    void *pw;
    int state;
    size_t bytes;
};

typedef struct netsurf_bridge_view {
    int width;
    int height;
} netsurf_bridge_view_t;

typedef struct netsurf_bridge_bitmap {
    int width;
    int height;
    bool opaque;
    uint8_t *pixels;
} netsurf_bridge_bitmap_t;

typedef struct netsurf_bridge_image_content {
    struct content base;
    struct bitmap *bitmap;
    bool fallback_placeholder;
} netsurf_bridge_image_content_t;

typedef struct netsurf_bridge_http_fetch {
    struct netsurf_bridge_http_fetch *next;
    struct fetch *fetch;
    nsurl *url;
    uint64_t retry_after_tick;
    uint32_t attempt_count;
    bool cacheable;
    bool aborted;
    bool locked;
} netsurf_bridge_http_fetch_t;

typedef struct netsurf_bridge_resource_cache_entry {
    struct netsurf_bridge_resource_cache_entry *next;
    char *url;
    char *content_type;
    uint8_t *data;
    uint32_t size;
    uint64_t last_used;
} netsurf_bridge_resource_cache_entry_t;

#define NETSURF_BRIDGE_SCHEDULE_MAX 256u
#define NETSURF_BRIDGE_SCHEDULE_PUMP_LIMIT 32u
#define NETSURF_BRIDGE_HTTP_MAX_FETCHES 256u
#define NETSURF_BRIDGE_HTTP_MAX_BYTES (8u * 1024u * 1024u)
#define NETSURF_BRIDGE_HTTP_MIN_BYTES (512u * 1024u)
#define NETSURF_BRIDGE_HTTP_FETCHES_PER_POLL 1u
#define NETSURF_BRIDGE_HTTP_SPACING_MS 20u
#define NETSURF_BRIDGE_HTTP_429_RETRY_LIMIT 1u
#define NETSURF_BRIDGE_HTTP_429_BACKOFF_MS 2500u
#define NETSURF_BRIDGE_POLL_CALLBACK_BUDGET 8u
#define NETSURF_BRIDGE_PREPARE_CALLBACK_BUDGET 64u
#define NETSURF_BRIDGE_PREPARE_ROUND_LIMIT 512u
#define NETSURF_BRIDGE_PREPARE_STALL_LIMIT 48u
#define NETSURF_BRIDGE_POLL_STALL_LIMIT 80u
#define NETSURF_BRIDGE_RESOURCE_CACHE_MAX_ENTRIES 96u
#define NETSURF_BRIDGE_RESOURCE_CACHE_MAX_BYTES (16u * 1024u * 1024u)

typedef struct netsurf_bridge_schedule_item {
    bool active;
    void (*callback)(void *p);
    void *p;
} netsurf_bridge_schedule_item_t;

typedef struct netsurf_bridge_document {
    struct llcache_object object;
    struct llcache_handle handle;
    netsurf_bridge_llcache_header_t headers[1];
    nsurl *url;
    struct content *content;
    netsurf_bridge_view_t view;
    const uint8_t *source;
    uint32_t source_len;
    uint32_t source_signature;
    struct jsheap *jsheap;
    bool needs_reformat;
    bool needs_redraw;
    bool prepare_pending;
    bool opened;
    unsigned int poll_last_pending;
    unsigned int poll_stall_count;
} netsurf_bridge_document_t;

typedef struct netsurf_bridge_box_stats {
    unsigned int boxes;
    unsigned int text_boxes;
    unsigned int text_bytes;
    unsigned int object_boxes;
    unsigned int boxes_with_children;
    unsigned int max_depth;
    unsigned int flex_boxes;
    unsigned int grid_display_boxes;
    unsigned int table_boxes;
    unsigned int float_boxes;
    unsigned int form_controls;
    unsigned int iframe_boxes;
    unsigned int fixed_boxes;
    unsigned int sticky_boxes;
    unsigned int negative_boxes;
    unsigned int overwide_boxes;
    unsigned int absolute_boxes;
    unsigned int relative_boxes;
    int min_left;
    int min_top;
    int max_right;
    int max_bottom;
} netsurf_bridge_box_stats_t;

static bool netsurf_bridge_initialised;
static bool netsurf_bridge_schedule_draining;
static netsurf_bridge_schedule_item_t netsurf_bridge_schedule_queue[NETSURF_BRIDGE_SCHEDULE_MAX];
static netsurf_bridge_document_t *netsurf_bridge_cached_document;
static netsurf_bridge_http_fetch_t *netsurf_bridge_http_fetches;
static netsurf_bridge_resource_cache_entry_t *netsurf_bridge_resource_cache;
static uint64_t netsurf_bridge_http_next_fetch_tick;
static uint32_t netsurf_bridge_http_fetch_count;
static uint32_t netsurf_bridge_http_fetch_success_count;
static uint32_t netsurf_bridge_http_fetch_error_count;
static uint32_t netsurf_bridge_resource_cache_hits;
static uint32_t netsurf_bridge_resource_cache_stores;
static uint32_t netsurf_bridge_resource_cache_evictions;
static uint32_t netsurf_bridge_resource_cache_entries;
static uint32_t netsurf_bridge_resource_cache_bytes;
static int32_t netsurf_bridge_http_last_error;
static uint32_t netsurf_bridge_http_last_status;
static uint32_t netsurf_bridge_http_last_bytes;
static uint32_t netsurf_bridge_image_decode_count;
static uint32_t netsurf_bridge_image_error_count;
static uint32_t netsurf_bridge_image_fallback_count;
static uint32_t netsurf_bridge_css_compat_count;
static uint32_t netsurf_bridge_css_compat_var_count;
static uint32_t netsurf_bridge_css_compat_group_count;
static const char *netsurf_bridge_status = "NetSurf core bridge not initialised";
static char netsurf_bridge_status_buffer[768];
static char netsurf_bridge_pending_url[1024];
static bool netsurf_bridge_navigation_pending;

static void netsurf_bridge_destroy_content(struct content *content);
static nserror netsurf_bridge_image_init(void);
static const char *netsurf_bridge_filetype(const char *path);
static bool netsurf_bridge_is_css_response(const char *content_type, const char *url);

static uint32_t netsurf_bridge_memory_cache_limit(void) {
    uint64_t free_bytes = kmem_free_pages() * (uint64_t)KMEM_PAGE_SIZE;
    uint64_t limit = 32ull * 1024ull * 1024ull;

    if (free_bytes != 0 && free_bytes / 4u < limit) {
        limit = free_bytes / 4u;
    }
    if (limit < 4ull * 1024ull * 1024ull) {
        limit = 4ull * 1024ull * 1024ull;
    }
    return (uint32_t)limit;
}

static uint32_t netsurf_bridge_http_byte_limit(void) {
    kmem_stats_t stats;
    uint64_t limit = NETSURF_BRIDGE_HTTP_MAX_BYTES;
    uint64_t free_bytes;
    uint64_t largest_bytes;

    kmem_get_stats(&stats);
    free_bytes = stats.free_pages * (uint64_t)KMEM_PAGE_SIZE;
    largest_bytes = stats.largest_free_range_pages * (uint64_t)KMEM_PAGE_SIZE;

    if (free_bytes != 0 && free_bytes / 8u < limit) {
        limit = free_bytes / 8u;
    }
    if (largest_bytes != 0 && largest_bytes / 2u < limit) {
        limit = largest_bytes / 2u;
    }
    if (limit < NETSURF_BRIDGE_HTTP_MIN_BYTES) {
        limit = NETSURF_BRIDGE_HTTP_MIN_BYTES;
    }
    if (limit > NETSURF_BRIDGE_HTTP_MAX_BYTES) {
        limit = NETSURF_BRIDGE_HTTP_MAX_BYTES;
    }
    return (uint32_t)limit;
}

static char *netsurf_bridge_strdup(const char *src) {
    size_t len;
    char *copy;

    if (src == 0) {
        src = "";
    }
    len = strlen(src);
    copy = malloc(len + 1u);
    if (copy != 0) {
        memcpy(copy, src, len + 1u);
    }
    return copy;
}

static bool netsurf_bridge_resource_cache_allowed(const char *url,
                                                  const char *content_type,
                                                  uint32_t size) {
    if (url == 0 || content_type == 0 || size == 0u ||
        size > NETSURF_BRIDGE_HTTP_MAX_BYTES) {
        return false;
    }
    if (netsurf_bridge_is_css_response(content_type, url)) {
        return true;
    }
    if (strstr(content_type, "javascript") != 0 ||
        strstr(content_type, "ecmascript") != 0 ||
        strncmp(content_type, "image/", 6u) == 0) {
        return true;
    }
    return false;
}

static netsurf_bridge_resource_cache_entry_t *
netsurf_bridge_resource_cache_find(const char *url) {
    netsurf_bridge_resource_cache_entry_t *entry = netsurf_bridge_resource_cache;

    while (entry != 0) {
        if (entry->url != 0 && strcmp(entry->url, url) == 0) {
            entry->last_used = timer_ticks();
            return entry;
        }
        entry = entry->next;
    }
    return 0;
}

static void netsurf_bridge_resource_cache_remove(netsurf_bridge_resource_cache_entry_t *entry) {
    netsurf_bridge_resource_cache_entry_t **slot = &netsurf_bridge_resource_cache;

    while (*slot != 0) {
        if (*slot == entry) {
            *slot = entry->next;
            if (netsurf_bridge_resource_cache_entries != 0u) {
                --netsurf_bridge_resource_cache_entries;
            }
            if (netsurf_bridge_resource_cache_bytes >= entry->size) {
                netsurf_bridge_resource_cache_bytes -= entry->size;
            } else {
                netsurf_bridge_resource_cache_bytes = 0;
            }
            free(entry->url);
            free(entry->content_type);
            free(entry->data);
            free(entry);
            ++netsurf_bridge_resource_cache_evictions;
            return;
        }
        slot = &(*slot)->next;
    }
}

static void netsurf_bridge_resource_cache_trim(void) {
    while (netsurf_bridge_resource_cache_entries > NETSURF_BRIDGE_RESOURCE_CACHE_MAX_ENTRIES ||
           netsurf_bridge_resource_cache_bytes > NETSURF_BRIDGE_RESOURCE_CACHE_MAX_BYTES) {
        netsurf_bridge_resource_cache_entry_t *entry = netsurf_bridge_resource_cache;
        netsurf_bridge_resource_cache_entry_t *oldest = entry;

        while (entry != 0) {
            if (oldest == 0 || entry->last_used < oldest->last_used) {
                oldest = entry;
            }
            entry = entry->next;
        }
        if (oldest == 0) {
            return;
        }
        netsurf_bridge_resource_cache_remove(oldest);
    }
}

static void netsurf_bridge_resource_cache_store(const char *url,
                                                const char *content_type,
                                                const uint8_t *data,
                                                uint32_t size) {
    netsurf_bridge_resource_cache_entry_t *entry;
    uint8_t *copy;
    char *url_copy;
    char *type_copy;

    if (!netsurf_bridge_resource_cache_allowed(url, content_type, size) || data == 0) {
        return;
    }
    entry = netsurf_bridge_resource_cache_find(url);
    if (entry != 0) {
        netsurf_bridge_resource_cache_remove(entry);
    }

    copy = malloc(size);
    url_copy = netsurf_bridge_strdup(url);
    type_copy = netsurf_bridge_strdup(content_type);
    entry = calloc(1, sizeof(*entry));
    if (copy == 0 || url_copy == 0 || type_copy == 0 || entry == 0) {
        free(copy);
        free(url_copy);
        free(type_copy);
        free(entry);
        return;
    }

    memcpy(copy, data, size);
    entry->url = url_copy;
    entry->content_type = type_copy;
    entry->data = copy;
    entry->size = size;
    entry->last_used = timer_ticks();
    entry->next = netsurf_bridge_resource_cache;
    netsurf_bridge_resource_cache = entry;
    ++netsurf_bridge_resource_cache_entries;
    ++netsurf_bridge_resource_cache_stores;
    netsurf_bridge_resource_cache_bytes += size;
    netsurf_bridge_resource_cache_trim();
}

static uint64_t netsurf_bridge_ms_to_ticks(uint32_t ms) {
    uint32_t hz = timer_frequency();
    uint64_t ticks;

    if (hz == 0u) {
        hz = 100u;
    }
    ticks = ((uint64_t)ms * (uint64_t)hz + 999u) / 1000u;
    return ticks != 0u ? ticks : 1u;
}

static const char netsurf_bridge_empty_css[] = "";

static void netsurf_bridge_schedule_remove(void (*callback)(void *p), void *p) {
    size_t i;

    for (i = 0; i < NETSURF_BRIDGE_SCHEDULE_MAX; ++i) {
        if (netsurf_bridge_schedule_queue[i].active &&
            netsurf_bridge_schedule_queue[i].callback == callback &&
            netsurf_bridge_schedule_queue[i].p == p) {
            netsurf_bridge_schedule_queue[i].active = false;
        }
    }
}

static void netsurf_bridge_schedule_clear(void) {
    size_t i;

    for (i = 0; i < NETSURF_BRIDGE_SCHEDULE_MAX; ++i) {
        netsurf_bridge_schedule_queue[i].active = false;
    }
}

static unsigned int netsurf_bridge_schedule_count(void) {
    unsigned int count = 0;
    size_t i;

    for (i = 0; i < NETSURF_BRIDGE_SCHEDULE_MAX; ++i) {
        if (netsurf_bridge_schedule_queue[i].active) {
            ++count;
        }
    }
    return count;
}

static unsigned int netsurf_bridge_http_pending_count(void) {
    unsigned int count = 0;
    netsurf_bridge_http_fetch_t *ctx = netsurf_bridge_http_fetches;

    while (ctx != 0) {
        ++count;
        ctx = ctx->next;
    }
    return count;
}

static unsigned int netsurf_bridge_fetch_pending_count(void) {
    /*
     * NetSurf's generic fetch queue counters are internal to content/fetch.c.
     * The LainOS frontend currently tracks its own async HTTP fetch contexts,
     * so use those for readiness decisions and keep this hook as a placeholder
     * for a future local fetcher wrapper.
     */
    return 0u;
}

static unsigned int netsurf_bridge_pump_fetchers(void) {
    fd_set read_set;
    fd_set write_set;
    fd_set error_set;
    int maxfd = -1;
    unsigned int before = netsurf_bridge_fetch_pending_count() +
                          netsurf_bridge_http_pending_count();

    (void)fetch_fdset(&read_set, &write_set, &error_set, &maxfd);
    return before != 0u || maxfd >= 0 ? 1u : 0u;
}

static bool netsurf_bridge_url_contains(const char *url, const char *needle) {
    size_t needle_len;
    size_t i;
    size_t j;

    if (url == 0 || needle == 0) {
        return false;
    }
    needle_len = strlen(needle);
    if (needle_len == 0u) {
        return true;
    }
    for (i = 0; url[i] != 0; ++i) {
        j = 0;
        while (needle[j] != 0 && url[i + j] != 0 && url[i + j] == needle[j]) {
            ++j;
        }
        if (j == needle_len) {
            return true;
        }
    }
    return false;
}

static bool netsurf_bridge_ascii_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

static bool netsurf_bridge_ascii_match_ci(const char *text, const char *word) {
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

static unsigned int netsurf_bridge_css_group_rule_kind(const char *css, size_t len, size_t at) {
    size_t i = at + 1u;

    while (i < len && netsurf_bridge_ascii_is_space(css[i])) {
        ++i;
    }
    if (netsurf_bridge_ascii_match_ci(css + i, "layer")) {
        return 1u;
    }
    if (netsurf_bridge_ascii_match_ci(css + i, "supports") ||
        netsurf_bridge_ascii_match_ci(css + i, "container")) {
        return 2u;
    }
    return 0u;
}

static size_t netsurf_bridge_css_find_rule_body(const char *css, size_t len, size_t at) {
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

static size_t netsurf_bridge_css_find_matching_paren(const char *css, size_t len, size_t open) {
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

static size_t netsurf_bridge_css_find_matching_brace(const char *css, size_t len, size_t open) {
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

static size_t netsurf_bridge_css_find_var_fallback(const char *css, size_t start, size_t end) {
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

static bool netsurf_bridge_ascii_equal_ci_n(const char *text, size_t len, const char *word) {
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

static bool netsurf_bridge_ascii_ends_ci_n(const char *text, size_t len, const char *suffix) {
    size_t suffix_len;

    if (text == 0 || suffix == 0) {
        return false;
    }
    suffix_len = strlen(suffix);
    if (suffix_len > len) {
        return false;
    }
    return netsurf_bridge_ascii_equal_ci_n(text + len - suffix_len, suffix_len, suffix);
}

static bool netsurf_bridge_css_property_allows_var_fallback(const char *css, size_t len, size_t var_at) {
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
    while (start < var_at && netsurf_bridge_ascii_is_space(css[start])) {
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
    while (name_end > name_start && netsurf_bridge_ascii_is_space(css[name_end - 1u])) {
        --name_end;
    }
    if (name_end <= name_start) {
        return false;
    }

    if (netsurf_bridge_ascii_equal_ci_n(css + name_start, name_end - name_start, "color") ||
        netsurf_bridge_ascii_equal_ci_n(css + name_start, name_end - name_start, "background") ||
        netsurf_bridge_ascii_equal_ci_n(css + name_start, name_end - name_start, "fill") ||
        netsurf_bridge_ascii_equal_ci_n(css + name_start, name_end - name_start, "stroke") ||
        netsurf_bridge_ascii_equal_ci_n(css + name_start, name_end - name_start, "box-shadow") ||
        netsurf_bridge_ascii_equal_ci_n(css + name_start, name_end - name_start, "text-shadow") ||
        netsurf_bridge_ascii_equal_ci_n(css + name_start, name_end - name_start, "accent-color") ||
        netsurf_bridge_ascii_ends_ci_n(css + name_start, name_end - name_start, "-color")) {
        return true;
    }
    return false;
}

static bool netsurf_bridge_is_css_response(const char *content_type, const char *url) {
    return netsurf_bridge_url_contains(content_type, "text/css") ||
           netsurf_bridge_url_contains(url, ".css") ||
           netsurf_bridge_url_contains(url, "only=styles") ||
           netsurf_bridge_url_contains(url, "type=text/css");
}

static char *netsurf_bridge_css_compat_filter(const char *css,
                                              uint32_t len,
                                              uint32_t *out_len,
                                              uint32_t *out_vars,
                                              uint32_t *out_groups) {
    size_t i = 0;
    size_t out = 0;
    unsigned int depth = 0u;
    unsigned int skip_depths[16];
    unsigned int skip_count = 0u;
    uint32_t vars = 0u;
    uint32_t groups = 0u;
    char *filtered;

    if (out_len != 0) {
        *out_len = len;
    }
    if (out_vars != 0) {
        *out_vars = 0;
    }
    if (out_groups != 0) {
        *out_groups = 0;
    }
    if (css == 0 || len == 0u) {
        return 0;
    }

    filtered = malloc((size_t)len + 1u);
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
            unsigned int group_kind = netsurf_bridge_css_group_rule_kind(css, len, i);
            size_t body = netsurf_bridge_css_find_rule_body(css, len, i);
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
                size_t close = netsurf_bridge_css_find_matching_brace(css, len, body);
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
            netsurf_bridge_ascii_match_ci(css + i, "var(")) {
            size_t close = netsurf_bridge_css_find_matching_paren(css, len, i + 3u);
            if (close < len) {
                size_t comma = netsurf_bridge_css_find_var_fallback(css, i + 4u, close);
                if (comma < close && netsurf_bridge_css_property_allows_var_fallback(css, len, i)) {
                    size_t start = comma + 1u;
                    size_t end = close;
                    while (start < end && netsurf_bridge_ascii_is_space(css[start])) {
                        ++start;
                    }
                    while (end > start && netsurf_bridge_ascii_is_space(css[end - 1u])) {
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
    if (out_vars != 0) {
        *out_vars = vars;
    }
    if (out_groups != 0) {
        *out_groups = groups;
    }
    if (vars == 0u && groups == 0u) {
        free(filtered);
        return 0;
    }
    return filtered;
}

static unsigned int netsurf_bridge_http_priority(const netsurf_bridge_http_fetch_t *ctx) {
    const char *url;

    if (ctx == 0 || ctx->url == 0) {
        return 0u;
    }
    url = nsurl_access(ctx->url);
    if (netsurf_bridge_url_contains(url, ".css") ||
        netsurf_bridge_url_contains(url, "only=styles") ||
        netsurf_bridge_url_contains(url, "type=text/css") ||
        netsurf_bridge_url_contains(url, "styles")) {
        return 100u;
    }
    if (netsurf_bridge_url_contains(url, ".js") ||
        netsurf_bridge_url_contains(url, "javascript") ||
        netsurf_bridge_url_contains(url, "load.php")) {
        return 80u;
    }
    if (netsurf_bridge_url_contains(url, "favicon") ||
        netsurf_bridge_url_contains(url, "apple-touch-icon")) {
        return 10u;
    }
    if (netsurf_bridge_url_contains(url, ".png") ||
        netsurf_bridge_url_contains(url, ".jpg") ||
        netsurf_bridge_url_contains(url, ".jpeg") ||
        netsurf_bridge_url_contains(url, ".gif") ||
        netsurf_bridge_url_contains(url, ".webp") ||
        netsurf_bridge_url_contains(url, ".svg") ||
        netsurf_bridge_url_contains(url, ".ico")) {
        return 30u;
    }
    return 20u;
}

static netsurf_bridge_http_fetch_t *netsurf_bridge_http_select_fetch(uint64_t now) {
    netsurf_bridge_http_fetch_t *ctx = netsurf_bridge_http_fetches;
    netsurf_bridge_http_fetch_t *best = 0;
    unsigned int best_priority = 0u;

    while (ctx != 0) {
        unsigned int priority;

        if (ctx->locked ||
            (ctx->retry_after_tick != 0 && now < ctx->retry_after_tick)) {
            ctx = ctx->next;
            continue;
        }
        priority = netsurf_bridge_http_priority(ctx);
        if (best == 0 || priority > best_priority) {
            best = ctx;
            best_priority = priority;
        }
        ctx = ctx->next;
    }
    return best;
}

static void netsurf_bridge_html_stats(const struct content *content,
                                      unsigned int *css_total,
                                      unsigned int *css_loaded,
                                      unsigned int *obj_total,
                                      unsigned int *obj_loaded) {
    const html_content *html;
    const struct content_html_object *object;
    unsigned int i;

    *css_total = 0;
    *css_loaded = 0;
    *obj_total = 0;
    *obj_loaded = 0;
    if (content == 0) {
        return;
    }

    html = (const html_content *)content;
    for (i = 0; i < html->stylesheet_count; ++i) {
        bool count_sheet = i >= STYLESHEET_START || html->stylesheets[i].sheet != 0;

        if (count_sheet) {
            ++*css_total;
        }
        if (html->stylesheets[i].sheet != 0) {
            content_status status = content_get_status(html->stylesheets[i].sheet);
            if (status == CONTENT_STATUS_READY || status == CONTENT_STATUS_DONE) {
                ++*css_loaded;
            }
        }
    }

    *obj_total = html->num_objects;
    for (object = html->object_list; object != 0; object = object->next) {
        if (object->content != 0) {
            content_status status = content_get_status(object->content);
            if (status == CONTENT_STATUS_READY || status == CONTENT_STATUS_DONE) {
                ++*obj_loaded;
            }
        }
    }
}

static void netsurf_bridge_count_box_tree(const struct box *box,
                                          unsigned int depth,
                                          int parent_x,
                                          int parent_y,
                                          netsurf_bridge_box_stats_t *stats) {
    while (box != 0) {
        int abs_x = parent_x + box->x;
        int abs_y = parent_y + box->y;
        int right = abs_x + box->width;
        int bottom = abs_y + box->height;

        if (stats->boxes == 0u || abs_x < stats->min_left) {
            stats->min_left = abs_x;
        }
        if (stats->boxes == 0u || abs_y < stats->min_top) {
            stats->min_top = abs_y;
        }
        ++stats->boxes;
        if (depth > stats->max_depth) {
            stats->max_depth = depth;
        }
        if (box->type == BOX_TEXT && box->text != 0 && box->length != 0u) {
            ++stats->text_boxes;
            stats->text_bytes += (unsigned int)box->length;
        }
        if (box->object != 0 || box->background != 0) {
            ++stats->object_boxes;
        }
        if (box->type == BOX_FLEX || box->type == BOX_INLINE_FLEX) {
            ++stats->flex_boxes;
        }
        if (box->type == BOX_TABLE) {
            ++stats->table_boxes;
        }
        if (box->type == BOX_FLOAT_LEFT || box->type == BOX_FLOAT_RIGHT) {
            ++stats->float_boxes;
        }
        if (box->style != 0) {
            uint8_t display = css_computed_display(box->style, false);
            uint8_t position = css_computed_position(box->style);
            if (display == CSS_DISPLAY_GRID || display == CSS_DISPLAY_INLINE_GRID) {
                ++stats->grid_display_boxes;
            }
            if (position == CSS_POSITION_FIXED) {
                ++stats->fixed_boxes;
            } else if (position == CSS_POSITION_STICKY) {
                ++stats->sticky_boxes;
            } else if (position == CSS_POSITION_ABSOLUTE) {
                ++stats->absolute_boxes;
            } else if (position == CSS_POSITION_RELATIVE) {
                ++stats->relative_boxes;
            }
        }
        if (box->gadget != 0) {
            ++stats->form_controls;
        }
        if ((box->flags & IFRAME) != 0 || box->iframe != 0) {
            ++stats->iframe_boxes;
        }
        if (abs_x < 0 || abs_y < 0) {
            ++stats->negative_boxes;
        }
        if (box->width > 2400 || right > 3200) {
            ++stats->overwide_boxes;
        }
        if (right > stats->max_right) {
            stats->max_right = right;
        }
        if (bottom > stats->max_bottom) {
            stats->max_bottom = bottom;
        }
        if (box->children != 0) {
            ++stats->boxes_with_children;
            netsurf_bridge_count_box_tree(box->children,
                                          depth + 1u,
                                          abs_x,
                                          abs_y,
                                          stats);
        }
        if (box->list_marker != 0) {
            netsurf_bridge_count_box_tree(box->list_marker,
                                          depth + 1u,
                                          abs_x,
                                          abs_y,
                                          stats);
        }
        box = box->next;
    }
}

static void netsurf_bridge_html_box_stats(const struct content *content,
                                          netsurf_bridge_box_stats_t *stats) {
    const html_content *html;

    memset(stats, 0, sizeof(*stats));
    if (content == 0) {
        return;
    }
    html = (const html_content *)content;
    if (html->layout != 0) {
        netsurf_bridge_count_box_tree(html->layout, 0u, 0, 0, stats);
    }
}

static void netsurf_bridge_set_progress_status(const char *phase,
                                               const struct content *content,
                                               unsigned int ran) {
    unsigned int css_total;
    unsigned int css_loaded;
    unsigned int obj_total;
    unsigned int obj_loaded;

    netsurf_bridge_html_stats(content, &css_total, &css_loaded, &obj_total, &obj_loaded);
    snprintf(netsurf_bridge_status_buffer,
             sizeof(netsurf_bridge_status_buffer),
             "NetSurf: %s act %u sched %u fq %u fa %u css %u/%u obj %u/%u http %u/%u fail %u hit %u ce %u cb %u err %d bytes %u status %u img %u fb %u err %u step %u",
             phase != 0 ? phase : "poll",
             content != 0 ? content->active : 0u,
             netsurf_bridge_schedule_count(),
             0u,
             0u,
             css_loaded,
             css_total,
             obj_loaded,
             obj_total,
             netsurf_bridge_http_fetch_success_count,
             netsurf_bridge_http_fetch_count,
             netsurf_bridge_http_fetch_error_count,
             netsurf_bridge_resource_cache_hits,
             netsurf_bridge_resource_cache_entries,
             netsurf_bridge_resource_cache_bytes / 1024u,
             netsurf_bridge_http_last_error,
             netsurf_bridge_http_last_bytes,
             netsurf_bridge_http_last_status,
             netsurf_bridge_image_decode_count,
             netsurf_bridge_image_fallback_count,
             netsurf_bridge_image_error_count,
             ran);
    netsurf_bridge_status = netsurf_bridge_status_buffer;
}

static void netsurf_bridge_set_render_status(const char *phase,
                                             const struct content *content,
                                             const netsurf_kernel_plot_stats_t *plot_stats) {
    unsigned int css_total;
    unsigned int css_loaded;
    unsigned int obj_total;
    unsigned int obj_loaded;
    netsurf_bridge_box_stats_t box_stats;

    netsurf_bridge_html_stats(content, &css_total, &css_loaded, &obj_total, &obj_loaded);
    netsurf_bridge_html_box_stats(content, &box_stats);
    snprintf(netsurf_bridge_status_buffer,
             sizeof(netsurf_bridge_status_buffer),
             "NS %s b%u txt%u/%u objb%u flex%u g%u tbl%u flt%u form%u if%u pos%u/%u/%u/%u neg%u wide%u min%dx%d ext%dx%d bm%u/%u r%u css%u/%u cx%u/%u/%u obj%u/%u http%u/%u fail%u hit%u ce%u cb%u st%u img%u fb%u err%u",
             phase != 0 ? phase : "rendered",
             box_stats.boxes,
             plot_stats != 0 ? plot_stats->visible_texts : 0u,
             box_stats.text_boxes,
             box_stats.object_boxes,
             box_stats.flex_boxes,
             box_stats.grid_display_boxes,
             box_stats.table_boxes,
             box_stats.float_boxes,
             box_stats.form_controls,
             box_stats.iframe_boxes,
             box_stats.fixed_boxes,
             box_stats.sticky_boxes,
             box_stats.absolute_boxes,
             box_stats.relative_boxes,
             box_stats.negative_boxes,
             box_stats.overwide_boxes,
             box_stats.min_left,
             box_stats.min_top,
             box_stats.max_right,
             box_stats.max_bottom,
             plot_stats != 0 ? plot_stats->visible_bitmaps : 0u,
             plot_stats != 0 ? plot_stats->bitmaps : 0u,
             plot_stats != 0 ? plot_stats->rectangles : 0u,
             css_loaded,
             css_total,
             netsurf_bridge_css_compat_count,
             netsurf_bridge_css_compat_var_count,
             netsurf_bridge_css_compat_group_count,
             obj_loaded,
             obj_total,
             netsurf_bridge_http_fetch_success_count,
             netsurf_bridge_http_fetch_count,
             netsurf_bridge_http_fetch_error_count,
             netsurf_bridge_resource_cache_hits,
             netsurf_bridge_resource_cache_entries,
             netsurf_bridge_resource_cache_bytes / 1024u,
             netsurf_bridge_http_last_status,
             netsurf_bridge_image_decode_count,
             netsurf_bridge_image_fallback_count,
             netsurf_bridge_image_error_count);
    netsurf_bridge_status = netsurf_bridge_status_buffer;
}

static uint32_t netsurf_bridge_signature(const uint8_t *data, uint32_t len) {
    uint32_t hash = 2166136261u;
    uint32_t step;
    uint32_t i;

    if (data == 0) {
        return 0;
    }
    hash ^= len;
    hash *= 16777619u;
    step = len > 4096u ? (len / 64u) : 1u;
    for (i = 0; i < len; i += step) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    if (len != 0u) {
        uint32_t tail = len > 256u ? len - 256u : 0u;
        for (i = tail; i < len; ++i) {
            hash ^= data[i];
            hash *= 16777619u;
        }
    }
    return hash;
}

static void netsurf_bridge_set_waiting_status(const struct content *content) {
    const char *status = "unknown";
    unsigned int css_total;
    unsigned int css_loaded;
    unsigned int obj_total;
    unsigned int obj_loaded;

    if (content != 0 &&
        content->status >= CONTENT_STATUS_LOADING &&
        content->status <= CONTENT_STATUS_ERROR) {
        status = content_status_name[content->status];
    }
    netsurf_bridge_html_stats(content, &css_total, &css_loaded, &obj_total, &obj_loaded);
    snprintf(netsurf_bridge_status_buffer,
             sizeof(netsurf_bridge_status_buffer),
             "NetSurf: waiting %s act %u sched %u fq %u fa %u css %u/%u obj %u/%u http %u/%u fail %u hit %u ce %u cb %u err %d bytes %u status %u img %u fb %u err %u",
             status,
             content != 0 ? content->active : 0u,
             netsurf_bridge_schedule_count(),
             0u,
             0u,
             css_loaded,
             css_total,
             obj_loaded,
             obj_total,
             netsurf_bridge_http_fetch_success_count,
             netsurf_bridge_http_fetch_count,
             netsurf_bridge_http_fetch_error_count,
             netsurf_bridge_resource_cache_hits,
             netsurf_bridge_resource_cache_entries,
             netsurf_bridge_resource_cache_bytes / 1024u,
             netsurf_bridge_http_last_error,
             netsurf_bridge_http_last_bytes,
             netsurf_bridge_http_last_status,
             netsurf_bridge_image_decode_count,
             netsurf_bridge_image_fallback_count,
             netsurf_bridge_image_error_count);
    netsurf_bridge_status = netsurf_bridge_status_buffer;
}

void netsurf_core_invalidate_cache(void) {
    netsurf_bridge_schedule_clear();
    netsurf_bridge_http_fetch_count = 0;
    netsurf_bridge_http_fetch_success_count = 0;
    netsurf_bridge_http_fetch_error_count = 0;
    netsurf_bridge_resource_cache_hits = 0;
    netsurf_bridge_resource_cache_stores = 0;
    netsurf_bridge_resource_cache_evictions = 0;
    netsurf_bridge_http_last_error = 0;
    netsurf_bridge_http_last_status = 0;
    netsurf_bridge_http_last_bytes = 0;
    netsurf_bridge_http_next_fetch_tick = 0;
    netsurf_bridge_image_decode_count = 0;
    netsurf_bridge_image_error_count = 0;
    netsurf_bridge_image_fallback_count = 0;
    netsurf_bridge_css_compat_count = 0;
    netsurf_bridge_css_compat_var_count = 0;
    netsurf_bridge_css_compat_group_count = 0;
    netsurf_bridge_navigation_pending = false;
    netsurf_bridge_pending_url[0] = '\0';
    if (netsurf_bridge_cached_document == 0) {
        return;
    }
    if (netsurf_bridge_cached_document->opened &&
        netsurf_bridge_cached_document->content != 0 &&
        netsurf_bridge_cached_document->content->handler != 0 &&
        netsurf_bridge_cached_document->content->handler->close != 0) {
        (void)netsurf_bridge_cached_document->content->handler->close(
            netsurf_bridge_cached_document->content);
        netsurf_bridge_cached_document->opened = false;
    }
    netsurf_bridge_destroy_content(netsurf_bridge_cached_document->content);
    if (netsurf_bridge_cached_document->url != 0) {
        nsurl_unref(netsurf_bridge_cached_document->url);
    }
    if (netsurf_bridge_cached_document->jsheap != 0) {
        js_destroyheap(netsurf_bridge_cached_document->jsheap);
    }
    free(netsurf_bridge_cached_document);
    netsurf_bridge_cached_document = 0;
}

static nserror netsurf_bridge_schedule(int t, void (*callback)(void *p), void *p) {
    size_t i;
    size_t free_slot = NETSURF_BRIDGE_SCHEDULE_MAX;

    if (callback == 0) {
        return NSERROR_OK;
    }
    if (t < 0) {
        netsurf_bridge_schedule_remove(callback, p);
        return NSERROR_OK;
    }
    for (i = 0; i < NETSURF_BRIDGE_SCHEDULE_MAX; ++i) {
        if (netsurf_bridge_schedule_queue[i].active) {
            if (netsurf_bridge_schedule_queue[i].callback == callback &&
                netsurf_bridge_schedule_queue[i].p == p) {
                return NSERROR_OK;
            }
        } else if (free_slot == NETSURF_BRIDGE_SCHEDULE_MAX) {
            free_slot = i;
        }
    }
    if (free_slot == NETSURF_BRIDGE_SCHEDULE_MAX) {
        return NSERROR_NOMEM;
    }

    netsurf_bridge_schedule_queue[free_slot].active = true;
    netsurf_bridge_schedule_queue[free_slot].callback = callback;
    netsurf_bridge_schedule_queue[free_slot].p = p;
    return NSERROR_OK;
}

static unsigned int netsurf_bridge_run_scheduled_budget(unsigned int callback_limit) {
    unsigned int iterations = 0;
    unsigned int ran = 0;

    if (netsurf_bridge_schedule_draining) {
        return 0;
    }
    if (callback_limit == 0u) {
        callback_limit = NETSURF_BRIDGE_SCHEDULE_PUMP_LIMIT;
    }
    netsurf_bridge_schedule_draining = true;
    while (iterations++ < callback_limit) {
        size_t i;
        void (*callback)(void *p) = 0;
        void *p = 0;

        for (i = 0; i < NETSURF_BRIDGE_SCHEDULE_MAX; ++i) {
            if (netsurf_bridge_schedule_queue[i].active) {
                callback = netsurf_bridge_schedule_queue[i].callback;
                p = netsurf_bridge_schedule_queue[i].p;
                netsurf_bridge_schedule_queue[i].active = false;
                break;
            }
        }
        if (callback == 0) {
            break;
        }
        callback(p);
        ++ran;
    }
    netsurf_bridge_schedule_draining = false;
    return ran;
}

static void netsurf_bridge_run_scheduled(void) {
    (void)netsurf_bridge_run_scheduled_budget(NETSURF_BRIDGE_SCHEDULE_PUMP_LIMIT);
}

static void netsurf_bridge_run_scheduled_until_ready(struct content *content) {
    unsigned int rounds = 0;
    unsigned int stalled = 0;
    unsigned int last_pending = 0xffffffffu;

    while (content != 0 &&
           content->status != CONTENT_STATUS_READY &&
           content->status != CONTENT_STATUS_DONE &&
           rounds++ < NETSURF_BRIDGE_PREPARE_ROUND_LIMIT) {
        unsigned int pending = netsurf_bridge_schedule_count() +
                               netsurf_bridge_fetch_pending_count() +
                               netsurf_bridge_http_pending_count() +
                               content->active;
        unsigned int ran;

        if (pending == 0u) {
            break;
        }
        ran = netsurf_bridge_run_scheduled_budget(NETSURF_BRIDGE_PREPARE_CALLBACK_BUDGET);
        ran += netsurf_bridge_pump_fetchers();
        if (pending == last_pending) {
            ++stalled;
        } else {
            stalled = 0;
            last_pending = pending;
        }
        if (ran == 0u || stalled >= NETSURF_BRIDGE_PREPARE_STALL_LIMIT) {
            break;
        }
    }
}

static void netsurf_bridge_http_send_callback(const fetch_msg *msg,
                                              netsurf_bridge_http_fetch_t *ctx) {
    ctx->locked = true;
    fetch_send_callback(msg, ctx->fetch);
    ctx->locked = false;
}

static void netsurf_bridge_http_send_header(netsurf_bridge_http_fetch_t *ctx,
                                            const char *header) {
    fetch_msg msg;

    msg.type = FETCH_HEADER;
    msg.data.header_or_data.buf = (const uint8_t *)header;
    msg.data.header_or_data.len = strlen(header);
    netsurf_bridge_http_send_callback(&msg, ctx);
}

static bool netsurf_bridge_http_serve_cache(netsurf_bridge_http_fetch_t *ctx) {
    netsurf_bridge_resource_cache_entry_t *entry;
    fetch_msg msg;
    char header[128];

    if (ctx == 0 || ctx->url == 0 || !ctx->cacheable || ctx->aborted) {
        return false;
    }
    entry = netsurf_bridge_resource_cache_find(nsurl_access(ctx->url));
    if (entry == 0 || entry->data == 0 || entry->content_type == 0) {
        return false;
    }

    fetch_set_http_code(ctx->fetch, 200);
    snprintf(header, sizeof(header), "Content-Type: %s", entry->content_type);
    netsurf_bridge_http_send_header(ctx, header);
    snprintf(header, sizeof(header), "Content-Length: %u", entry->size);
    if (!ctx->aborted) {
        netsurf_bridge_http_send_header(ctx, header);
    }
    if (!ctx->aborted) {
        msg.type = FETCH_DATA;
        msg.data.header_or_data.buf = entry->data;
        msg.data.header_or_data.len = entry->size;
        netsurf_bridge_http_send_callback(&msg, ctx);
    }
    if (!ctx->aborted) {
        msg.type = FETCH_FINISHED;
        netsurf_bridge_http_send_callback(&msg, ctx);
    }
    ++netsurf_bridge_resource_cache_hits;
    ++netsurf_bridge_http_fetch_success_count;
    netsurf_bridge_http_last_error = 0;
    netsurf_bridge_http_last_status = 200;
    netsurf_bridge_http_last_bytes = entry->size;
    return true;
}

static bool netsurf_bridge_http_initialise(lwc_string *scheme) {
    (void)scheme;
    return true;
}

static void netsurf_bridge_http_finalise(lwc_string *scheme) {
    (void)scheme;
}

static bool netsurf_bridge_http_can_fetch(const nsurl *url) {
    (void)url;
    return true;
}

static void *netsurf_bridge_http_setup(struct fetch *parent_fetch,
                                       nsurl *url,
                                       bool only_2xx,
                                       bool downgrade_tls,
                                       const char *post_urlenc,
                                       const struct fetch_multipart_data *post_multipart,
                                       const char **headers) {
    netsurf_bridge_http_fetch_t *ctx;
    netsurf_bridge_http_fetch_t **slot;

    (void)only_2xx;
    (void)downgrade_tls;
    (void)post_urlenc;
    (void)post_multipart;
    (void)headers;

    ctx = calloc(1, sizeof(*ctx));
    if (ctx == 0) {
        return 0;
    }
    ctx->fetch = parent_fetch;
    ctx->url = nsurl_ref(url);
    ctx->cacheable = post_urlenc == 0 && post_multipart == 0;
    slot = &netsurf_bridge_http_fetches;
    while (*slot != 0) {
        slot = &(*slot)->next;
    }
    *slot = ctx;
    return ctx;
}

static bool netsurf_bridge_http_start(void *fetch) {
    (void)fetch;
    return true;
}

static void netsurf_bridge_http_abort(void *fetch) {
    netsurf_bridge_http_fetch_t *ctx = fetch;
    if (ctx != 0) {
        ctx->aborted = true;
    }
}

static void netsurf_bridge_http_free(void *fetch) {
    netsurf_bridge_http_fetch_t *ctx = fetch;
    netsurf_bridge_http_fetch_t **slot = &netsurf_bridge_http_fetches;

    while (*slot != 0) {
        if (*slot == ctx) {
            *slot = ctx->next;
            break;
        }
        slot = &(*slot)->next;
    }
    if (ctx != 0) {
        if (ctx->url != 0) {
            nsurl_unref(ctx->url);
        }
        free(ctx);
    }
}

static void netsurf_bridge_http_poll(lwc_string *scheme) {
    netsurf_bridge_http_fetch_t *ctx;
    uint64_t now = timer_ticks();
    uint32_t handled = 0;

    (void)scheme;
    if (netsurf_bridge_http_next_fetch_tick != 0 &&
        now < netsurf_bridge_http_next_fetch_tick) {
        return;
    }
    ctx = netsurf_bridge_http_select_fetch(now);
    while (ctx != 0 && handled < NETSURF_BRIDGE_HTTP_FETCHES_PER_POLL) {
        fetch_msg msg;
        net_http_info_t info;
        char *buffer;
        char *compat_buffer;
        char header[128];
        const char *content_type;
        const char *payload;
        uint32_t byte_limit;
        uint32_t body_size;
        uint32_t payload_size;
        uint32_t compat_vars;
        uint32_t compat_groups;
        int rc;

        if (ctx->locked ||
            (ctx->retry_after_tick != 0 && now < ctx->retry_after_tick)) {
            ctx = netsurf_bridge_http_select_fetch(now);
            continue;
        }

        if (netsurf_bridge_http_serve_cache(ctx)) {
            fetch_remove_from_queues(ctx->fetch);
            fetch_free(ctx->fetch);
            ++handled;
            ctx = netsurf_bridge_http_select_fetch(now);
            continue;
        }

        if (!ctx->aborted && netsurf_bridge_http_fetch_count < NETSURF_BRIDGE_HTTP_MAX_FETCHES) {
            ++netsurf_bridge_http_fetch_count;
            ++ctx->attempt_count;
            byte_limit = netsurf_bridge_http_byte_limit();
            buffer = malloc(byte_limit + 1u);
            if (buffer != 0) {
                rc = shell_api_http_get_ex(nsurl_access(ctx->url),
                                           buffer,
                                           byte_limit,
                                           &info);
                netsurf_bridge_http_last_error = info.error;
                netsurf_bridge_http_last_status = info.status_code;
                body_size = info.body_size != 0u ? info.body_size : (rc > 0 ? (uint32_t)rc : 0u);
                netsurf_bridge_http_last_bytes = body_size;
                if (rc >= 0 &&
                    info.status_code == 429u &&
                    ctx->attempt_count < NETSURF_BRIDGE_HTTP_429_RETRY_LIMIT &&
                    !ctx->aborted) {
                    ctx->retry_after_tick = now +
                        netsurf_bridge_ms_to_ticks(NETSURF_BRIDGE_HTTP_429_BACKOFF_MS *
                                                   ctx->attempt_count);
                    netsurf_bridge_http_next_fetch_tick = ctx->retry_after_tick;
                    free(buffer);
                    ++handled;
                    ctx = netsurf_bridge_http_select_fetch(now);
                    continue;
                }
                if (rc >= 0 &&
                    (info.status_code == 0u || info.status_code < 400u) &&
                    !ctx->aborted) {
                    if (body_size > byte_limit) {
                        body_size = byte_limit;
                    }
                    buffer[body_size] = 0;
                    fetch_set_http_code(ctx->fetch,
                                        info.status_code != 0u ? info.status_code : 200u);
                    content_type = info.content_type[0] != 0 ?
                        info.content_type :
                        netsurf_bridge_filetype(nsurl_access(ctx->url));
                    payload = buffer;
                    payload_size = body_size;
                    compat_buffer = 0;
                    if (netsurf_bridge_is_css_response(content_type, nsurl_access(ctx->url))) {
                        compat_buffer = netsurf_bridge_css_compat_filter(buffer,
                                                                         body_size,
                                                                         &payload_size,
                                                                         &compat_vars,
                                                                         &compat_groups);
                        if (compat_buffer != 0) {
                            payload = compat_buffer;
                            ++netsurf_bridge_css_compat_count;
                            netsurf_bridge_css_compat_var_count += compat_vars;
                            netsurf_bridge_css_compat_group_count += compat_groups;
                        }
                    }
                    snprintf(header, sizeof(header), "Content-Type: %s", content_type);
                    netsurf_bridge_http_send_header(ctx, header);
                    snprintf(header, sizeof(header), "Content-Length: %u", payload_size);
                    if (!ctx->aborted) {
                        netsurf_bridge_http_send_header(ctx, header);
                    }
                    if (!ctx->aborted) {
                        msg.type = FETCH_DATA;
                        msg.data.header_or_data.buf = (const uint8_t *)payload;
                        msg.data.header_or_data.len = payload_size;
                        netsurf_bridge_http_send_callback(&msg, ctx);
                    }
                    if (!ctx->aborted) {
                        msg.type = FETCH_FINISHED;
                        netsurf_bridge_http_send_callback(&msg, ctx);
                    }
                    if (!ctx->aborted && ctx->cacheable) {
                        netsurf_bridge_resource_cache_store(nsurl_access(ctx->url),
                                                            content_type,
                                                            (const uint8_t *)payload,
                                                            payload_size);
                    }
                    if (compat_buffer != 0) {
                        free(compat_buffer);
                    }
                    ++netsurf_bridge_http_fetch_success_count;
                } else if (!ctx->aborted) {
                    ++netsurf_bridge_http_fetch_error_count;
                    msg.type = FETCH_ERROR;
                    msg.data.error = info.status_code == 429u ?
                        "kernel HTTP fetch rate limited" :
                        "kernel HTTP fetch failed";
                    netsurf_bridge_http_send_callback(&msg, ctx);
                }
                free(buffer);
            } else if (!ctx->aborted) {
                netsurf_bridge_http_last_error = -11;
                ++netsurf_bridge_http_fetch_error_count;
                msg.type = FETCH_ERROR;
                msg.data.error = "kernel HTTP fetch allocation failed";
                netsurf_bridge_http_send_callback(&msg, ctx);
            }
        } else if (!ctx->aborted) {
            netsurf_bridge_http_last_error = -12;
            ++netsurf_bridge_http_fetch_error_count;
            msg.type = FETCH_ERROR;
            msg.data.error = "kernel HTTP fetch limit reached";
            netsurf_bridge_http_send_callback(&msg, ctx);
        }

        netsurf_bridge_http_next_fetch_tick =
            timer_ticks() + netsurf_bridge_ms_to_ticks(NETSURF_BRIDGE_HTTP_SPACING_MS);
        fetch_remove_from_queues(ctx->fetch);
        fetch_free(ctx->fetch);
        ++handled;
        ctx = netsurf_bridge_http_select_fetch(now);
    }
}

static nserror netsurf_bridge_http_register_scheme(const char *name) {
    static const struct fetcher_operation_table ops = {
        .initialise = netsurf_bridge_http_initialise,
        .acceptable = netsurf_bridge_http_can_fetch,
        .setup = netsurf_bridge_http_setup,
        .start = netsurf_bridge_http_start,
        .abort = netsurf_bridge_http_abort,
        .free = netsurf_bridge_http_free,
        .poll = netsurf_bridge_http_poll,
        .fdset = 0,
        .finalise = netsurf_bridge_http_finalise,
    };
    lwc_string *scheme = 0;

    if (lwc_intern_string(name, strlen(name), &scheme) != lwc_error_ok) {
        return NSERROR_NOMEM;
    }
    return fetcher_add(scheme, &ops);
}

static nserror netsurf_bridge_launch_url(struct nsurl *url) {
    (void)url;
    return NSERROR_NOT_IMPLEMENTED;
}

static int netsurf_bridge_extension_is(const char *dot, const char *ext) {
    size_t i = 0;

    if (dot == 0 || ext == 0) {
        return 0;
    }
    while (ext[i] != 0) {
        char a = dot[i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a + ('a' - 'A'));
        }
        if (a != b) {
            return 0;
        }
        ++i;
    }
    return dot[i] == 0 || dot[i] == '?' || dot[i] == '#' || dot[i] == '&';
}

static const char *netsurf_bridge_filetype(const char *path) {
    const char *scan = path != 0 ? path : "";
    const char *dot = 0;

    if (netsurf_bridge_url_contains(path, "only=styles") ||
        netsurf_bridge_url_contains(path, "type=text/css")) {
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
    if (netsurf_bridge_extension_is(dot, ".css")) {
        return "text/css";
    }
    if (netsurf_bridge_extension_is(dot, ".html") || netsurf_bridge_extension_is(dot, ".htm")) {
        return "text/html";
    }
    if (netsurf_bridge_extension_is(dot, ".png")) {
        return "image/png";
    }
    if (netsurf_bridge_extension_is(dot, ".jpg") || netsurf_bridge_extension_is(dot, ".jpeg")) {
        return "image/jpeg";
    }
    if (netsurf_bridge_extension_is(dot, ".gif")) {
        return "image/gif";
    }
    if (netsurf_bridge_extension_is(dot, ".bmp")) {
        return "image/bmp";
    }
    if (netsurf_bridge_extension_is(dot, ".webp")) {
        return "image/webp";
    }
    if (netsurf_bridge_extension_is(dot, ".ico")) {
        return "image/x-icon";
    }
    if (netsurf_bridge_extension_is(dot, ".svg")) {
        return "image/svg+xml";
    }
    return "text/plain";
}

static nserror netsurf_bridge_resource_data(const char *path,
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
        *data = (const uint8_t *)netsurf_bridge_empty_css;
        *data_len = 0u;
        return NSERROR_OK;
    }
    *data = 0;
    *data_len = 0;
    return NSERROR_NOT_FOUND;
}

static struct nsurl *netsurf_bridge_resource_url(const char *path) {
    (void)path;
    return 0;
}

static nserror netsurf_bridge_release_resource_data(const uint8_t *data) {
    (void)data;
    return NSERROR_OK;
}

static nserror netsurf_bridge_llcache_initialise(const struct llcache_store_parameters *parameters) {
    (void)parameters;
    return NSERROR_OK;
}

static nserror netsurf_bridge_llcache_finalise(void) {
    return NSERROR_OK;
}

static nserror netsurf_bridge_llcache_store(struct nsurl *url,
                                            enum backing_store_flags flags,
                                            uint8_t *data,
                                            const size_t datalen) {
    (void)url;
    (void)flags;
    (void)data;
    (void)datalen;
    return NSERROR_SAVE_FAILED;
}

static nserror netsurf_bridge_llcache_fetch(struct nsurl *url,
                                            enum backing_store_flags flags,
                                            uint8_t **data_out,
                                            size_t *datalen_out) {
    (void)url;
    (void)flags;
    if (data_out != 0) {
        *data_out = 0;
    }
    if (datalen_out != 0) {
        *datalen_out = 0;
    }
    return NSERROR_NOT_FOUND;
}

static nserror netsurf_bridge_llcache_release(struct nsurl *url, enum backing_store_flags flags) {
    (void)url;
    (void)flags;
    return NSERROR_NOT_FOUND;
}

static nserror netsurf_bridge_llcache_invalidate(struct nsurl *url) {
    (void)url;
    return NSERROR_NOT_FOUND;
}

static void *netsurf_bridge_bitmap_create(int width, int height, enum gui_bitmap_flags flags) {
    netsurf_bridge_bitmap_t *bitmap;
    size_t bytes;

    if (width <= 0 || height <= 0) {
        return 0;
    }
    bitmap = calloc(1, sizeof(*bitmap));
    if (bitmap == 0) {
        return 0;
    }
    bytes = (size_t)width * (size_t)height * 4u;
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

static void netsurf_bridge_bitmap_destroy(void *bitmap) {
    netsurf_bridge_bitmap_t *b = bitmap;
    if (b != 0) {
        free(b->pixels);
        free(b);
    }
}

static void netsurf_bridge_bitmap_set_opaque(void *bitmap, bool opaque) {
    if (bitmap != 0) {
        ((netsurf_bridge_bitmap_t *)bitmap)->opaque = opaque;
    }
}

static bool netsurf_bridge_bitmap_get_opaque(void *bitmap) {
    return bitmap != 0 ? ((netsurf_bridge_bitmap_t *)bitmap)->opaque : false;
}

static unsigned char *netsurf_bridge_bitmap_get_buffer(void *bitmap) {
    return bitmap != 0 ? ((netsurf_bridge_bitmap_t *)bitmap)->pixels : 0;
}

static size_t netsurf_bridge_bitmap_get_rowstride(void *bitmap) {
    return bitmap != 0 ? (size_t)((netsurf_bridge_bitmap_t *)bitmap)->width * 4u : 0;
}

static int netsurf_bridge_bitmap_get_width(void *bitmap) {
    return bitmap != 0 ? ((netsurf_bridge_bitmap_t *)bitmap)->width : 0;
}

static int netsurf_bridge_bitmap_get_height(void *bitmap) {
    return bitmap != 0 ? ((netsurf_bridge_bitmap_t *)bitmap)->height : 0;
}

static void netsurf_bridge_bitmap_modified(void *bitmap) {
    (void)bitmap;
}

static nserror netsurf_bridge_bitmap_render(struct bitmap *bitmap, struct hlcache_handle *content) {
    (void)bitmap;
    (void)content;
    return NSERROR_NOT_IMPLEMENTED;
}

int netsurf_kernel_bitmap_width(struct bitmap *bitmap) {
    return bitmap != 0 ? ((netsurf_bridge_bitmap_t *)bitmap)->width : 0;
}

int netsurf_kernel_bitmap_height(struct bitmap *bitmap) {
    return bitmap != 0 ? ((netsurf_bridge_bitmap_t *)bitmap)->height : 0;
}

int netsurf_kernel_bitmap_rowstride(struct bitmap *bitmap) {
    return bitmap != 0 ? (int)((netsurf_bridge_bitmap_t *)bitmap)->width * 4 : 0;
}

int netsurf_kernel_bitmap_opaque(struct bitmap *bitmap) {
    return bitmap != 0 && ((netsurf_bridge_bitmap_t *)bitmap)->opaque;
}

uint8_t *netsurf_kernel_bitmap_buffer(struct bitmap *bitmap) {
    return bitmap != 0 ? ((netsurf_bridge_bitmap_t *)bitmap)->pixels : 0;
}

static bool netsurf_bridge_image_placeholder(netsurf_bridge_image_content_t *image,
                                             int width,
                                             int height,
                                             uint32_t fill,
                                             uint32_t stroke) {
    uint8_t *dst;
    int rowstride;
    int x;
    int y;

    if (width < 32) {
        width = 32;
    } else if (width > 240) {
        width = 240;
    }
    if (height < 24) {
        height = 24;
    } else if (height > 160) {
        height = 160;
    }

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

    for (y = 0; y < height; ++y) {
        uint8_t *row = dst + (size_t)y * (size_t)rowstride;
        for (x = 0; x < width; ++x) {
            uint32_t rgb = fill;
            if (x == 0 || y == 0 || x == width - 1 || y == height - 1 ||
                x == y || x == width - 1 - y) {
                rgb = stroke;
            }
            row[x * 4] = (uint8_t)((rgb >> 16) & 0xffu);
            row[x * 4 + 1] = (uint8_t)((rgb >> 8) & 0xffu);
            row[x * 4 + 2] = (uint8_t)(rgb & 0xffu);
            row[x * 4 + 3] = 0xffu;
        }
    }
    guit->bitmap->set_opaque(image->bitmap, true);
    guit->bitmap->modified(image->bitmap);
    image->base.width = width;
    image->base.height = height;
    image->base.size += (unsigned int)((size_t)width * (size_t)height * 4u);
    content_set_ready(&image->base);
    content_set_done(&image->base);
    content_set_status(&image->base, "");
    return true;
}

static bool netsurf_bridge_image_decode(netsurf_bridge_image_content_t *image) {
    const uint8_t *source;
    uint8_t *rgba;
    uint8_t *dst;
    image_info_t info;
    size_t source_size;
    size_t rgba_bytes;
    int rowstride;
    bool opaque = true;
    uint32_t x;
    uint32_t y;

    source = content__get_source_data(&image->base, &source_size);
    if (source == 0 || source_size == 0u || source_size > 0xffffffffu) {
        content_broadcast_error(&image->base, NSERROR_UNKNOWN, "image source unavailable");
        ++netsurf_bridge_image_error_count;
        return false;
    }
    if (image_probe(source, (uint32_t)source_size, &info) != IMAGE_OK ||
        info.width == 0u ||
        info.height == 0u ||
        info.width > 4096u ||
        info.height > 4096u ||
        info.width > 0xffffffffu / info.height ||
        info.width * info.height > 4096u * 4096u) {
        ++netsurf_bridge_image_error_count;
        if (netsurf_bridge_image_placeholder(image, 96, 64, 0xf0f3f5u, 0x7b8794u)) {
            ++netsurf_bridge_image_decode_count;
            ++netsurf_bridge_image_fallback_count;
            return true;
        }
        content_broadcast_error(&image->base, NSERROR_UNKNOWN, "image unsupported");
        return false;
    }

    rgba_bytes = (size_t)info.width * (size_t)info.height * 4u;
    if (rgba_bytes > 32u * 1024u * 1024u) {
        content_broadcast_error(&image->base, NSERROR_NOMEM, "image too large");
        ++netsurf_bridge_image_error_count;
        return false;
    }
    rgba = malloc(rgba_bytes);
    if (rgba == 0) {
        content_broadcast_error(&image->base, NSERROR_NOMEM, "image decode memory");
        ++netsurf_bridge_image_error_count;
        return false;
    }
    if (image_decode_rgba32(source,
                            (uint32_t)source_size,
                            rgba,
                            (uint32_t)rgba_bytes,
                            &info) != IMAGE_OK) {
        free(rgba);
        ++netsurf_bridge_image_error_count;
        if (netsurf_bridge_image_placeholder(image, 96, 64, 0xf0f3f5u, 0x7b8794u)) {
            ++netsurf_bridge_image_decode_count;
            ++netsurf_bridge_image_fallback_count;
            return true;
        }
        content_broadcast_error(&image->base, NSERROR_UNKNOWN, "image decode failed");
        return false;
    }

    image->bitmap = (struct bitmap *)guit->bitmap->create((int)info.width,
                                                          (int)info.height,
                                                          BITMAP_CLEAR);
    if (image->bitmap == 0) {
        free(rgba);
        content_broadcast_error(&image->base, NSERROR_NOMEM, "image bitmap memory");
        ++netsurf_bridge_image_error_count;
        return false;
    }

    dst = guit->bitmap->get_buffer(image->bitmap);
    rowstride = (int)guit->bitmap->get_rowstride(image->bitmap);
    if (dst == 0 || rowstride <= 0) {
        guit->bitmap->destroy(image->bitmap);
        image->bitmap = 0;
        free(rgba);
        content_broadcast_error(&image->base, NSERROR_NOMEM, "image bitmap unavailable");
        ++netsurf_bridge_image_error_count;
        return false;
    }

    for (y = 0; y < info.height; ++y) {
        uint8_t *row = dst + (size_t)y * (size_t)rowstride;
        const uint8_t *src = rgba + (size_t)y * (size_t)info.width * 4u;
        for (x = 0; x < info.width; ++x) {
            row[x * 4u] = src[x * 4u];
            row[x * 4u + 1u] = src[x * 4u + 1u];
            row[x * 4u + 2u] = src[x * 4u + 2u];
            row[x * 4u + 3u] = src[x * 4u + 3u];
            if (row[x * 4u + 3u] != 0xffu) {
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
    content_set_ready(&image->base);
    content_set_done(&image->base);
    content_set_status(&image->base, "");
    ++netsurf_bridge_image_decode_count;
    return true;
}

static nserror netsurf_bridge_image_create(const content_handler *handler,
                                           lwc_string *imime_type,
                                           const struct http_parameter *params,
                                           struct llcache_handle *llcache,
                                           const char *fallback_charset,
                                           bool quirks,
                                           struct content **c) {
    netsurf_bridge_image_content_t *image;
    nserror err;

    image = calloc(1, sizeof(*image));
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

static bool netsurf_bridge_image_complete(struct content *c) {
    return netsurf_bridge_image_decode((netsurf_bridge_image_content_t *)c);
}

static bool netsurf_bridge_image_redraw(struct content *c,
                                        struct content_redraw_data *data,
                                        const struct rect *clip,
                                        const struct redraw_context *ctx) {
    netsurf_bridge_image_content_t *image = (netsurf_bridge_image_content_t *)c;
    bitmap_flags_t flags = BITMAPF_NONE;

    (void)clip;
    if (image->bitmap == 0 || data == 0 || ctx == 0 || ctx->plot == 0 || ctx->plot->bitmap == 0) {
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

static void netsurf_bridge_image_destroy(struct content *c) {
    netsurf_bridge_image_content_t *image = (netsurf_bridge_image_content_t *)c;
    if (image->bitmap != 0) {
        guit->bitmap->destroy(image->bitmap);
        image->bitmap = 0;
    }
}

static nserror netsurf_bridge_image_clone(const struct content *old, struct content **newc) {
    const netsurf_bridge_image_content_t *old_image = (const netsurf_bridge_image_content_t *)old;
    netsurf_bridge_image_content_t *image;
    nserror err;

    image = calloc(1, sizeof(*image));
    if (image == 0) {
        return NSERROR_NOMEM;
    }
    err = content__clone(old, &image->base);
    if (err != NSERROR_OK) {
        content_destroy(&image->base);
        return err;
    }
    if (old_image->bitmap != 0) {
        int width = guit->bitmap->get_width(old_image->bitmap);
        int height = guit->bitmap->get_height(old_image->bitmap);
        int rowstride = (int)guit->bitmap->get_rowstride(old_image->bitmap);
        const uint8_t *src = guit->bitmap->get_buffer(old_image->bitmap);
        uint8_t *dst;
        int y;

        image->bitmap = (struct bitmap *)guit->bitmap->create(width, height, BITMAP_CLEAR);
        dst = image->bitmap != 0 ? guit->bitmap->get_buffer(image->bitmap) : 0;
        if (dst == 0 || src == 0 || rowstride <= 0) {
            content_destroy(&image->base);
            return NSERROR_NOMEM;
        }
        for (y = 0; y < height; ++y) {
            memcpy(dst + (size_t)y * (size_t)rowstride,
                   src + (size_t)y * (size_t)rowstride,
                   (size_t)rowstride);
        }
        guit->bitmap->set_opaque(image->bitmap, guit->bitmap->get_opaque(old_image->bitmap));
        guit->bitmap->modified(image->bitmap);
    } else if (!netsurf_bridge_image_decode(image)) {
        content_destroy(&image->base);
        return NSERROR_UNKNOWN;
    }
    *newc = &image->base;
    return NSERROR_OK;
}

static content_type netsurf_bridge_image_type(void) {
    return CONTENT_IMAGE;
}

static void *netsurf_bridge_image_internal(const struct content *c, void *context) {
    (void)context;
    return ((const netsurf_bridge_image_content_t *)c)->bitmap;
}

static bool netsurf_bridge_image_opaque(struct content *c) {
    const netsurf_bridge_image_content_t *image = (const netsurf_bridge_image_content_t *)c;
    return image->bitmap != 0 && guit->bitmap->get_opaque(image->bitmap);
}

static const content_handler netsurf_bridge_image_handler = {
    .create = netsurf_bridge_image_create,
    .data_complete = netsurf_bridge_image_complete,
    .destroy = netsurf_bridge_image_destroy,
    .redraw = netsurf_bridge_image_redraw,
    .clone = netsurf_bridge_image_clone,
    .get_internal = netsurf_bridge_image_internal,
    .type = netsurf_bridge_image_type,
    .is_opaque = netsurf_bridge_image_opaque,
};

static nserror netsurf_bridge_image_init(void) {
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
    };
    size_t i;
    nserror err;

    for (i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        err = content_factory_register_handler(types[i], &netsurf_bridge_image_handler);
        if (err != NSERROR_OK) {
            return err;
        }
    }
    return NSERROR_OK;
}

static struct gui_misc_table netsurf_bridge_misc = {
    .schedule = netsurf_bridge_schedule,
    .launch_url = netsurf_bridge_launch_url,
};

static struct gui_fetch_table netsurf_bridge_fetch = {
    .filetype = netsurf_bridge_filetype,
    .get_resource_url = netsurf_bridge_resource_url,
    .get_resource_data = netsurf_bridge_resource_data,
    .release_resource_data = netsurf_bridge_release_resource_data,
};

static struct gui_bitmap_table netsurf_bridge_bitmap = {
    .create = netsurf_bridge_bitmap_create,
    .destroy = netsurf_bridge_bitmap_destroy,
    .set_opaque = netsurf_bridge_bitmap_set_opaque,
    .get_opaque = netsurf_bridge_bitmap_get_opaque,
    .get_buffer = netsurf_bridge_bitmap_get_buffer,
    .get_rowstride = netsurf_bridge_bitmap_get_rowstride,
    .get_width = netsurf_bridge_bitmap_get_width,
    .get_height = netsurf_bridge_bitmap_get_height,
    .modified = netsurf_bridge_bitmap_modified,
    .render = netsurf_bridge_bitmap_render,
};

static struct gui_llcache_table netsurf_bridge_llcache = {
    .initialise = netsurf_bridge_llcache_initialise,
    .finalise = netsurf_bridge_llcache_finalise,
    .store = netsurf_bridge_llcache_store,
    .fetch = netsurf_bridge_llcache_fetch,
    .release = netsurf_bridge_llcache_release,
    .invalidate = netsurf_bridge_llcache_invalidate,
};

static struct netsurf_table netsurf_bridge_table = {
    .misc = &netsurf_bridge_misc,
    .fetch = &netsurf_bridge_fetch,
    .llcache = &netsurf_bridge_llcache,
    .bitmap = &netsurf_bridge_bitmap,
};

struct netsurf_table *guit = &netsurf_bridge_table;

const char * const netsurf_version = "3.12-kernel";
const int netsurf_version_major = 3;
const int netsurf_version_minor = 12;
bool html_redraw_printing = false;
int html_redraw_printing_border = 0;
int html_redraw_printing_top_cropped = 0;

static nserror netsurf_bridge_options(struct nsoption_s *defaults) {
    (void)defaults;
    nsoption_set_int(font_size, 100);
    nsoption_set_int(font_min_size, 80);
    nsoption_set_int(memory_cache_size, (int)netsurf_bridge_memory_cache_limit());
    nsoption_set_uint(disc_cache_size, 0);
    nsoption_set_bool(block_advertisements, false);
    nsoption_set_bool(author_level_css, true);
    nsoption_set_bool(enable_javascript, true);
    nsoption_set_int(script_timeout, 3);
    nsoption_set_int(max_fetchers, 8);
    nsoption_set_int(max_fetchers_per_host, 4);
    nsoption_set_uint(max_retried_fetches, 0);
    return NSERROR_OK;
}

static nserror netsurf_bridge_init(void) {
    struct hlcache_parameters cache_params;
    bitmap_fmt_t bitmap_format = {
        .layout = BITMAP_LAYOUT_R8G8B8A8,
        .pma = false,
    };
    nserror err;

    if (netsurf_bridge_initialised) {
        return NSERROR_OK;
    }
    netsurf_bridge_table.layout = netsurf_kernel_layout_table();

    err = nsoption_init(netsurf_bridge_options, 0, 0);
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: nsoption init failed";
        return err;
    }
    err = corestrings_init();
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: corestrings init failed";
        return err;
    }
    err = nscolour_update();
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: colour init failed";
        return err;
    }
    bitmap_set_format(&bitmap_format);
    err = nscss_init();
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: CSS handler init failed";
        return err;
    }
    err = svg_init();
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: SVG handler init failed";
        return err;
    }
    err = netsurf_bridge_image_init();
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: image handler init failed";
        return err;
    }
    err = html_init();
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: HTML handler init failed";
        return err;
    }
    err = fetcher_init();
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: fetcher init failed";
        return err;
    }
    err = netsurf_bridge_http_register_scheme("http");
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: HTTP fetcher init failed";
        return err;
    }
    err = netsurf_bridge_http_register_scheme("https");
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: HTTPS fetcher init failed";
        return err;
    }

    memset(&cache_params, 0, sizeof(cache_params));
    cache_params.bg_clean_time = 1000;
    cache_params.llcache.limit = netsurf_bridge_memory_cache_limit();
    cache_params.llcache.hysteresis = cache_params.llcache.limit / 8u;
    if (cache_params.llcache.hysteresis < 512u * 1024u) {
        cache_params.llcache.hysteresis = 512u * 1024u;
    }
    err = hlcache_initialise(&cache_params);
    if (err != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: hlcache init failed";
        return err;
    }
    js_initialise();

    netsurf_bridge_initialised = true;
    netsurf_bridge_status = "NetSurf core bridge ready";
    return NSERROR_OK;
}

static void netsurf_bridge_content_user(struct content *c,
                                        content_msg msg,
                                        const union content_msg_data *data,
                                        void *pw) {
    netsurf_bridge_document_t *doc = pw;
    (void)c;
    if (doc == 0) {
        return;
    }
    if (msg == CONTENT_MSG_GETDIMS && data != 0) {
        if (data->getdims.viewport_width != 0) {
            *data->getdims.viewport_width = (unsigned)doc->view.width;
        }
        if (data->getdims.viewport_height != 0) {
            *data->getdims.viewport_height = (unsigned)doc->view.height;
        }
        return;
    }
    if (msg == CONTENT_MSG_GETTHREAD && data != 0 && data->jsthread != 0) {
        struct jsthread *thread = 0;
        if (doc->jsheap == 0 &&
            js_newheap(nsoption_int(script_timeout), &doc->jsheap) != NSERROR_OK) {
            netsurf_bridge_status = "NetSurf core bridge: JS heap create failed";
            return;
        }
        if (js_newthread(doc->jsheap, doc, doc->content, &thread) == NSERROR_OK) {
            *data->jsthread = thread;
        }
        return;
    }
    if (msg == CONTENT_MSG_READY || msg == CONTENT_MSG_DONE) {
        doc->needs_reformat = true;
        doc->needs_redraw = true;
        return;
    }
    if (msg == CONTENT_MSG_REDRAW ||
        msg == CONTENT_MSG_REFORMAT ||
        msg == CONTENT_MSG_CARET ||
        msg == CONTENT_MSG_STATUS ||
        msg == CONTENT_MSG_POINTER ||
        msg == CONTENT_MSG_ERROR) {
        doc->needs_redraw = true;
    }
}

static bool netsurf_bridge_reformat_if_needed(netsurf_bridge_document_t *doc,
                                              struct content *content,
                                              uint32_t width,
                                              uint32_t height,
                                              bool force) {
    if (doc == 0 || content == 0) {
        return false;
    }
    if (!force && !doc->needs_reformat) {
        return false;
    }
    if (content->status != CONTENT_STATUS_READY &&
        content->status != CONTENT_STATUS_DONE) {
        return false;
    }
    doc->view.width = (int)width;
    doc->view.height = (int)height;
    doc->needs_reformat = false;
    content__reformat(content, false, (int)width, (int)height);
    doc->needs_redraw = true;
    return true;
}

static bool netsurf_bridge_note_stalled_initial_load(netsurf_bridge_document_t *doc,
                                                     const struct content *content) {
    if (doc == 0 || content == 0) {
        return false;
    }
    if (content->status != CONTENT_STATUS_LOADING) {
        return false;
    }
    if (netsurf_bridge_schedule_count() != 0u ||
        netsurf_bridge_fetch_pending_count() != 0u ||
        netsurf_bridge_http_pending_count() != 0u) {
        doc->poll_stall_count = 0;
        return false;
    }
    if (++doc->poll_stall_count < NETSURF_BRIDGE_POLL_STALL_LIMIT) {
        return false;
    }

    doc->poll_stall_count = 0;
    netsurf_bridge_set_progress_status("waiting for NetSurf resource callbacks", content, 0u);
    return true;
}

static nserror netsurf_bridge_open_document(netsurf_bridge_document_t *doc) {
    if (doc == 0 || doc->content == 0) {
        return NSERROR_BAD_PARAMETER;
    }
    if (doc->opened) {
        return NSERROR_OK;
    }
    if (doc->content->handler != 0 && doc->content->handler->open != 0) {
        nserror err = doc->content->handler->open(doc->content,
                                                  (struct browser_window *)doc,
                                                  0,
                                                  0);
        if (err != NSERROR_OK) {
            return err;
        }
    }
    doc->opened = true;
    return NSERROR_OK;
}

static void netsurf_bridge_destroy_content(struct content *content) {
    if (content == 0) {
        return;
    }
    if (content->handler != 0 && content->handler->destroy != 0) {
        content->handler->destroy(content);
    }
    if (content->mime_type != 0) {
        lwc_string_unref(content->mime_type);
    }
    free(content->user_list);
    free(content->title);
    free(content->fallback_charset);
    free(content);
}

static int netsurf_core_render_html_mode(const uint8_t *url,
                                         const uint8_t *html,
                                         uint32_t len,
                                         uint32_t x,
                                         uint32_t y,
                                         uint32_t width,
                                         uint32_t height,
                                         uint32_t scroll,
                                         bool draw,
                                         netsurf_browser_render_result_t *out_result) {
    lwc_string *mime = 0;
    nsurl *ns_url = 0;
    netsurf_bridge_document_t *doc = 0;
    struct content *content = 0;
    struct content_redraw_data data;
    struct redraw_context ctx;
    struct rect clip;
    uint32_t signature;
    nserror err;
    bool cache_hit = false;
    bool reformat_needed;
    bool redraw_ok;

    if (html == 0 || len == 0u || width < 16u || height < 16u) {
        return -1;
    }
    err = netsurf_bridge_init();
    if (err != NSERROR_OK) {
        return -1;
    }

    signature = netsurf_bridge_signature(html, len);
    if (netsurf_bridge_cached_document != 0 &&
        netsurf_bridge_cached_document->source == html &&
        netsurf_bridge_cached_document->source_len == len &&
        netsurf_bridge_cached_document->source_signature == signature) {
        doc = netsurf_bridge_cached_document;
        content = doc->content;
        cache_hit = true;
    } else {
        netsurf_core_invalidate_cache();

        err = nsurl_create((const char *)(url != 0 ? url : (const uint8_t *)"about:blank"), &ns_url);
        if (err != NSERROR_OK) {
            err = nsurl_create("about:blank", &ns_url);
            if (err != NSERROR_OK) {
                netsurf_bridge_status = "NetSurf core bridge: URL creation failed";
                return -1;
            }
        }
        if (lwc_intern_string("text/html", 9, &mime) != lwc_error_ok) {
            nsurl_unref(ns_url);
            netsurf_bridge_status = "NetSurf core bridge: MIME intern failed";
            return -1;
        }

        doc = calloc(1, sizeof(*doc));
        if (doc == 0) {
            lwc_string_unref(mime);
            nsurl_unref(ns_url);
            netsurf_bridge_status = "NetSurf core bridge: document cache allocation failed";
            return -1;
        }

        doc->url = ns_url;
        doc->headers[0].name = "Content-Type";
        doc->headers[0].value = "text/html; charset=utf-8";
        doc->object.url = ns_url;
        doc->object.source_data = (uint8_t *)html;
        doc->object.source_len = len;
        doc->object.source_alloc = len;
        doc->object.store_state = LLCACHE_STATE_RAM;
        doc->object.headers = doc->headers;
        doc->object.num_headers = 1;
        doc->handle.object = &doc->object;

        content = content_factory_create_content(&doc->handle, "UTF-8", false, mime);
        lwc_string_unref(mime);
        if (content == 0) {
            nsurl_unref(ns_url);
            free(doc);
            netsurf_bridge_status = "NetSurf core bridge: HTML content create failed";
            return -1;
        }
        doc->content = content;
        doc->view.width = (int)width;
        doc->view.height = (int)height;
        if (!content_add_user(content, netsurf_bridge_content_user, doc)) {
            netsurf_bridge_destroy_content(content);
            nsurl_unref(ns_url);
            free(doc);
            netsurf_bridge_status = "NetSurf core bridge: content user failed";
            return -1;
        }

        if (content->handler->process_data == 0 ||
            content->handler->data_complete == 0 ||
            content->handler->redraw == 0 ||
            !content->handler->process_data(content, (const char *)html, len)) {
            netsurf_bridge_destroy_content(content);
            nsurl_unref(ns_url);
            if (doc->jsheap != 0) {
                js_destroyheap(doc->jsheap);
            }
            free(doc);
            netsurf_bridge_schedule_clear();
            netsurf_bridge_status = "NetSurf core bridge: HTML conversion failed";
            return -1;
        }
        netsurf_bridge_run_scheduled_until_ready(content);
        if (!content->handler->data_complete(content)) {
            netsurf_bridge_destroy_content(content);
            nsurl_unref(ns_url);
            if (doc->jsheap != 0) {
                js_destroyheap(doc->jsheap);
            }
            free(doc);
            netsurf_bridge_schedule_clear();
            netsurf_bridge_status = "NetSurf core bridge: HTML conversion failed";
            return -1;
        }
        netsurf_bridge_run_scheduled_until_ready(content);
        if (content->status != CONTENT_STATUS_READY && content->status != CONTENT_STATUS_DONE) {
            netsurf_bridge_set_waiting_status(content);
            doc->source = html;
            doc->source_len = len;
            doc->source_signature = signature;
            doc->prepare_pending = true;
            netsurf_bridge_cached_document = doc;
            if (out_result != 0) {
                out_result->flags |= NETSURF_BROWSER_RENDER_CORE_PENDING;
                out_result->flags |= NETSURF_BROWSER_RENDER_FRONTEND_OK;
                out_result->rendered_lines = 0;
                out_result->skipped_tags = 0;
            }
            return -1;
        }

        doc->source = html;
        doc->source_len = len;
        doc->source_signature = signature;
        doc->prepare_pending = false;
        netsurf_bridge_cached_document = doc;
    }

    if (content->status != CONTENT_STATUS_READY && content->status != CONTENT_STATUS_DONE) {
        netsurf_bridge_set_waiting_status(content);
        if (out_result != 0) {
            out_result->flags |= NETSURF_BROWSER_RENDER_CORE_PENDING;
            out_result->flags |= NETSURF_BROWSER_RENDER_FRONTEND_OK;
            out_result->rendered_lines = 0;
            out_result->skipped_tags = 0;
        }
        return -1;
    }
    doc->prepare_pending = false;
    if (netsurf_bridge_open_document(doc) != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf core bridge: content open failed";
        return -1;
    }

    reformat_needed = !cache_hit ||
                      doc->view.width != (int)width ||
                      doc->view.height != (int)height;
    doc->view.width = (int)width;
    doc->view.height = (int)height;
    if (reformat_needed) {
        (void)netsurf_bridge_reformat_if_needed(doc, content, width, height, true);
    }
    if (cache_hit && (content->active != 0u ||
                      netsurf_bridge_schedule_count() != 0u ||
                      netsurf_bridge_fetch_pending_count() != 0u ||
                      netsurf_bridge_http_pending_count() != 0u)) {
        (void)netsurf_bridge_run_scheduled_budget(NETSURF_BRIDGE_POLL_CALLBACK_BUDGET);
        (void)netsurf_bridge_pump_fetchers();
        (void)netsurf_bridge_reformat_if_needed(doc, content, width, height, false);
    }

    if (!draw) {
        if (out_result != 0) {
            out_result->flags &= ~NETSURF_BROWSER_RENDER_CORE_PENDING;
            out_result->flags |= NETSURF_BROWSER_RENDER_BOX_TREE | NETSURF_BROWSER_RENDER_FRONTEND_OK;
            out_result->rendered_lines = 0;
            out_result->skipped_tags = 0;
        }
        netsurf_bridge_set_progress_status(cache_hit ? "cached box tree ready" :
                                                       "prepared box tree",
                                           content,
                                           0u);
        return 0;
    }

    memset(&data, 0, sizeof(data));
    data.x = (int)x;
    data.y = (int)y - (int)scroll;
    data.width = content->width > 0 ? content->width : (int)width;
    data.height = content->height > 0 ? content->height : (int)height;
    data.background_colour = 0xffffffu;
    data.scale = 1.0f;

    clip.x0 = (int)x;
    clip.y0 = (int)y;
    clip.x1 = (int)(x + width);
    clip.y1 = (int)(y + height);
    netsurf_kernel_redraw_context(&ctx, 0);
    netsurf_kernel_plot_stats_reset();
    redraw_ok = content->handler->redraw(content, &data, &clip, &ctx);

    if (out_result != 0) {
        netsurf_kernel_plot_stats_t stats;
        netsurf_kernel_plot_stats_snapshot(&stats);
        out_result->flags &= ~NETSURF_BROWSER_RENDER_CORE_PENDING;
        out_result->flags |= NETSURF_BROWSER_RENDER_BOX_TREE | NETSURF_BROWSER_RENDER_FRONTEND_OK;
        out_result->rendered_lines = stats.visible_texts;
        out_result->skipped_tags = stats.visible_bitmaps;
    }

    if (redraw_ok) {
        netsurf_kernel_plot_stats_t stats;
        netsurf_kernel_plot_stats_snapshot(&stats);
        if (stats.visible_texts == 0u && stats.visible_bitmaps == 0u) {
            netsurf_bridge_box_stats_t box_stats;
            const html_content *html = (const html_content *)content;
            const struct box *layout = html != 0 ? html->layout : 0;

            netsurf_bridge_html_box_stats(content, &box_stats);
            snprintf(netsurf_bridge_status_buffer,
                     sizeof(netsurf_bridge_status_buffer),
                     "NetSurf blank: boxes %u textboxes %u textbytes %u objboxes %u depth %u cw %d ch %d ltype %d lchild %p plots rect %u text %u/%u bitmap %u/%u path %u",
                     box_stats.boxes,
                     box_stats.text_boxes,
                     box_stats.text_bytes,
                     box_stats.object_boxes,
                     box_stats.max_depth,
                     content->width,
                     content->height,
                     layout != 0 ? (int)layout->type : -1,
                     layout != 0 ? (void *)layout->children : 0,
                     stats.rectangles,
                     stats.visible_texts,
                     stats.texts,
                     stats.visible_bitmaps,
                     stats.bitmaps,
                     stats.paths);
            netsurf_bridge_status = netsurf_bridge_status_buffer;
            return -1;
        }
        netsurf_bridge_set_render_status(cache_hit ? "rendered cached box tree" :
                                                     "rendered box tree",
                                         content,
                                         &stats);
    } else {
        netsurf_bridge_status = "NetSurf core bridge: redraw reported failure";
    }
    return redraw_ok ? 0 : -1;
}

NETSURF_BROWSER_ENTRY int netsurf_core_prepare_html(const uint8_t *url,
                                                    const uint8_t *html,
                                                    uint32_t len,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    netsurf_browser_render_result_t *out_result) {
    return netsurf_core_render_html_mode(url,
                                         html,
                                         len,
                                         0,
                                         0,
                                         width,
                                         height,
                                         0,
                                         false,
                                         out_result);
}

NETSURF_BROWSER_ENTRY int netsurf_core_render_html(const uint8_t *url,
                                                   const uint8_t *html,
                                                   uint32_t len,
                                                   uint32_t x,
                                                   uint32_t y,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   uint32_t scroll,
                                                   netsurf_browser_render_result_t *out_result) {
    return netsurf_core_render_html_mode(url,
                                         html,
                                         len,
                                         x,
                                         y,
                                         width,
                                         height,
                                         scroll,
                                         true,
                                         out_result);
}

int netsurf_core_poll(void) {
    netsurf_bridge_document_t *doc;
    struct content *content;
    unsigned int pending_before;
    unsigned int ran;
    unsigned int pending_after;
    bool reformatted;
    bool wants_redraw;

    if (netsurf_bridge_cached_document == 0 ||
        netsurf_bridge_cached_document->content == 0) {
        return 0;
    }

    doc = netsurf_bridge_cached_document;
    content = doc->content;
    pending_before = netsurf_bridge_schedule_count() +
                     netsurf_bridge_http_pending_count() +
                     netsurf_bridge_fetch_pending_count() +
                     content->active +
                     (doc->needs_reformat ? 1u : 0u) +
                     (doc->needs_redraw ? 1u : 0u) +
                     (doc->prepare_pending ? 1u : 0u);
    if (pending_before == 0u) {
        netsurf_bridge_set_progress_status("idle", content, 0u);
        return 0;
    }

    ran = netsurf_bridge_run_scheduled_budget(NETSURF_BRIDGE_POLL_CALLBACK_BUDGET);
    ran += netsurf_bridge_pump_fetchers();
    (void)netsurf_bridge_note_stalled_initial_load(doc, content);
    if (doc->prepare_pending &&
        (content->status == CONTENT_STATUS_READY || content->status == CONTENT_STATUS_DONE)) {
        doc->prepare_pending = false;
        doc->needs_reformat = true;
        doc->needs_redraw = true;
    }
    reformatted = netsurf_bridge_reformat_if_needed(doc,
                                                    content,
                                                    (uint32_t)doc->view.width,
                                                    (uint32_t)doc->view.height,
                                                    false);
    wants_redraw = doc->needs_redraw || reformatted;
    doc->needs_redraw = false;
    pending_after = netsurf_bridge_schedule_count() +
                    netsurf_bridge_http_pending_count() +
                    netsurf_bridge_fetch_pending_count() +
                    content->active +
                    (doc->needs_reformat ? 1u : 0u) +
                    (doc->prepare_pending ? 1u : 0u);
    netsurf_bridge_set_progress_status(pending_after == 0u ? "objects ready" : "polling objects",
                                       content,
                                       ran);
    if (wants_redraw || reformatted) {
        return 2;
    }
    return ran != 0u ? 1 : 1;
}

int netsurf_core_mouse_event(uint32_t x, uint32_t y, uint32_t mouse_state) {
    netsurf_bridge_document_t *doc;
    struct content *content;
    html_content *html;
    struct box *box;
    int box_x;
    int box_y;
    nserror submit_error;

    if (netsurf_bridge_cached_document == 0 ||
        netsurf_bridge_cached_document->content == 0) {
        netsurf_bridge_status = "NetSurf mouse ignored: no document";
        return 0;
    }

    doc = netsurf_bridge_cached_document;
    content = doc->content;
    if (content->status != CONTENT_STATUS_READY && content->status != CONTENT_STATUS_DONE) {
        netsurf_bridge_status = "NetSurf mouse ignored: document loading";
        return 0;
    }
    if (netsurf_bridge_open_document(doc) != NSERROR_OK) {
        netsurf_bridge_status = "NetSurf mouse ignored: content open failed";
        return 0;
    }

    if (content->handler == 0 || content->handler->mouse_action == 0) {
        return 0;
    }
    content->handler->mouse_action(content,
                                   (struct browser_window *)doc,
                                   (browser_mouse_state)mouse_state,
                                   (int)x,
                                   (int)y);
    if (!netsurf_bridge_navigation_pending &&
        (mouse_state & BROWSER_MOUSE_CLICK_1) != 0u) {
        html = (html_content *)content;
        box = html->layout;
        box_x = 0;
        box_y = 0;
        while (box != 0) {
            box = box_at_point(&html->unit_len_ctx, box, (int)x, (int)y, &box_x, &box_y);
            if (box != 0 &&
                box->gadget != 0 &&
                box->gadget->form != 0 &&
                (box->gadget->type == GADGET_SUBMIT || box->gadget->type == GADGET_IMAGE)) {
                submit_error = form_submit(content_get_url(content),
                                           (struct browser_window *)doc,
                                           box->gadget->form,
                                           box->gadget);
                if (submit_error != NSERROR_OK) {
                    static char submit_status[96];
                    snprintf(submit_status,
                             sizeof(submit_status),
                             "NetSurf form submit failed %d",
                             (int)submit_error);
                    netsurf_bridge_status = submit_status;
                }
                break;
            }
        }
    }
    doc->needs_redraw = true;
    return netsurf_bridge_navigation_pending ? 2 : 1;
}

int netsurf_core_key_event(uint32_t key) {
    netsurf_bridge_document_t *doc;
    struct content *content;
    bool handled;

    if (netsurf_bridge_cached_document == 0 ||
        netsurf_bridge_cached_document->content == 0) {
        return 0;
    }

    doc = netsurf_bridge_cached_document;
    content = doc->content;
    if (content->status != CONTENT_STATUS_READY && content->status != CONTENT_STATUS_DONE) {
        return 0;
    }
    if (netsurf_bridge_open_document(doc) != NSERROR_OK) {
        return 0;
    }

    if (content->handler == 0 || content->handler->keypress == 0) {
        return 0;
    }
    handled = content->handler->keypress(content, key);
    if (handled) {
        doc->needs_redraw = true;
    }
    return netsurf_bridge_navigation_pending ? 2 : (handled ? 1 : 0);
}

int netsurf_core_consume_navigation(uint8_t *out, uint32_t capacity) {
    uint32_t i;

    if (!netsurf_bridge_navigation_pending ||
        out == 0 ||
        capacity == 0 ||
        netsurf_bridge_pending_url[0] == '\0') {
        return 0;
    }

    i = 0;
    while (netsurf_bridge_pending_url[i] != '\0' && i + 1u < capacity) {
        out[i] = (uint8_t)netsurf_bridge_pending_url[i];
        ++i;
    }
    out[i] = 0;
    netsurf_bridge_navigation_pending = false;
    netsurf_bridge_pending_url[0] = '\0';
    return i != 0u ? 1 : 0;
}

const char *netsurf_core_status(void) {
    return netsurf_bridge_status;
}

nserror fetch_file_register(void) {
    return NSERROR_OK;
}

nserror fetch_about_register(void) {
    return NSERROR_OK;
}

bool urldb_add_url(struct nsurl *url) {
    (void)url;
    return true;
}

void urldb_destroy(void) {
}

nserror urldb_set_url_persistence(struct nsurl *url, bool persist) {
    (void)url;
    (void)persist;
    return NSERROR_OK;
}

nserror urldb_set_url_title(struct nsurl *url, const char *title) {
    (void)url;
    (void)title;
    return NSERROR_OK;
}

nserror urldb_set_url_content_type(struct nsurl *url, content_type type) {
    (void)url;
    (void)type;
    return NSERROR_OK;
}

void urldb_set_auth_details(struct nsurl *url, const char *realm, const char *auth) {
    (void)url;
    (void)realm;
    (void)auth;
}

const char *urldb_get_auth_details(struct nsurl *url, const char *realm) {
    (void)url;
    (void)realm;
    return 0;
}

nserror urldb_update_url_visit_data(struct nsurl *url) {
    (void)url;
    return NSERROR_OK;
}

void urldb_reset_url_visit_data(struct nsurl *url) {
    (void)url;
}

struct nsurl *urldb_get_url(struct nsurl *url) {
    return url;
}

bool urldb_get_cert_permissions(struct nsurl *url) {
    (void)url;
    return false;
}

bool urldb_set_cookie(const char *header, struct nsurl *url, struct nsurl *referrer) {
    (void)header;
    (void)url;
    (void)referrer;
    return true;
}

char *urldb_get_cookie(struct nsurl *url, bool include_http_only) {
    (void)url;
    (void)include_http_only;
    return 0;
}

bool urldb_set_hsts_policy(struct nsurl *url, const char *header) {
    (void)url;
    (void)header;
    return false;
}

bool urldb_get_hsts_enabled(struct nsurl *url) {
    (void)url;
    return false;
}

const struct url_data *urldb_get_url_data(struct nsurl *url) {
    static struct url_data data;
    (void)url;
    memset(&data, 0, sizeof(data));
    return &data;
}

bool knockout_plot_start(const struct redraw_context *ctx, struct redraw_context *knk_ctx) {
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

struct selection *selection_create(struct content *c) {
    (void)c;
    return (struct selection *)calloc(1, 1);
}

void selection_destroy(struct selection *s) {
    free(s);
}

void selection_init(struct selection *s) {
    (void)s;
}

void selection_reinit(struct selection *s) {
    (void)s;
}

bool selection_clear(struct selection *s, bool redraw) {
    (void)s;
    (void)redraw;
    return false;
}

void selection_select_all(struct selection *s) {
    (void)s;
}

void selection_set_position(struct selection *s, unsigned start, unsigned end) {
    (void)s;
    (void)start;
    (void)end;
}

bool selection_click(struct selection *s,
                     struct browser_window *top,
                     browser_mouse_state mouse,
                     unsigned idx) {
    (void)s;
    (void)top;
    (void)mouse;
    (void)idx;
    return false;
}

void selection_track(struct selection *s, browser_mouse_state mouse, unsigned idx) {
    (void)s;
    (void)mouse;
    (void)idx;
}

bool selection_copy_to_clipboard(struct selection *s) {
    (void)s;
    return false;
}

char *selection_get_copy(struct selection *s) {
    (void)s;
    return 0;
}

bool selection_active(struct selection *s) {
    (void)s;
    return false;
}

bool selection_dragging(struct selection *s) {
    (void)s;
    return false;
}

bool selection_dragging_start(struct selection *s) {
    (void)s;
    return false;
}

void selection_drag_end(struct selection *s) {
    (void)s;
}

bool selection_highlighted(const struct selection *s,
                           unsigned start,
                           unsigned end,
                           unsigned *start_idx,
                           unsigned *end_idx) {
    (void)s;
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
        netsurf_bridge_status = "NetSurf form POST is not implemented yet";
        return NSERROR_NOT_IMPLEMENTED;
    }
    strncpy(netsurf_bridge_pending_url,
            nsurl_access(url),
            sizeof(netsurf_bridge_pending_url) - 1u);
    netsurf_bridge_pending_url[sizeof(netsurf_bridge_pending_url) - 1u] = '\0';
    netsurf_bridge_navigation_pending = netsurf_bridge_pending_url[0] != '\0';
    netsurf_bridge_status = "NetSurf navigation requested";
    return netsurf_bridge_navigation_pending ? NSERROR_OK : NSERROR_BAD_PARAMETER;
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

nserror cert_chain_alloc(size_t depth, struct cert_chain **chain_out) {
    (void)depth;
    if (chain_out != 0) {
        *chain_out = 0;
    }
    return NSERROR_OK;
}

nserror cert_chain_dup_into(const struct cert_chain *src, struct cert_chain *dst) {
    (void)src;
    (void)dst;
    return NSERROR_OK;
}

nserror cert_chain_dup(const struct cert_chain *src, struct cert_chain **dst_out) {
    (void)src;
    if (dst_out != 0) {
        *dst_out = 0;
    }
    return NSERROR_OK;
}

nserror cert_chain_from_query(struct nsurl *url, struct cert_chain **chain_out) {
    (void)url;
    if (chain_out != 0) {
        *chain_out = 0;
    }
    return NSERROR_OK;
}

nserror cert_chain_to_query(struct cert_chain *chain, struct nsurl **url_out) {
    (void)chain;
    if (url_out != 0) {
        *url_out = 0;
    }
    return NSERROR_NOT_IMPLEMENTED;
}

nserror cert_chain_free(struct cert_chain *chain) {
    (void)chain;
    return NSERROR_OK;
}

size_t cert_chain_size(const struct cert_chain *chain) {
    (void)chain;
    return 0;
}

nsuerror nsu_getmonotonic_ms(uint64_t *current_out) {
    if (current_out != 0) {
        *current_out = timer_ticks();
    }
    return NSUERROR_OK;
}

char *strndup(const char *s, size_t n) {
    size_t len = 0;
    char *out;
    while (len < n && s[len] != 0) {
        ++len;
    }
    out = malloc(len + 1u);
    if (out == 0) {
        return 0;
    }
    memcpy(out, s, len);
    out[len] = 0;
    return out;
}

char *strcasestr(const char *haystack, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return (char *)haystack;
    }
    while (*haystack != 0) {
        if (strncasecmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
        ++haystack;
    }
    return 0;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] != 0 && strchr(reject, s[n]) == 0) {
        ++n;
    }
    return n;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] != 0 && strchr(accept, s[n]) != 0) {
        ++n;
    }
    return n;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s != 0) {
        if (strchr(accept, *s) != 0) {
            return (char *)s;
        }
        ++s;
    }
    return 0;
}

char *strtok(char *str, const char *delim) {
    static char *next;
    char *start;
    if (str != 0) {
        next = str;
    }
    if (next == 0) {
        return 0;
    }
    next += strspn(next, delim);
    if (*next == 0) {
        next = 0;
        return 0;
    }
    start = next;
    next += strcspn(next, delim);
    if (*next != 0) {
        *next++ = 0;
    } else {
        next = 0;
    }
    return start;
}

int atoi(const char *nptr) {
    return (int)strtol(nptr, 0, 10);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    return (unsigned long long)strtoul(nptr, endptr, base);
}

float strtof(const char *nptr, char **endptr) {
    long whole = strtol(nptr, endptr, 10);
    return (float)whole;
}

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

float fabsf(float x) {
    return x < 0.0f ? -x : x;
}

int isnan(double x) {
    return x != x;
}

extern double pow(double x, double y);

double ceil(double x) {
    long xi = (long)x;
    if ((double)xi < x) {
        ++xi;
    }
    return (double)xi;
}

float ceilf(float x) {
    return (float)ceil((double)x);
}

float powf(float x, float y) {
    return (float)pow((double)x, (double)y);
}

long lroundf(float x) {
    return (long)(x >= 0.0f ? x + 0.5f : x - 0.5f);
}

int rand(void) {
    static uint32_t state = 1u;
    state = state * 1103515245u + 12345u;
    return (int)((state >> 16) & 0x7fffu);
}

char *getenv(const char *name) {
    (void)name;
    return 0;
}

int getpid(void) {
    return 1;
}

void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *)) {
    uint8_t *items = base;
    uint8_t tmp[128];
    size_t i;
    if (base == 0 || compar == 0 || size == 0 || size > sizeof(tmp)) {
        return;
    }
    for (i = 1; i < nmemb; ++i) {
        size_t j = i;
        memcpy(tmp, items + i * size, size);
        while (j > 0 && compar(items + (j - 1u) * size, tmp) > 0) {
            memmove(items + j * size, items + (j - 1u) * size, size);
            --j;
        }
        memcpy(items + j * size, tmp, size);
    }
}

FILE *stderr;

NETSURF_BROWSER_ENTRY int fprintf(FILE *stream, const char *format, ...) {
    (void)stream;
    (void)format;
    return 0;
}

FILE *fopen(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    return 0;
}

int fclose(FILE *stream) {
    (void)stream;
    return 0;
}

char *fgets(char *s, int size, FILE *stream) {
    (void)s;
    (void)size;
    (void)stream;
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    (void)ptr;
    (void)size;
    (void)nmemb;
    (void)stream;
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    (void)stream;
    (void)offset;
    (void)whence;
    return -1;
}

long ftell(FILE *stream) {
    (void)stream;
    return -1;
}

NETSURF_BROWSER_ENTRY int sscanf(const char *str, const char *format, ...) {
    (void)str;
    (void)format;
    return 0;
}

int inflateInit2(void *stream, int window_bits) {
    (void)stream;
    (void)window_bits;
    return -1;
}

int inflate(void *stream, int flush) {
    (void)stream;
    (void)flush;
    return -1;
}

int inflateEnd(void *stream) {
    (void)stream;
    return 0;
}

void *gzopen(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    return 0;
}

char *gzgets(void *file, char *buf, int len) {
    (void)file;
    (void)buf;
    (void)len;
    return 0;
}

int gzclose(void *file) {
    (void)file;
    return 0;
}

char *strerror(int errnum) {
    (void)errnum;
    return "error";
}

int feof(FILE *stream) {
    (void)stream;
    return 1;
}

int gettimeofday(void *tv, void *tz) {
    uint64_t now = timer_ticks();
    long *parts = (long *)tv;
    (void)tz;
    if (parts != 0) {
        parts[0] = (long)(now / 1000u);
        parts[1] = (long)((now % 1000u) * 1000u);
    }
    return 0;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    (void)stream;
    (void)format;
    (void)ap;
    return 0;
}

int fputc(int c, FILE *stream) {
    (void)stream;
    return c;
}

int fputs(const char *s, FILE *stream) {
    (void)stream;
    return s != 0 ? (int)strlen(s) : 0;
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;
}

int atexit(void (*function)(void)) {
    (void)function;
    return 0;
}

long long strtoll(const char *nptr, char **endptr, int base) {
    return (long long)strtol(nptr, endptr, base);
}

struct tm *gmtime(const time_t *timer) {
    static struct tm tm_value;
    (void)timer;
    memset(&tm_value, 0, sizeof(tm_value));
    tm_value.tm_mday = 1;
    return &tm_value;
}

int uname(void *buf) {
    char *out = buf;
    if (out != 0) {
        memset(out, 0, 390);
        strcpy(out, "LainOS");
        strcpy(out + 65, "lain");
        strcpy(out + 130, "0");
        strcpy(out + 195, "0");
        strcpy(out + 260, "x86_64");
    }
    return 0;
}

void *iconv_open(const char *tocode, const char *fromcode) {
    (void)tocode;
    (void)fromcode;
    return (void *)-1;
}

size_t iconv(void *cd, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft) {
    size_t copied = 0;
    (void)cd;
    while (inbuf != 0 && *inbuf != 0 && inbytesleft != 0 && *inbytesleft != 0 &&
           outbuf != 0 && *outbuf != 0 && outbytesleft != 0 && *outbytesleft != 0) {
        **outbuf = **inbuf;
        ++*outbuf;
        ++*inbuf;
        --*outbytesleft;
        --*inbytesleft;
        ++copied;
    }
    return copied;
}

int iconv_close(void *cd) {
    (void)cd;
    return 0;
}

int stat(const char *path, void *buf) {
    (void)path;
    (void)buf;
    return -1;
}

void nscss_dump_computed_style(FILE *stream, const void *style) {
    (void)stream;
    (void)style;
}
