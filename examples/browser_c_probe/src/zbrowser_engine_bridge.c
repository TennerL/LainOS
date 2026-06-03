#include "zbrowser_engine_abi.h"

#ifdef ZBROWSER_ENGINE_ENABLE_DOM
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dom/bindings/hubbub/parser.h>
#include <dom/core/document.h>
#include <dom/core/element.h>
#include <dom/core/node.h>
#include <dom/core/string.h>
#include <libcss/computed.h>
#include <libcss/properties.h>
#include <libcss/select.h>
#include <libcss/stylesheet.h>
#include <libcss/unit.h>
#include "utils/errors.h"
#include "content/content_factory.h"
#include "content/content.h"
#include "content/content_protected.h"
#include "content/fetchers.h"
#include "content/hlcache.h"
#include "content/llcache.h"
#include "content/handlers/css/css.h"
#include "netsurf/content.h"
#include "netsurf/layout.h"
#include "netsurf/plotters.h"
#include "netsurf/plot_style.h"
#include "utils/nsurl.h"
#include "utils/corestrings.h"
#include "utils/nscolour.h"
#include "utils/nsoption.h"
#include "utils/talloc.h"
#include "netsurf/bitmap.h"
#include "content/handlers/html/box.h"
#include "content/handlers/html/box_inspect.h"
#include "content/handlers/html/box_construct.h"
#include "content/handlers/html/form_internal.h"
#include "content/handlers/html/layout.h"
#include "content/handlers/html/html.h"
#include "content/handlers/html/private.h"
#include "content/handlers/javascript/js.h"
#include "netsurf_resource_css.h"

#undef dom_node_unref
static void zbrowser_engine_dom_node_unref(void *node) {
    (void)node;
}
#define dom_node_unref(node) zbrowser_engine_dom_node_unref((void *)(node))

#undef css_stylesheet_destroy
static css_error zbrowser_engine_css_stylesheet_destroy(css_stylesheet *sheet) {
    (void)sheet;
    return CSS_OK;
}
#define css_stylesheet_destroy(sheet) zbrowser_engine_css_stylesheet_destroy((sheet))
#endif

extern void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
extern void gfx_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
extern void gfx_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color);
extern int gfx_draw_rect_packed(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint32_t *pixels, uint32_t pixel_count);
extern void draw_text_at_pixel(uint32_t x, uint32_t y, const uint8_t *text, uint32_t fg, uint32_t bg);
extern void draw_text_scaled_at_pixel(uint32_t x, uint32_t y, const uint8_t *text, uint32_t fg, uint32_t bg, uint32_t scale);
extern void draw_text_sized_at_pixel(uint32_t x,
                                     uint32_t y,
                                     const uint8_t *text,
                                     uint32_t fg,
                                     uint32_t bg,
                                     uint32_t glyph_width,
                                     uint32_t glyph_height,
                                     uint32_t advance,
                                     int bold,
                                     int italic);
extern int zbrowser_lainos_plot_text_ttf(const plot_font_style_t *fstyle,
                                         int x,
                                         int y,
                                         const char *text,
                                         size_t length,
                                         uint32_t fg,
                                         int bold,
                                         int italic,
                                         const struct rect *clip);

static int zbrowser_engine_ready;
static const uint8_t zbrowser_engine_status_initial[] =
    "C engine ABI compiled; NetSurf core integration pending";
static const uint8_t zbrowser_engine_status_ready[] =
    "C engine ABI prepared raw document";
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
static const uint8_t zbrowser_engine_status_dom_ready[] =
    "NetSurf libdom/hubbub parsed document";
static const uint8_t zbrowser_engine_status_dom_css_ready[] =
    "NetSurf libdom/hubbub/libcss parsed document";
static const uint8_t zbrowser_engine_status_dom_css_selected[] =
    "NetSurf libdom/hubbub/libcss selected computed root style";
static const uint8_t zbrowser_engine_status_dom_no_root[] =
    "NetSurf libdom/hubbub parsed document without root element";
static const uint8_t zbrowser_engine_status_dom_create_failed[] =
    "NetSurf libdom/hubbub create failed; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_nomem[] =
    "NetSurf libdom/hubbub create nomem; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_badparm[] =
    "NetSurf libdom/hubbub create badparm; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_dom[] =
    "NetSurf libdom document create failed; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_hubbub[] =
    "NetSurf hubbub parser create failed; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_hubbub_nomem[] =
    "NetSurf hubbub parser create nomem; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_hubbub_badparm[] =
    "NetSurf hubbub parser create badparm; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_hubbub_invalid[] =
    "NetSurf hubbub parser create invalid; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_hubbub_filenotfound[] =
    "NetSurf hubbub parser create filenotfound; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_hubbub_needdata[] =
    "NetSurf hubbub parser create needdata; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_hubbub_badencoding[] =
    "NetSurf hubbub parser create badencoding; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_hubbub_unknown[] =
    "NetSurf hubbub parser create unknown; raw fallback";
static const uint8_t zbrowser_engine_status_dom_create_null[] =
    "NetSurf libdom/hubbub create null output; raw fallback";
static const uint8_t zbrowser_engine_status_dom_chunk_failed[] =
    "NetSurf libdom/hubbub chunk parse failed; raw fallback";
static const uint8_t zbrowser_engine_status_dom_complete_failed[] =
    "NetSurf libdom/hubbub complete failed; raw fallback";
static const uint8_t zbrowser_engine_status_css_parse_failed[] =
    "NetSurf libcss parse failed";
static const uint8_t zbrowser_engine_status_css_select_failed[] =
    "NetSurf libcss style selection failed";
static const uint8_t zbrowser_engine_status_html_layout_ready[] =
    "NetSurf HTML box tree/layout/redraw ready";
static const uint8_t zbrowser_engine_status_html_layout_failed[] =
    "NetSurf HTML layout path failed; DOM painter fallback";
static const uint8_t zbrowser_engine_status_html_layout_no_root[] =
    "NetSurf HTML layout failed: document has no root; DOM painter fallback";
static const uint8_t zbrowser_engine_status_html_layout_select_ctx_failed[] =
    "NetSurf HTML layout failed: CSS selection context; DOM painter fallback";
static const uint8_t zbrowser_engine_status_html_layout_root_style_failed[] =
    "NetSurf HTML layout failed: root style selection; DOM painter fallback";
static const uint8_t zbrowser_engine_status_html_layout_base_url_failed[] =
    "NetSurf HTML layout failed: base URL creation; DOM painter fallback";
static const uint8_t zbrowser_engine_status_html_layout_lwc_failed[] =
    "NetSurf HTML layout failed: lwc intern; DOM painter fallback";
static const uint8_t zbrowser_engine_status_html_layout_box_failed[] =
    "NetSurf HTML layout failed: DOM to box conversion; DOM painter fallback";
static const uint8_t zbrowser_engine_status_html_layout_document_failed[] =
    "NetSurf HTML layout failed: layout_document; DOM painter fallback";
static const uint8_t zbrowser_engine_status_content_ready[] =
    "NetSurf content pipeline ready";
static const uint8_t zbrowser_engine_status_content_init_failed[] =
    "NetSurf content pipeline failed: init; DOM painter fallback";
static const uint8_t zbrowser_engine_status_content_create_failed[] =
    "NetSurf content pipeline failed: content create; DOM painter fallback";
static const uint8_t zbrowser_engine_status_content_parse_failed[] =
    "NetSurf content pipeline failed: process data; DOM painter fallback";
static const uint8_t zbrowser_engine_status_content_convert_failed[] =
    "NetSurf content pipeline failed: conversion; DOM painter fallback";
static const uint8_t zbrowser_engine_status_content_not_ready[] =
    "NetSurf content pipeline pending resources; DOM painter fallback";
static const uint8_t zbrowser_engine_status_content_unavailable[] =
    "NetSurf content pipeline unavailable";
static uint8_t zbrowser_engine_status_detail[768];
static char zbrowser_engine_normalized_url[1024];
static char zbrowser_engine_content_error_detail[128];
static unsigned int zbrowser_engine_content_data_complete_called;
static unsigned int zbrowser_engine_content_data_complete_ok;
static unsigned int zbrowser_engine_content_active_before_complete;
static unsigned int zbrowser_engine_content_active_after_complete;
#endif

static const uint8_t *zbrowser_engine_status_text = zbrowser_engine_status_initial;

static uint32_t zbrowser_engine_scroll_lines_to_px(uint32_t scroll_lines) {
    if (scroll_lines > UINT32_MAX / 18u) {
        return UINT32_MAX;
    }
    return scroll_lines * 18u;
}

#ifdef ZBROWSER_ENGINE_ENABLE_DOM
static dom_document *zbrowser_engine_document;
static char zbrowser_engine_root_name[32];
static uint32_t zbrowser_engine_css_blocks;
static uint32_t zbrowser_engine_css_selected;
static css_color zbrowser_engine_root_color;
static css_color zbrowser_engine_root_background;
static uint8_t zbrowser_engine_root_display;
static uint32_t zbrowser_engine_css_compat_blocks;
static uint32_t zbrowser_engine_css_compat_vars;
static uint32_t zbrowser_engine_css_compat_groups;
unsigned int zbrowser_engine_redraw_box_visits;
unsigned int zbrowser_engine_redraw_text_box_visits;
unsigned int zbrowser_engine_redraw_text_box_paths;
unsigned int zbrowser_engine_redraw_clip_skips;
unsigned int zbrowser_engine_redraw_child_clip_skips;
int zbrowser_engine_redraw_first_child_skip_type;
int zbrowser_engine_redraw_first_child_skip_x;
int zbrowser_engine_redraw_first_child_skip_y;
int zbrowser_engine_redraw_first_child_skip_w;
int zbrowser_engine_redraw_first_child_skip_h;
int zbrowser_engine_redraw_first_child_skip_r0;
int zbrowser_engine_redraw_first_child_skip_r1;
int zbrowser_engine_redraw_first_child_skip_c0;
int zbrowser_engine_redraw_first_child_skip_c1;
#define ZBROWSER_ENGINE_MAX_STYLESHEETS 64u
static css_stylesheet *zbrowser_engine_sheets[ZBROWSER_ENGINE_MAX_STYLESHEETS];
static uint32_t zbrowser_engine_sheet_count;
static uint32_t zbrowser_engine_css_skipped_blocks;
static css_stylesheet *zbrowser_engine_ua_sheet;
static html_content zbrowser_engine_html;
static int zbrowser_engine_html_ready;
static css_select_ctx *zbrowser_engine_html_select_ctx;
static struct nsurl *zbrowser_engine_base_url;
static int zbrowser_engine_content_initialised;
static int zbrowser_engine_core_initialised;
static struct content *zbrowser_engine_content;
static struct nsurl *zbrowser_engine_content_url;
static uint32_t zbrowser_engine_content_viewport_width;
static uint32_t zbrowser_engine_content_viewport_height;
static int zbrowser_engine_content_opened;
static int zbrowser_engine_content_needs_reformat;
static int zbrowser_engine_content_needs_redraw;
static int zbrowser_engine_content_needs_chrome;
static int zbrowser_engine_content_prepare_pending;
static int zbrowser_engine_content_rendered_once;
static int zbrowser_engine_content_browser_window_cookie;
static uint32_t zbrowser_engine_form_key_count;
static uint32_t zbrowser_engine_form_mouse_count;
static jsheap *zbrowser_engine_content_jsheap;
static const uint8_t *zbrowser_engine_content_source_html;
static uint32_t zbrowser_engine_content_source_size;
static char zbrowser_engine_content_source_url[1024];
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
typedef struct zbrowser_engine_llcache_header {
    char *name;
    char *value;
} zbrowser_engine_llcache_header_t;

typedef struct zbrowser_engine_llcache_fetch {
    uint32_t flags;
    struct nsurl *referer;
    llcache_post_data *post;
    struct fetch *fetch;
    int state;
    uint32_t redirect_count;
    uint32_t retries_remaining;
    bool hsts_in_use;
    bool tried_with_auth;
    bool tried_with_tls_downgrade;
    bool tainted_tls;
} zbrowser_engine_llcache_fetch_t;

typedef struct zbrowser_engine_llcache_cache_control {
    time_t req_time;
    time_t res_time;
    time_t fin_time;
    time_t date;
    time_t expires;
    int age;
    int max_age;
    int no_cache;
    char *etag;
    time_t last_modified;
} zbrowser_engine_llcache_cache_control_t;

typedef struct llcache_object llcache_object;
struct llcache_object {
    llcache_object *prev;
    llcache_object *next;
    struct nsurl *url;
    uint8_t *source_data;
    size_t source_len;
    size_t source_alloc;
    struct cert_chain *chain;
    int store_state;
    void *users;
    zbrowser_engine_llcache_fetch_t fetch;
    zbrowser_engine_llcache_cache_control_t cache;
    llcache_object *candidate;
    uint32_t candidate_count;
    zbrowser_engine_llcache_header_t *headers;
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

static struct llcache_object zbrowser_engine_content_object;
static struct llcache_handle zbrowser_engine_content_llcache;
static zbrowser_engine_llcache_header_t zbrowser_engine_content_headers[1];
#else
struct llcache_handle {
    struct nsurl *url;
    struct content *content;
    const uint8_t *source_data;
    size_t source_len;
    const char *content_type;
    void *cb;
    void *pw;
};
static struct llcache_handle zbrowser_engine_content_llcache;
#endif
extern const struct gui_layout_table zbrowser_lainos_layout_table;
extern nserror zbrowser_lainos_image_init(void);
extern int zbrowser_lainos_bitmap_width(struct bitmap *bitmap);
extern int zbrowser_lainos_bitmap_height(struct bitmap *bitmap);
extern int zbrowser_lainos_bitmap_rowstride(struct bitmap *bitmap);
extern int zbrowser_lainos_bitmap_opaque(struct bitmap *bitmap);
extern uint8_t *zbrowser_lainos_bitmap_buffer(struct bitmap *bitmap);
extern void put_pixel(uint32_t x, uint32_t y, uint32_t color);
extern uint32_t gfx_get_pixel(uint32_t x, uint32_t y);
extern uint32_t gfx_width(void);
extern uint32_t gfx_height(void);
extern int zbrowser_lainos_consume_navigation(uint8_t *out, uint32_t capacity);

static nserror zbrowser_engine_options(struct nsoption_s *defaults) {
    (void)defaults;
    nsoption_set_int(font_size, 100);
    nsoption_set_int(font_min_size, 80);
    nsoption_set_int(memory_cache_size, 32 * 1024 * 1024);
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

static const uint8_t zbrowser_engine_fallback_ua_css[] =
    "html,body{display:block;margin:0;padding:0;color:#202122;background:#fff;font-family:sans-serif;font-size:16px;line-height:20px}"
    "head,script,style,title,meta,link{display:none}"
    "div,p,section,article,header,footer,nav,main,form,ul,ol,li,table,tr{display:block}"
    "p{margin-top:8px;margin-bottom:8px}"
    "h1,h2,h3,h4,h5,h6{display:block;font-weight:bold;margin-top:12px;margin-bottom:8px}"
    "h1{font-size:32px;line-height:38px}h2{font-size:24px;line-height:30px}h3{font-size:20px;line-height:26px}"
    "a{color:#0645ad}em,i{font-style:italic}strong,b{font-weight:bold}"
    "blockquote{display:block;margin-top:8px;margin-bottom:8px;padding-left:24px}"
    "input,textarea,select,img{display:inline-block}";

#define ZBROWSER_DOM_HUBBUB_NOMEM 1u
#define ZBROWSER_DOM_HUBBUB_BADPARM 2u
#define ZBROWSER_DOM_HUBBUB_DOM 3u
#define ZBROWSER_DOM_HUBBUB_HUBBUB_ERR (1u << 16)
#define ZBROWSER_HUBBUB_NOMEM 5u
#define ZBROWSER_HUBBUB_BADPARM 6u
#define ZBROWSER_HUBBUB_INVALID 7u
#define ZBROWSER_HUBBUB_FILENOTFOUND 8u
#define ZBROWSER_HUBBUB_NEEDDATA 9u
#define ZBROWSER_HUBBUB_BADENCODING 10u
#define ZBROWSER_HUBBUB_UNKNOWN 11u

typedef struct {
    uint32_t fg;
    uint32_t bg;
    uint32_t indent_px;
    uint32_t padding_right_px;
    uint32_t margin_top_lines;
    uint32_t margin_bottom_lines;
    uint32_t scale;
    uint32_t font_px;
    uint32_t line_height_px;
    uint8_t bold;
    uint8_t italic;
    uint8_t display;
    uint8_t text_align;
    uint8_t display_none;
} zbrowser_engine_style_t;

typedef struct {
    css_select_ctx *select_ctx;
    css_unit_ctx unit_ctx;
    css_media media;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t scroll_line;
    uint32_t line_index;
    uint32_t line_len;
    char line[160];
} zbrowser_engine_paint_t;

static void zbrowser_engine_destroy_css_sheets(css_stylesheet **sheets, uint32_t count);
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
static void zbrowser_engine_release_content(void);
#endif

static const uint8_t *zbrowser_engine_dom_create_status(dom_hubbub_error error,
        dom_hubbub_parser *parser, dom_document *document) {
    if ((uint32_t)error == ZBROWSER_DOM_HUBBUB_NOMEM) {
        return zbrowser_engine_status_dom_create_nomem;
    }
    if ((uint32_t)error == ZBROWSER_DOM_HUBBUB_BADPARM) {
        return zbrowser_engine_status_dom_create_badparm;
    }
    if ((uint32_t)error == ZBROWSER_DOM_HUBBUB_DOM) {
        return zbrowser_engine_status_dom_create_dom;
    }
    if (((uint32_t)error & ZBROWSER_DOM_HUBBUB_HUBBUB_ERR) != 0u) {
        uint32_t hubbub_error = (uint32_t)error & ~ZBROWSER_DOM_HUBBUB_HUBBUB_ERR;
        if (hubbub_error == ZBROWSER_HUBBUB_NOMEM) {
            return zbrowser_engine_status_dom_create_hubbub_nomem;
        }
        if (hubbub_error == ZBROWSER_HUBBUB_BADPARM) {
            return zbrowser_engine_status_dom_create_hubbub_badparm;
        }
        if (hubbub_error == ZBROWSER_HUBBUB_INVALID) {
            return zbrowser_engine_status_dom_create_hubbub_invalid;
        }
        if (hubbub_error == ZBROWSER_HUBBUB_FILENOTFOUND) {
            return zbrowser_engine_status_dom_create_hubbub_filenotfound;
        }
        if (hubbub_error == ZBROWSER_HUBBUB_NEEDDATA) {
            return zbrowser_engine_status_dom_create_hubbub_needdata;
        }
        if (hubbub_error == ZBROWSER_HUBBUB_BADENCODING) {
            return zbrowser_engine_status_dom_create_hubbub_badencoding;
        }
        if (hubbub_error == ZBROWSER_HUBBUB_UNKNOWN) {
            return zbrowser_engine_status_dom_create_hubbub_unknown;
        }
        return zbrowser_engine_status_dom_create_hubbub;
    }
    if (parser == 0 || document == 0) {
        return zbrowser_engine_status_dom_create_null;
    }
    return zbrowser_engine_status_dom_create_failed;
}

static uint32_t zbrowser_engine_skip_tag(const uint8_t *html, uint32_t size, uint32_t pos) {
    uint8_t quote = 0;

    while (pos < size) {
        uint8_t ch = html[pos];
        if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '>') {
            return pos + 1u;
        }
        ++pos;
    }
    return size;
}

static uint8_t zbrowser_engine_lower(uint8_t ch) {
    return ch >= 'A' && ch <= 'Z' ? (uint8_t)(ch + 32u) : ch;
}

static int zbrowser_engine_is_space(uint8_t ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f';
}

static int zbrowser_engine_ci_starts(const uint8_t *text, uint32_t size, uint32_t pos, const char *needle) {
    uint32_t i = 0;

    while (needle[i] != '\0') {
        if (pos + i >= size || zbrowser_engine_lower(text[pos + i]) != (uint8_t)needle[i]) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static int zbrowser_engine_tag_name_is(const uint8_t *html, uint32_t size,
        uint32_t tag_start, const char *name) {
    uint32_t pos = tag_start;
    uint32_t i = 0;

    if (pos < size && html[pos] == '/') {
        ++pos;
    }
    while (pos < size && zbrowser_engine_is_space(html[pos])) {
        ++pos;
    }
    while (name[i] != '\0') {
        if (pos + i >= size ||
            zbrowser_engine_lower(html[pos + i]) != (uint8_t)name[i]) {
            return 0;
        }
        ++i;
    }
    return pos + i >= size ||
        zbrowser_engine_is_space(html[pos + i]) ||
        html[pos + i] == '>' ||
        html[pos + i] == '/';
}

static void zbrowser_engine_release_document(void) {
    struct form *form;
    struct form *next_form;

#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
    zbrowser_engine_release_content();
#endif
    if (zbrowser_engine_html_ready != 0 && zbrowser_engine_html.box_conversion_context != 0) {
        (void)cancel_dom_to_box(zbrowser_engine_html.box_conversion_context);
        zbrowser_engine_html.box_conversion_context = 0;
    }
    if (zbrowser_engine_html_ready != 0 && zbrowser_engine_html.bctx != 0) {
        talloc_free(zbrowser_engine_html.bctx);
        zbrowser_engine_html.bctx = 0;
        zbrowser_engine_html.layout = 0;
    }
    if (zbrowser_engine_html_select_ctx != 0) {
        css_select_ctx_destroy(zbrowser_engine_html_select_ctx);
        zbrowser_engine_html_select_ctx = 0;
    }
    if (zbrowser_engine_html.universal != 0) {
        lwc_string_unref(zbrowser_engine_html.universal);
        zbrowser_engine_html.universal = 0;
    }
    if (zbrowser_engine_html.media.prefers_color_scheme != 0) {
        lwc_string_unref(zbrowser_engine_html.media.prefers_color_scheme);
        zbrowser_engine_html.media.prefers_color_scheme = 0;
    }
    if (zbrowser_engine_base_url != 0) {
        nsurl_unref(zbrowser_engine_base_url);
        zbrowser_engine_base_url = 0;
    }
    for (form = zbrowser_engine_html.forms; form != 0; form = next_form) {
        next_form = form->prev;
        form_free(form);
    }
    zbrowser_engine_html.forms = 0;
    memset(&zbrowser_engine_html, 0, sizeof(zbrowser_engine_html));
    zbrowser_engine_html_ready = 0;
    if (zbrowser_engine_document != 0) {
        dom_node_unref(zbrowser_engine_document);
        zbrowser_engine_document = 0;
    }
    zbrowser_engine_destroy_css_sheets(zbrowser_engine_sheets, zbrowser_engine_sheet_count);
    zbrowser_engine_sheet_count = 0;
    if (zbrowser_engine_ua_sheet != 0) {
        css_stylesheet_destroy(zbrowser_engine_ua_sheet);
        zbrowser_engine_ua_sheet = 0;
    }
    zbrowser_engine_root_name[0] = 0;
}

static void zbrowser_engine_copy_root_name(dom_document *document) {
    struct dom_element *root = 0;
    dom_string *name = 0;
    const char *name_data;
    size_t name_len;
    size_t copy_len;

    zbrowser_engine_root_name[0] = 0;
    if (document == 0 ||
        dom_document_get_document_element(document, &root) != DOM_NO_ERR ||
        root == 0) {
        return;
    }

    if (dom_node_get_node_name(root, &name) == DOM_NO_ERR && name != 0) {
        name_data = dom_string_data(name);
        name_len = dom_string_byte_length(name);
        copy_len = name_len;
        if (copy_len >= sizeof(zbrowser_engine_root_name)) {
            copy_len = sizeof(zbrowser_engine_root_name) - 1u;
        }
        for (size_t i = 0; i < copy_len; ++i) {
            zbrowser_engine_root_name[i] = name_data[i];
        }
        zbrowser_engine_root_name[copy_len] = 0;
        dom_string_unref(name);
    }

    dom_node_unref(root);
}

static bool zbrowser_engine_ascii_is_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

static bool zbrowser_engine_ascii_match_ci(const char *text, const char *word) {
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

static bool zbrowser_engine_ascii_equal_ci_n(const char *text, size_t len, const char *word) {
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

static bool zbrowser_engine_ascii_ends_ci_n(const char *text, size_t len, const char *suffix) {
    size_t suffix_len;

    if (text == 0 || suffix == 0) {
        return false;
    }
    suffix_len = strlen(suffix);
    if (suffix_len > len) {
        return false;
    }
    return zbrowser_engine_ascii_equal_ci_n(text + len - suffix_len, suffix_len, suffix);
}

static unsigned int zbrowser_engine_css_group_rule_kind(const char *css, size_t len, size_t at) {
    size_t i = at + 1u;

    while (i < len && zbrowser_engine_ascii_is_space(css[i])) {
        ++i;
    }
    if (zbrowser_engine_ascii_match_ci(css + i, "layer")) {
        return 1u;
    }
    if (zbrowser_engine_ascii_match_ci(css + i, "supports") ||
        zbrowser_engine_ascii_match_ci(css + i, "container")) {
        return 2u;
    }
    return 0u;
}

static size_t zbrowser_engine_css_find_rule_body(const char *css, size_t len, size_t at) {
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

static size_t zbrowser_engine_css_find_matching_paren(const char *css, size_t len, size_t open) {
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

static size_t zbrowser_engine_css_find_matching_brace(const char *css, size_t len, size_t open) {
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

static size_t zbrowser_engine_css_find_var_fallback(const char *css, size_t start, size_t end) {
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

static bool zbrowser_engine_css_property_allows_var_fallback(const char *css, size_t len, size_t var_at) {
    size_t start = var_at;
    size_t colon;
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
    while (start < var_at && zbrowser_engine_ascii_is_space(css[start])) {
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
    while (name_end > name_start && zbrowser_engine_ascii_is_space(css[name_end - 1u])) {
        --name_end;
    }
    if (name_end <= name_start) {
        return false;
    }
    return zbrowser_engine_ascii_equal_ci_n(css + name_start, name_end - name_start, "color") ||
           zbrowser_engine_ascii_equal_ci_n(css + name_start, name_end - name_start, "background") ||
           zbrowser_engine_ascii_equal_ci_n(css + name_start, name_end - name_start, "fill") ||
           zbrowser_engine_ascii_equal_ci_n(css + name_start, name_end - name_start, "stroke") ||
           zbrowser_engine_ascii_equal_ci_n(css + name_start, name_end - name_start, "box-shadow") ||
           zbrowser_engine_ascii_equal_ci_n(css + name_start, name_end - name_start, "text-shadow") ||
           zbrowser_engine_ascii_equal_ci_n(css + name_start, name_end - name_start, "accent-color") ||
           zbrowser_engine_ascii_ends_ci_n(css + name_start, name_end - name_start, "-color");
}

static char *zbrowser_engine_css_compat_filter(const char *css,
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
            unsigned int group_kind = zbrowser_engine_css_group_rule_kind(css, len, i);
            size_t body = zbrowser_engine_css_find_rule_body(css, len, i);
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
                size_t close = zbrowser_engine_css_find_matching_brace(css, len, body);
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
            zbrowser_engine_ascii_match_ci(css + i, "var(")) {
            size_t close = zbrowser_engine_css_find_matching_paren(css, len, i + 3u);
            if (close < len) {
                size_t comma = zbrowser_engine_css_find_var_fallback(css, i + 4u, close);
                if (comma < close && zbrowser_engine_css_property_allows_var_fallback(css, len, i)) {
                    size_t start = comma + 1u;
                    size_t end = close;
                    while (start < end && zbrowser_engine_ascii_is_space(css[start])) {
                        ++start;
                    }
                    while (end > start && zbrowser_engine_ascii_is_space(css[end - 1u])) {
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

static int zbrowser_engine_create_css_sheet(const uint8_t *css, uint32_t size,
        const char *url, css_stylesheet **out_sheet) {
    css_stylesheet_params params;
    css_stylesheet *sheet = 0;
    css_error error;

    *out_sheet = 0;
    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_DEFAULT;
    params.charset = "UTF-8";
    params.url = url;
    params.title = 0;
    params.allow_quirks = true;
    params.inline_style = false;
    params.resolve = 0;
    params.resolve_pw = 0;
    params.import = 0;
    params.import_pw = 0;
    params.color = 0;
    params.color_pw = 0;
    params.font = 0;
    params.font_pw = 0;

    error = css_stylesheet_create(&params, &sheet);
    if (error != CSS_OK || sheet == 0) {
        return -1;
    }
    error = css_stylesheet_append_data(sheet, css, size);
    if (error == CSS_OK || error == CSS_NEEDDATA) {
        error = css_stylesheet_data_done(sheet);
    }
    if (error != CSS_OK) {
        css_stylesheet_destroy(sheet);
        return -1;
    }

    *out_sheet = sheet;
    return 0;
}

static int zbrowser_engine_collect_css_blocks(const uint8_t *html, uint32_t size,
        css_stylesheet **sheets, uint32_t max_sheets, uint32_t *out_count) {
    uint32_t pos = 0;
    uint32_t count = 0;

    zbrowser_engine_css_blocks = 0;
    zbrowser_engine_css_skipped_blocks = 0;
    zbrowser_engine_css_compat_blocks = 0;
    zbrowser_engine_css_compat_vars = 0;
    zbrowser_engine_css_compat_groups = 0;
    *out_count = 0;
    while (pos < size) {
        uint32_t content_start;
        uint32_t content_end;

        while (pos < size && html[pos] != '<') {
            ++pos;
        }
        if (pos >= size) {
            break;
        }
        if (!zbrowser_engine_ci_starts(html, size, pos + 1u, "style")) {
            ++pos;
            continue;
        }
        content_start = zbrowser_engine_skip_tag(html, size, pos + 1u);
        content_end = content_start;
        while (content_end < size && !zbrowser_engine_ci_starts(html, size, content_end, "</style")) {
            ++content_end;
        }
        if (content_end > content_start) {
            if (count >= max_sheets) {
                ++zbrowser_engine_css_skipped_blocks;
            } else {
                uint32_t css_len = content_end - content_start;
                uint32_t filtered_len = css_len;
                uint32_t vars = 0;
                uint32_t groups = 0;
                char *filtered = zbrowser_engine_css_compat_filter((const char *)(html + content_start),
                                                                   css_len,
                                                                   &filtered_len,
                                                                   &vars,
                                                                   &groups);
                const uint8_t *css_data = filtered != 0 ? (const uint8_t *)filtered : html + content_start;
                if (filtered != 0) {
                    ++zbrowser_engine_css_compat_blocks;
                    zbrowser_engine_css_compat_vars += vars;
                    zbrowser_engine_css_compat_groups += groups;
                }
                if (zbrowser_engine_create_css_sheet(css_data,
                                                     filtered_len,
                                                     "about:staged-inline",
                                                     &sheets[count]) != 0) {
                    ++zbrowser_engine_css_skipped_blocks;
                } else {
                    ++zbrowser_engine_css_blocks;
                    ++count;
                }
                if (filtered != 0) {
                    free(filtered);
                }
            }
        }
        pos = zbrowser_engine_skip_tag(html, size, content_end + 1u);
    }
    *out_count = count;
    return 0;
}

static void zbrowser_engine_prepare_ua_sheet(void) {
    if (zbrowser_engine_ua_sheet != 0) {
        return;
    }
    if (zbrowser_engine_create_css_sheet(netsurf_resource_default_css,
                                         netsurf_resource_default_css_len,
                                         "resource:netsurf-default.css",
                                         &zbrowser_engine_ua_sheet) == 0) {
        return;
    }
    (void)zbrowser_engine_create_css_sheet(zbrowser_engine_fallback_ua_css,
                                           sizeof(zbrowser_engine_fallback_ua_css) - 1u,
                                           "resource:lainos-fallback-ua.css",
                                           &zbrowser_engine_ua_sheet);
}

static css_error zbrowser_engine_node_name(void *pw, void *node, css_qname *qname) {
    dom_string *name = 0;

    (void)pw;
    qname->ns = 0;
    qname->name = 0;
    if (dom_node_get_node_name((dom_node *)node, &name) != DOM_NO_ERR || name == 0) {
        return CSS_NOMEM;
    }
    if (dom_string_intern(name, &qname->name) != DOM_NO_ERR) {
        dom_string_unref(name);
        return CSS_NOMEM;
    }
    dom_string_unref(name);
    return CSS_OK;
}

static css_error zbrowser_engine_node_classes(void *pw, void *node,
        lwc_string ***classes, uint32_t *n_classes) {
    (void)pw;
    *classes = 0;
    *n_classes = 0;
    return dom_element_get_classes((dom_element *)node, classes, n_classes) == DOM_NO_ERR
        ? CSS_OK
        : CSS_NOMEM;
}

static css_error zbrowser_engine_node_id(void *pw, void *node, lwc_string **id) {
    dom_string *name = 0;
    dom_string *value = 0;

    (void)pw;
    *id = 0;
    if (dom_string_create_interned((const uint8_t *)"id", 2, &name) != DOM_NO_ERR) {
        return CSS_NOMEM;
    }
    if (dom_element_get_attribute((dom_element *)node, name, &value) != DOM_NO_ERR) {
        dom_string_unref(name);
        return CSS_OK;
    }
    dom_string_unref(name);
    if (value != 0) {
        if (dom_string_intern(value, id) != DOM_NO_ERR) {
            dom_string_unref(value);
            return CSS_NOMEM;
        }
        dom_string_unref(value);
    }
    return CSS_OK;
}

static int zbrowser_engine_qname_is_universal(const css_qname *qname) {
    return qname->name != 0 &&
        lwc_string_length(qname->name) == 1u &&
        lwc_string_data(qname->name)[0] == '*';
}

static dom_string *zbrowser_engine_qname_to_dom_string(const css_qname *qname) {
    dom_string *name = 0;

    if (qname == 0 || qname->name == 0) {
        return 0;
    }
    if (dom_string_create_interned((const uint8_t *)lwc_string_data(qname->name),
            lwc_string_length(qname->name), &name) != DOM_NO_ERR) {
        return 0;
    }
    return name;
}

static css_error zbrowser_engine_named_none(void *pw, void *node,
        const css_qname *qname, void **result) {
    (void)pw;
    (void)node;
    (void)qname;
    *result = 0;
    return CSS_OK;
}

static css_error zbrowser_engine_relation_none(void *pw, void *node, void **result) {
    (void)pw;
    (void)node;
    *result = 0;
    return CSS_OK;
}

static css_error zbrowser_engine_node_has_name(void *pw, void *node,
        const css_qname *qname, bool *match) {
    dom_string *name = 0;

    (void)pw;
    *match = false;
    if (zbrowser_engine_qname_is_universal(qname)) {
        *match = true;
        return CSS_OK;
    }
    if (dom_node_get_node_name((dom_node *)node, &name) != DOM_NO_ERR || name == 0) {
        return CSS_OK;
    }
    *match = dom_string_caseless_lwc_isequal(name, qname->name);
    dom_string_unref(name);
    return CSS_OK;
}

static css_error zbrowser_engine_node_has_class(void *pw, void *node,
        lwc_string *name, bool *match) {
    (void)pw;
    *match = false;
    return dom_element_has_class((dom_element *)node, name, match) == DOM_NO_ERR
        ? CSS_OK
        : CSS_OK;
}

static css_error zbrowser_engine_node_has_id(void *pw, void *node,
        lwc_string *name, bool *match) {
    lwc_string *id = 0;

    *match = false;
    if (zbrowser_engine_node_id(pw, node, &id) == CSS_OK && id != 0) {
        (void)lwc_string_isequal(id, name, match);
        lwc_string_unref(id);
    }
    return CSS_OK;
}

static css_error zbrowser_engine_node_has_attribute(void *pw, void *node,
        const css_qname *qname, bool *match) {
    dom_string *name;

    (void)pw;
    *match = false;
    name = zbrowser_engine_qname_to_dom_string(qname);
    if (name == 0) {
        return CSS_NOMEM;
    }
    (void)dom_element_has_attribute((dom_element *)node, name, match);
    dom_string_unref(name);
    return CSS_OK;
}

static css_error zbrowser_engine_node_has_attribute_equal(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match) {
    dom_string *name;
    dom_string *attr = 0;

    (void)pw;
    *match = false;
    name = zbrowser_engine_qname_to_dom_string(qname);
    if (name == 0) {
        return CSS_NOMEM;
    }
    if (dom_element_get_attribute((dom_element *)node, name, &attr) == DOM_NO_ERR && attr != 0) {
        *match = dom_string_caseless_lwc_isequal(attr, value);
        dom_string_unref(attr);
    }
    dom_string_unref(name);
    return CSS_OK;
}

static css_error zbrowser_engine_attribute_match_false(void *pw, void *node,
        const css_qname *qname, lwc_string *value, bool *match) {
    (void)pw;
    (void)node;
    (void)qname;
    (void)value;
    *match = false;
    return CSS_OK;
}

static css_error zbrowser_engine_node_is_root(void *pw, void *node, bool *match) {
    dom_node *parent = 0;
    dom_node_type type = DOM_DOCUMENT_NODE;

    (void)pw;
    *match = false;
    if (dom_node_get_parent_node((dom_node *)node, &parent) != DOM_NO_ERR) {
        return CSS_OK;
    }
    if (parent == 0) {
        *match = true;
        return CSS_OK;
    }
    if (dom_node_get_node_type(parent, &type) == DOM_NO_ERR) {
        *match = type == DOM_DOCUMENT_NODE;
    }
    dom_node_unref(parent);
    return CSS_OK;
}

static css_error zbrowser_engine_node_count_siblings(void *pw, void *node,
        bool same_name, bool after, int32_t *count) {
    (void)pw;
    (void)node;
    (void)same_name;
    (void)after;
    *count = 0;
    return CSS_OK;
}

static css_error zbrowser_engine_node_is_empty(void *pw, void *node, bool *match) {
    (void)pw;
    (void)node;
    *match = false;
    return CSS_OK;
}

static css_error zbrowser_engine_node_bool_false(void *pw, void *node, bool *match) {
    (void)pw;
    (void)node;
    *match = false;
    return CSS_OK;
}

static css_error zbrowser_engine_node_is_link(void *pw, void *node, bool *match) {
    dom_string *name = 0;
    const char *data;

    (void)pw;
    *match = false;
    if (dom_node_get_node_name((dom_node *)node, &name) == DOM_NO_ERR && name != 0) {
        data = dom_string_data(name);
        if (dom_string_byte_length(name) == 1u &&
            (data[0] == 'a' || data[0] == 'A')) {
            *match = true;
        }
        dom_string_unref(name);
    }
    return CSS_OK;
}

static css_error zbrowser_engine_node_is_lang(void *pw, void *node,
        lwc_string *lang, bool *match) {
    (void)pw;
    (void)node;
    (void)lang;
    *match = false;
    return CSS_OK;
}

static css_error zbrowser_engine_node_presentational_hint(void *pw, void *node,
        uint32_t *nhints, css_hint **hints) {
    (void)pw;
    (void)node;
    *nhints = 0;
    *hints = 0;
    return CSS_OK;
}

static css_error zbrowser_engine_ua_default_for_property(void *pw,
        uint32_t property, css_hint *hint) {
    (void)pw;
    if (property == CSS_PROP_COLOR) {
        hint->data.color = 0xff000000u;
        hint->status = CSS_COLOR_COLOR;
    } else if (property == CSS_PROP_FONT_FAMILY) {
        hint->data.strings = 0;
        hint->status = CSS_FONT_FAMILY_SANS_SERIF;
    } else if (property == CSS_PROP_QUOTES) {
        hint->data.strings = 0;
        hint->status = CSS_QUOTES_NONE;
    } else {
        return CSS_INVALID;
    }
    return CSS_OK;
}

static css_error zbrowser_engine_set_libcss_node_data(void *pw, void *node,
        void *libcss_node_data) {
    (void)pw;
    (void)node;
    (void)libcss_node_data;
    return CSS_OK;
}

static css_error zbrowser_engine_get_libcss_node_data(void *pw, void *node,
        void **libcss_node_data) {
    (void)pw;
    (void)node;
    *libcss_node_data = 0;
    return CSS_OK;
}

static css_select_handler zbrowser_engine_select_handler = {
    CSS_SELECT_HANDLER_VERSION_1,
    zbrowser_engine_node_name,
    zbrowser_engine_node_classes,
    zbrowser_engine_node_id,
    zbrowser_engine_named_none,
    zbrowser_engine_named_none,
    zbrowser_engine_named_none,
    zbrowser_engine_named_none,
    zbrowser_engine_relation_none,
    zbrowser_engine_relation_none,
    zbrowser_engine_node_has_name,
    zbrowser_engine_node_has_class,
    zbrowser_engine_node_has_id,
    zbrowser_engine_node_has_attribute,
    zbrowser_engine_node_has_attribute_equal,
    zbrowser_engine_attribute_match_false,
    zbrowser_engine_attribute_match_false,
    zbrowser_engine_attribute_match_false,
    zbrowser_engine_attribute_match_false,
    zbrowser_engine_attribute_match_false,
    zbrowser_engine_node_is_root,
    zbrowser_engine_node_count_siblings,
    zbrowser_engine_node_is_empty,
    zbrowser_engine_node_is_link,
    zbrowser_engine_node_bool_false,
    zbrowser_engine_node_bool_false,
    zbrowser_engine_node_bool_false,
    zbrowser_engine_node_bool_false,
    zbrowser_engine_node_bool_false,
    zbrowser_engine_node_bool_false,
    zbrowser_engine_node_bool_false,
    zbrowser_engine_node_bool_false,
    zbrowser_engine_node_is_lang,
    zbrowser_engine_node_presentational_hint,
    zbrowser_engine_ua_default_for_property,
    zbrowser_engine_set_libcss_node_data,
    zbrowser_engine_get_libcss_node_data
};

static void zbrowser_engine_destroy_css_sheets(css_stylesheet **sheets, uint32_t count) {
    (void)sheets;
    (void)count;
}

static int zbrowser_engine_select_root_style_with_ctx(dom_document *document,
        css_select_ctx *select_ctx, uint32_t viewport_width, uint32_t viewport_height) {
    dom_element *root = 0;
    css_select_results *results = 0;
    css_computed_style *style;
    css_unit_ctx unit_ctx;
    css_media media;
    css_error error;
    int result = -1;

    zbrowser_engine_css_selected = 0;
    if (document == 0 || select_ctx == 0 ||
        dom_document_get_document_element(document, &root) != DOM_NO_ERR ||
        root == 0) {
        return -1;
    }

    memset(&unit_ctx, 0, sizeof(unit_ctx));
    unit_ctx.viewport_width = INTTOFIX(viewport_width);
    unit_ctx.viewport_height = INTTOFIX(viewport_height);
    unit_ctx.font_size_default = INTTOFIX(16);
    unit_ctx.font_size_minimum = INTTOFIX(6);
    unit_ctx.device_dpi = INTTOFIX(96);

    memset(&media, 0, sizeof(media));
    media.type = CSS_MEDIA_SCREEN;
    media.width = unit_ctx.viewport_width;
    media.height = unit_ctx.viewport_height;
    media.aspect_ratio = viewport_height != 0u
        ? FDIV(media.width, media.height)
        : INTTOFIX(1);
    media.orientation = viewport_width >= viewport_height
        ? CSS_MEDIA_ORIENTATION_LANDSCAPE
        : CSS_MEDIA_ORIENTATION_PORTRAIT;
    media.resolution.value = INTTOFIX(96);
    media.resolution.unit = CSS_UNIT_PX;
    media.color = INTTOFIX(24);
    media.pointer = CSS_MEDIA_POINTER_FINE;
    media.any_pointer = CSS_MEDIA_POINTER_FINE;
    media.hover = CSS_MEDIA_HOVER_HOVER;
    media.any_hover = CSS_MEDIA_HOVER_HOVER;
    media.scripting = CSS_MEDIA_SCRIPTING_NONE;

    error = css_select_style(select_ctx, root, &unit_ctx, &media, 0,
        &zbrowser_engine_select_handler, 0, &results);
    if (error != CSS_OK || results == 0) {
        goto out;
    }

    style = results->styles[CSS_PSEUDO_ELEMENT_NONE];
    if (style == 0) {
        goto out;
    }

    zbrowser_engine_root_color = 0xff000000u;
    zbrowser_engine_root_background = 0xffffffffu;
    (void)css_computed_color(style, &zbrowser_engine_root_color);
    if (css_computed_background_color(style, &zbrowser_engine_root_background) != CSS_BACKGROUND_COLOR_COLOR) {
        zbrowser_engine_root_background = 0xffffffffu;
    }
    zbrowser_engine_root_display = css_computed_display(style, true);
    zbrowser_engine_css_selected = 1;
    result = 0;

out:
    if (results != 0) {
        css_select_results_destroy(results);
    }
    dom_node_unref(root);
    return result;
}

static int zbrowser_engine_create_select_ctx(css_select_ctx **out_ctx,
        css_stylesheet **sheets, uint32_t sheet_count) {
    css_select_ctx *select_ctx = 0;
    css_error error;

    *out_ctx = 0;
    error = css_select_ctx_create(&select_ctx);
    if (error != CSS_OK || select_ctx == 0) {
        return -1;
    }
    if (zbrowser_engine_ua_sheet != 0) {
        error = css_select_ctx_append_sheet(select_ctx, zbrowser_engine_ua_sheet,
                                            CSS_ORIGIN_UA, "screen");
        if (error != CSS_OK) {
            css_select_ctx_destroy(select_ctx);
            return -1;
        }
    }
    for (uint32_t i = 0; i < sheet_count; ++i) {
        error = css_select_ctx_append_sheet(select_ctx, sheets[i],
                                            CSS_ORIGIN_AUTHOR, "screen");
        if (error != CSS_OK) {
            css_select_ctx_destroy(select_ctx);
            return -1;
        }
    }
    *out_ctx = select_ctx;
    return 0;
}

static void zbrowser_engine_setup_media(html_content *htmlc,
        uint32_t viewport_width, uint32_t viewport_height) {
    memset(&htmlc->media, 0, sizeof(htmlc->media));
    htmlc->media.type = CSS_MEDIA_SCREEN;
    htmlc->media.width = INTTOFIX(viewport_width);
    htmlc->media.height = INTTOFIX(viewport_height);
    htmlc->media.aspect_ratio = viewport_height != 0u
        ? FDIV(htmlc->media.width, htmlc->media.height)
        : INTTOFIX(1);
    htmlc->media.orientation = viewport_width >= viewport_height
        ? CSS_MEDIA_ORIENTATION_LANDSCAPE
        : CSS_MEDIA_ORIENTATION_PORTRAIT;
    htmlc->media.resolution.value = INTTOFIX(96);
    htmlc->media.resolution.unit = CSS_UNIT_PX;
    htmlc->media.color = INTTOFIX(24);
    htmlc->media.pointer = CSS_MEDIA_POINTER_FINE;
    htmlc->media.any_pointer = CSS_MEDIA_POINTER_FINE;
    htmlc->media.hover = CSS_MEDIA_HOVER_HOVER;
    htmlc->media.any_hover = CSS_MEDIA_HOVER_HOVER;
    htmlc->media.scripting = CSS_MEDIA_SCRIPTING_NONE;

    memset(&htmlc->unit_len_ctx, 0, sizeof(htmlc->unit_len_ctx));
    htmlc->unit_len_ctx.viewport_width = htmlc->media.width;
    htmlc->unit_len_ctx.viewport_height = htmlc->media.height;
    htmlc->unit_len_ctx.font_size_default = INTTOFIX(16);
    htmlc->unit_len_ctx.font_size_minimum = INTTOFIX(6);
    htmlc->unit_len_ctx.device_dpi = INTTOFIX(96);
}

static void zbrowser_engine_dom_to_box_done(html_content *htmlc, bool success) {
    if (htmlc != 0) {
        htmlc->box_conversion_context = 0;
    }
    zbrowser_engine_html_ready = success ? 1 : -1;
}

static const uint8_t *zbrowser_engine_detail_status(const char *prefix,
        const char *detail,
        const char *suffix,
        const char *suffix2,
        const char *suffix3) {
    uint32_t pos = 0;
    const char *part;
    uint32_t i;

    zbrowser_engine_status_detail[0] = 0;
    for (i = 0; i < 5u; ++i) {
        part = i == 0u ? prefix :
            (i == 1u ? detail :
            (i == 2u ? suffix :
            (i == 3u ? suffix2 : suffix3)));
        if (part == 0) {
            continue;
        }
        while (*part != 0 && pos + 1u < sizeof(zbrowser_engine_status_detail)) {
            zbrowser_engine_status_detail[pos++] = (uint8_t)*part++;
        }
    }
    zbrowser_engine_status_detail[pos] = 0;
    return zbrowser_engine_status_detail;
}

struct nsurl *zbrowser_engine_current_base_url(void) {
    return zbrowser_engine_base_url;
}

static const char *zbrowser_engine_url_for_nsurl(const uint8_t *url) {
    const char *text;
    size_t pos;
    size_t out;

    if (url == 0 || url[0] == 0) {
        return "about:blank";
    }

    text = (const char *)url;
    for (pos = 0; text[pos] != 0; ++pos) {
        if (text[pos] == ':') {
            return text;
        }
        if (text[pos] == '/' || text[pos] == '?' || text[pos] == '#') {
            break;
        }
    }

    memcpy(zbrowser_engine_normalized_url, "file:///", 8u);
    out = 8u;
    for (pos = 0; text[pos] != 0 && out + 1u < sizeof(zbrowser_engine_normalized_url); ++pos) {
        zbrowser_engine_normalized_url[out++] = text[pos] == '\\' ? '/' : text[pos];
    }
    zbrowser_engine_normalized_url[out] = 0;
    return zbrowser_engine_normalized_url;
}

typedef struct {
    struct rect clip;
    unsigned int rectangles;
    unsigned int texts;
    unsigned int visible_texts;
    unsigned int bitmaps;
    unsigned int visible_bitmaps;
    unsigned int lines;
    unsigned int polygons;
    unsigned int visible_polygons;
    unsigned int discs;
    unsigned int visible_discs;
    unsigned int arcs;
    unsigned int visible_arcs;
    unsigned int paths;
    unsigned int visible_paths;
    unsigned int text_bytes;
    unsigned int text_width;
} zbrowser_engine_plot_ctx_t;

static const struct plotter_table zbrowser_engine_plotters;

#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
extern nserror zbrowser_lainos_resource_fetcher_register(void);
extern nserror zbrowser_lainos_file_fetcher_register(void);
extern nserror zbrowser_lainos_http_fetcher_register(void);
extern void zbrowser_lainos_schedule_clear(void);
extern unsigned int zbrowser_lainos_schedule_count(void);
extern unsigned int zbrowser_lainos_run_scheduled_budget(unsigned int callback_limit);
extern unsigned int zbrowser_lainos_fetch_pending_count(void);
extern unsigned int zbrowser_lainos_pump_fetchers(void);
extern void zbrowser_lainos_net_stats_reset(void);
extern uint32_t zbrowser_lainos_net_http_fetches(void);
extern uint32_t zbrowser_lainos_net_http_successes(void);
extern uint32_t zbrowser_lainos_net_http_errors(void);
extern uint32_t zbrowser_lainos_net_http_last_status(void);
extern uint32_t zbrowser_lainos_net_http_last_bytes(void);
extern int32_t zbrowser_lainos_net_http_last_error(void);
extern uint32_t zbrowser_lainos_net_content_type_rejects(void);
extern uint32_t zbrowser_lainos_net_cache_hits(void);
extern uint32_t zbrowser_lainos_net_cache_entries(void);
extern uint32_t zbrowser_lainos_net_cache_kib(void);
extern uint32_t zbrowser_lainos_net_cache_stores(void);
extern uint32_t zbrowser_lainos_net_cache_evictions(void);
extern uint32_t zbrowser_lainos_net_css_compat_transforms(void);
extern uint32_t zbrowser_lainos_net_css_compat_vars(void);
extern uint32_t zbrowser_lainos_net_css_compat_groups(void);
extern uint32_t zbrowser_lainos_net_image_decodes(void);
extern uint32_t zbrowser_lainos_net_image_fallbacks(void);
extern uint32_t zbrowser_lainos_net_image_errors(void);
extern uint32_t zbrowser_lainos_net_bitmap_renders(void);
extern uint32_t zbrowser_lainos_net_bitmap_render_successes(void);
extern uint32_t zbrowser_lainos_net_bitmap_render_errors(void);
extern uint32_t zbrowser_lainos_navigation_creates(void);
extern uint32_t zbrowser_lainos_navigation_consumes(void);
extern uint32_t zbrowser_lainos_js_execs(void);
extern uint32_t zbrowser_lainos_js_exec_successes(void);
extern uint32_t zbrowser_lainos_frontend_smoke(void);
extern const char *zbrowser_lainos_frontend_status(void);

#define ZBROWSER_ENGINE_PREPARE_CALLBACK_BUDGET 64u
#define ZBROWSER_ENGINE_PREPARE_ROUND_LIMIT 512u
#define ZBROWSER_ENGINE_PREPARE_STALL_LIMIT 48u
#define ZBROWSER_ENGINE_POLL_CALLBACK_BUDGET 8u

static int zbrowser_engine_core_init(void) {
    bitmap_fmt_t bitmap_format = {
        .layout = BITMAP_LAYOUT_R8G8B8A8,
        .pma = false,
    };
    nserror error;

    if (zbrowser_engine_core_initialised != 0) {
        return 0;
    }
    error = nsoption_init(zbrowser_engine_options, 0, 0);
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
    error = corestrings_init();
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
    error = nscolour_update();
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
    bitmap_set_format(&bitmap_format);
    zbrowser_engine_core_initialised = 1;
    return 0;
}

static void zbrowser_engine_content_user(struct content *content,
        content_msg msg,
        const union content_msg_data *data,
        void *pw) {
    (void)content;
    (void)pw;
    if (msg == CONTENT_MSG_GETDIMS && data != 0) {
        if (data->getdims.viewport_width != 0) {
            *data->getdims.viewport_width = (unsigned)zbrowser_engine_content_viewport_width;
        }
        if (data->getdims.viewport_height != 0) {
            *data->getdims.viewport_height = (unsigned)zbrowser_engine_content_viewport_height;
        }
        return;
    }
    if (msg == CONTENT_MSG_GETTHREAD && data != 0 && data->jsthread != 0) {
        struct jsthread *thread = 0;
        if (zbrowser_engine_content_jsheap == 0 &&
            js_newheap(nsoption_int(script_timeout),
                       &zbrowser_engine_content_jsheap) != NSERROR_OK) {
            zbrowser_engine_status_text = zbrowser_engine_status_content_convert_failed;
            return;
        }
        if (js_newthread(zbrowser_engine_content_jsheap,
                         (void *)&zbrowser_engine_content_browser_window_cookie,
                         zbrowser_engine_content,
                         &thread) == NSERROR_OK) {
            *data->jsthread = thread;
        }
        return;
    }
    if (msg == CONTENT_MSG_READY || msg == CONTENT_MSG_DONE) {
        zbrowser_engine_content_prepare_pending = 0;
        zbrowser_engine_content_needs_reformat = 1;
        zbrowser_engine_content_needs_redraw = 1;
        return;
    }
    if (msg == CONTENT_MSG_REFORMAT) {
        zbrowser_engine_content_needs_reformat = 1;
        zbrowser_engine_content_needs_redraw = 1;
        return;
    }
    if (msg == CONTENT_MSG_REDRAW ||
        msg == CONTENT_MSG_CARET) {
        zbrowser_engine_content_needs_redraw = 1;
        return;
    }
    if (msg == CONTENT_MSG_STATUS ||
        msg == CONTENT_MSG_POINTER) {
        zbrowser_engine_content_needs_chrome = 1;
        return;
    }
    if (msg == CONTENT_MSG_ERROR) {
        const char *error_msg = data != 0 && data->errordata.errormsg != 0
            ? data->errordata.errormsg
            : "";
        if ((error_msg == 0 || error_msg[0] == 0) &&
            data != 0 &&
            data->errordata.errorcode == NSERROR_BOX_CONVERT) {
            error_msg = "box conversion";
        }
        zbrowser_engine_content_needs_redraw = 1;
        snprintf(zbrowser_engine_content_error_detail,
                 sizeof(zbrowser_engine_content_error_detail),
                 "ce%d %.80s",
                 data != 0 ? (int)data->errordata.errorcode : 0,
                 error_msg);
        zbrowser_engine_content_error_detail[sizeof(zbrowser_engine_content_error_detail) - 1u] = 0;
        zbrowser_engine_status_text = zbrowser_engine_status_content_convert_failed;
    }
}

static int zbrowser_engine_content_init(void) {
    nserror error;

    if (zbrowser_engine_content_initialised != 0) {
        return 0;
    }
    if (zbrowser_engine_core_init() != 0) {
        return -1;
    }
    error = nscss_init();
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
    error = zbrowser_lainos_image_init();
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
    error = html_init();
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    error = fetcher_init();
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
#endif
    error = zbrowser_lainos_resource_fetcher_register();
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
    error = zbrowser_lainos_file_fetcher_register();
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
    error = zbrowser_lainos_http_fetcher_register();
    if (error != NSERROR_OK) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
        return -1;
    }
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    {
        struct hlcache_parameters cache_params;

        memset(&cache_params, 0, sizeof(cache_params));
        cache_params.bg_clean_time = 1000u;
        cache_params.llcache.limit = 32u * 1024u * 1024u;
        cache_params.llcache.hysteresis = 4u * 1024u * 1024u;
        cache_params.llcache.fetch_attempts = 1u;
        error = hlcache_initialise(&cache_params);
        if (error != NSERROR_OK) {
            zbrowser_engine_status_text = zbrowser_engine_status_content_init_failed;
            return -1;
        }
    }
#endif
    js_initialise();
    zbrowser_engine_content_initialised = 1;
    return 0;
}

static void zbrowser_engine_release_content(void) {
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    if (zbrowser_engine_content != 0) {
        if (zbrowser_engine_content_opened != 0 &&
            zbrowser_engine_content->handler != 0 &&
            zbrowser_engine_content->handler->close != 0) {
            (void)zbrowser_engine_content->handler->close(zbrowser_engine_content);
            zbrowser_engine_content_opened = 0;
        }
        if (zbrowser_engine_content->handler != 0 &&
            zbrowser_engine_content->handler->destroy != 0) {
            zbrowser_engine_content->handler->destroy(zbrowser_engine_content);
        }
        if (zbrowser_engine_content->mime_type != 0) {
            lwc_string_unref(zbrowser_engine_content->mime_type);
        }
        free(zbrowser_engine_content->user_list);
        free(zbrowser_engine_content->title);
        free(zbrowser_engine_content->fallback_charset);
        free(zbrowser_engine_content);
    }
    zbrowser_engine_content = 0;
    memset(&zbrowser_engine_content_object, 0, sizeof(zbrowser_engine_content_object));
    memset(&zbrowser_engine_content_llcache, 0, sizeof(zbrowser_engine_content_llcache));
    memset(zbrowser_engine_content_headers, 0, sizeof(zbrowser_engine_content_headers));
#else
    if (zbrowser_engine_content != 0) {
        content_destroy(zbrowser_engine_content);
        zbrowser_engine_content = 0;
    }
#endif
    if (zbrowser_engine_content_jsheap != 0) {
        js_destroyheap(zbrowser_engine_content_jsheap);
        zbrowser_engine_content_jsheap = 0;
    }
    zbrowser_engine_content_opened = 0;
    zbrowser_engine_content_needs_reformat = 0;
    zbrowser_engine_content_needs_redraw = 0;
    zbrowser_engine_content_needs_chrome = 0;
    zbrowser_engine_content_prepare_pending = 0;
    zbrowser_engine_content_rendered_once = 0;
    zbrowser_engine_content_error_detail[0] = 0;
    zbrowser_lainos_schedule_clear();
    if (zbrowser_engine_content_url != 0) {
        nsurl_unref(zbrowser_engine_content_url);
        zbrowser_engine_content_url = 0;
    }
#ifndef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    memset(&zbrowser_engine_content_llcache, 0, sizeof(zbrowser_engine_content_llcache));
#endif
}

static void zbrowser_engine_run_scheduled_until_ready(struct content *content) {
    unsigned int rounds = 0;
    unsigned int stalled = 0;
    unsigned int last_pending = 0xffffffffu;

    while (content != 0 &&
           content->status != CONTENT_STATUS_READY &&
           content->status != CONTENT_STATUS_DONE &&
           rounds++ < ZBROWSER_ENGINE_PREPARE_ROUND_LIMIT) {
        unsigned int pending = zbrowser_lainos_schedule_count() +
                               zbrowser_lainos_fetch_pending_count() +
                               content->active;
        unsigned int ran;

        if (pending == 0u) {
            break;
        }
        ran = zbrowser_lainos_run_scheduled_budget(ZBROWSER_ENGINE_PREPARE_CALLBACK_BUDGET);
        ran += zbrowser_lainos_pump_fetchers();
        if (pending == last_pending) {
            ++stalled;
        } else {
            stalled = 0;
            last_pending = pending;
        }
        if (ran == 0u || stalled >= ZBROWSER_ENGINE_PREPARE_STALL_LIMIT) {
            break;
        }
    }
}

static void zbrowser_engine_kick_html_conversion(struct content *content) {
    if (content == 0 ||
        content->status != CONTENT_STATUS_LOADING ||
        content->active != 0u ||
        content->handler == 0 ||
        content->handler->type == 0 ||
        content->handler->type() != CONTENT_HTML) {
        return;
    }
    if (html_can_begin_conversion((html_content *)content)) {
        (void)html_begin_conversion((html_content *)content);
    }
}

typedef struct {
    unsigned int boxes;
    unsigned int text_boxes;
    unsigned int text_bytes;
    unsigned int object_boxes;
    unsigned int max_depth;
    unsigned int negative_boxes;
    unsigned int overwide_boxes;
    int text_min_y;
    int text_max_y;
    int min_left;
    int min_top;
    int max_right;
    int max_bottom;
    unsigned int viewport_text_boxes;
    unsigned int viewport_text_bytes;
    unsigned int viewport_visible_text_boxes;
    unsigned int viewport_visible_text_bytes;
    unsigned int viewport_hidden_text_boxes;
    unsigned int viewport_zero_text_boxes;
    unsigned int grid_boxes;
    unsigned int first_grid_child_count;
    int first_grid_x;
    int first_grid_y;
    int first_grid_width;
    int first_grid_height;
    int first_grid_child_min_y;
    int first_grid_child_max_bottom;
    unsigned int largest_grid_child_count;
    int largest_grid_x;
    int largest_grid_y;
    int largest_grid_width;
    int largest_grid_height;
    int largest_grid_child_min_y;
    int largest_grid_child_max_bottom;
} zbrowser_engine_box_stats_t;

static void zbrowser_engine_count_box_tree(const struct box *box,
        unsigned int depth,
        int parent_x,
        int parent_y,
        zbrowser_engine_box_stats_t *stats) {
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
            if (stats->text_boxes == 1u || abs_y < stats->text_min_y) {
                stats->text_min_y = abs_y;
            }
            if (stats->text_boxes == 1u || bottom > stats->text_max_y) {
                stats->text_max_y = bottom;
            }
        }
        if (box->object != 0 || box->background != 0) {
            ++stats->object_boxes;
        }
        if (box->style != 0) {
            uint8_t display = css_computed_display(box->style, false);
            if (display == CSS_DISPLAY_GRID ||
                display == CSS_DISPLAY_INLINE_GRID) {
                const struct box *grid_child;
                unsigned int child_count = 0;
                int child_min_y = 0;
                int child_max_bottom = 0;

                ++stats->grid_boxes;
                for (grid_child = box->children;
                     grid_child != 0;
                     grid_child = grid_child->next) {
                    int child_bottom = grid_child->y + grid_child->height;

                    ++child_count;
                    if (child_count == 1u ||
                        grid_child->y < child_min_y) {
                        child_min_y = grid_child->y;
                    }
                    if (child_bottom > child_max_bottom) {
                        child_max_bottom = child_bottom;
                    }
                }
                if (stats->grid_boxes == 1u) {
                    stats->first_grid_x = abs_x;
                    stats->first_grid_y = abs_y;
                    stats->first_grid_width = box->width;
                    stats->first_grid_height = box->height;
                    stats->first_grid_child_count = child_count;
                    stats->first_grid_child_min_y = child_min_y;
                    stats->first_grid_child_max_bottom = child_max_bottom;
                }
                if (box->height > stats->largest_grid_height ||
                    (box->height == stats->largest_grid_height &&
                     box->width > stats->largest_grid_width)) {
                    stats->largest_grid_x = abs_x;
                    stats->largest_grid_y = abs_y;
                    stats->largest_grid_width = box->width;
                    stats->largest_grid_height = box->height;
                    stats->largest_grid_child_count = child_count;
                    stats->largest_grid_child_min_y = child_min_y;
                    stats->largest_grid_child_max_bottom = child_max_bottom;
                }
            }
        }
        if (abs_x < 0 || abs_y < 0) {
            ++stats->negative_boxes;
        }
        if (box->width > zbrowser_engine_content_viewport_width * 4u ||
            box->height > zbrowser_engine_content_viewport_height * 4u) {
            ++stats->overwide_boxes;
        }
        if (right > stats->max_right) {
            stats->max_right = right;
        }
        if (bottom > stats->max_bottom) {
            stats->max_bottom = bottom;
        }
        if (box->children != 0) {
            zbrowser_engine_count_box_tree(box->children,
                                           depth + 1u,
                                           abs_x,
                                           abs_y,
                                           stats);
        }
        if (box->list_marker != 0) {
            zbrowser_engine_count_box_tree(box->list_marker,
                                           depth + 1u,
                                           abs_x,
                                           abs_y,
                                           stats);
        }
        box = box->next;
    }
}

static void zbrowser_engine_html_box_stats(zbrowser_engine_box_stats_t *stats) {
    const html_content *html = (const html_content *)zbrowser_engine_content;

    memset(stats, 0, sizeof(*stats));
    if (html == 0 ||
        zbrowser_engine_content == 0 ||
        zbrowser_engine_content->handler == 0 ||
        zbrowser_engine_content->handler->type == 0 ||
        zbrowser_engine_content->handler->type() != CONTENT_HTML ||
        html->layout == 0) {
        return;
    }
    zbrowser_engine_count_box_tree(html->layout, 0u, 0, 0, stats);
}

static void zbrowser_engine_html_resource_stats(unsigned int *css_total,
        unsigned int *css_loaded,
        unsigned int *obj_total,
        unsigned int *obj_loaded) {
    html_content *html = (html_content *)zbrowser_engine_content;

    *css_total = 0;
    *css_loaded = 0;
    *obj_total = 0;
    *obj_loaded = 0;
    if (html == 0 || zbrowser_engine_content == 0 ||
        zbrowser_engine_content->handler == 0 ||
        zbrowser_engine_content->handler->type == 0 ||
        zbrowser_engine_content->handler->type() != CONTENT_HTML) {
        return;
    }
    *css_total = 0;
    for (unsigned int i = 0; i < html->stylesheet_count; ++i) {
        bool count_sheet = i >= STYLESHEET_START ||
            (html->stylesheets != 0 && html->stylesheets[i].sheet != 0);

        if (count_sheet) {
            ++*css_total;
        }
        if (html->stylesheets != 0 && html->stylesheets[i].sheet != 0) {
            content_status status = content_get_status(html->stylesheets[i].sheet);
            if (status == CONTENT_STATUS_READY ||
                status == CONTENT_STATUS_DONE) {
                ++*css_loaded;
            }
        }
    }
    *obj_total = html->num_objects;
    for (struct content_html_object *object = html->object_list;
         object != 0;
         object = object->next) {
        if (object->content != 0) {
            content_status object_status = content_get_status(object->content);
            if (object_status == CONTENT_STATUS_READY ||
                object_status == CONTENT_STATUS_DONE) {
                ++*obj_loaded;
            }
        }
    }
}

static void zbrowser_engine_count_viewport_text_boxes(const struct box *box,
        int draw_x,
        int draw_y,
        int ancestor_visible,
        const struct rect *clip,
        zbrowser_engine_box_stats_t *stats) {
    while (box != 0) {
        int abs_x = 0;
        int abs_y = 0;
        int width = box->width > 0 ? box->width : 1;
        int height = box->height > 0 ? box->height : 1;
        int screen_x;
        int screen_y;
        int visible = ancestor_visible;

        box_coords((struct box *)box, &abs_x, &abs_y);
        screen_x = draw_x + abs_x;
        screen_y = draw_y + abs_y;
        if (box->style != 0 &&
            (css_computed_visibility(box->style) == CSS_VISIBILITY_HIDDEN ||
             css_computed_visibility(box->style) == CSS_VISIBILITY_COLLAPSE)) {
            visible = 0;
        }

        if (box->type == BOX_TEXT && box->text != 0 && box->length != 0u &&
            clip != 0 &&
            screen_x < clip->x1 &&
            screen_y < clip->y1 &&
            screen_x + width > clip->x0 &&
            screen_y + height > clip->y0) {
            ++stats->viewport_text_boxes;
            stats->viewport_text_bytes += (unsigned int)box->length;
            if (box->width <= 0 || box->height <= 0) {
                ++stats->viewport_zero_text_boxes;
            } else if (visible) {
                ++stats->viewport_visible_text_boxes;
                stats->viewport_visible_text_bytes += (unsigned int)box->length;
            } else {
                ++stats->viewport_hidden_text_boxes;
            }
        }
        if (box->children != 0) {
            zbrowser_engine_count_viewport_text_boxes(box->children,
                                                      draw_x,
                                                      draw_y,
                                                      visible,
                                                      clip,
                                                      stats);
        }
        if (box->list_marker != 0) {
            zbrowser_engine_count_viewport_text_boxes(box->list_marker,
                                                      draw_x,
                                                      draw_y,
                                                      visible,
                                                      clip,
                                                      stats);
        }
        box = box->next;
    }
}

static void zbrowser_engine_viewport_box_stats(int draw_x,
        int draw_y,
        const struct rect *clip,
        zbrowser_engine_box_stats_t *stats) {
    const html_content *html = (const html_content *)zbrowser_engine_content;

    if (stats == 0) {
        return;
    }
    stats->viewport_text_boxes = 0;
    stats->viewport_text_bytes = 0;
    stats->viewport_visible_text_boxes = 0;
    stats->viewport_visible_text_bytes = 0;
    stats->viewport_hidden_text_boxes = 0;
    stats->viewport_zero_text_boxes = 0;
    if (html != 0 && html->layout != 0) {
        zbrowser_engine_count_viewport_text_boxes(html->layout,
                                                  draw_x,
                                                  draw_y,
                                                  1,
                                                  clip,
                                                  stats);
    }
}

static void zbrowser_engine_html_script_stats(unsigned int *script_total,
        unsigned int *script_handles,
        unsigned int *script_pending,
        unsigned int *script_not_started,
        unsigned int *script_inline,
        unsigned int *parse_complete,
        unsigned int *has_jsthread) {
    html_content *html = (html_content *)zbrowser_engine_content;

    *script_total = 0;
    *script_handles = 0;
    *script_pending = 0;
    *script_not_started = 0;
    *script_inline = 0;
    *parse_complete = 0;
    *has_jsthread = 0;
    if (html == 0 || zbrowser_engine_content == 0 ||
        zbrowser_engine_content->handler == 0 ||
        zbrowser_engine_content->handler->type == 0 ||
        zbrowser_engine_content->handler->type() != CONTENT_HTML) {
        return;
    }
    *script_total = html->scripts_count;
    *parse_complete = html->parse_completed ? 1u : 0u;
    *has_jsthread = html->jsthread != 0 ? 1u : 0u;
    for (unsigned int i = 0; i < html->scripts_count; ++i) {
        const struct html_script *script = &html->scripts[i];
        if (script->type == HTML_SCRIPT_INLINE) {
            ++*script_inline;
            continue;
        }
        if (script->data.handle != 0) {
            content_status status = content_get_status(script->data.handle);
            ++*script_handles;
            if (status != CONTENT_STATUS_READY &&
                status != CONTENT_STATUS_DONE &&
                status != CONTENT_STATUS_ERROR) {
                ++*script_pending;
            }
        }
        if (!script->already_started) {
            ++*script_not_started;
        }
    }
}

static const uint8_t *zbrowser_engine_content_detail_status(const char *phase) {
    unsigned int css_total = 0;
    unsigned int css_loaded = 0;
    unsigned int obj_total = 0;
    unsigned int obj_loaded = 0;
    unsigned int script_total = 0;
    unsigned int script_handles = 0;
    unsigned int script_pending = 0;
    unsigned int script_not_started = 0;
    unsigned int script_inline = 0;
    unsigned int parse_complete = 0;
    unsigned int has_jsthread = 0;
    zbrowser_engine_box_stats_t box_stats;
    const char *status_name = "none";
    int status_value = -1;

    zbrowser_engine_html_box_stats(&box_stats);
    zbrowser_engine_html_resource_stats(&css_total,
                                        &css_loaded,
                                        &obj_total,
                                        &obj_loaded);
    zbrowser_engine_html_script_stats(&script_total,
                                      &script_handles,
                                      &script_pending,
                                      &script_not_started,
                                      &script_inline,
                                      &parse_complete,
                                      &has_jsthread);
    if (zbrowser_engine_content != 0) {
        status_value = (int)zbrowser_engine_content->status;
        switch (zbrowser_engine_content->status) {
        case CONTENT_STATUS_LOADING:
            status_name = "loading";
            break;
        case CONTENT_STATUS_READY:
            status_name = "ready";
            break;
        case CONTENT_STATUS_DONE:
            status_name = "done";
            break;
        case CONTENT_STATUS_ERROR:
            status_name = "error";
            break;
        default:
            status_name = "unknown";
            break;
        }
    }
    if (zbrowser_engine_content != 0 &&
        zbrowser_engine_content->status == CONTENT_STATUS_ERROR) {
        snprintf((char *)zbrowser_engine_status_detail,
                 sizeof(zbrowser_engine_status_detail),
                 "NS %s error act%u dc%u/%u %u>%u detail:%s sched%u js%u/%u jse%u/%u pc%u css%u/%u obj%u/%u http%u/%u fail%u tr%u st%u err%d bytes%u frm%u/%u/%u/%u",
                 phase != 0 ? phase : "content",
                 zbrowser_engine_content->active,
                 zbrowser_engine_content_data_complete_ok,
                 zbrowser_engine_content_data_complete_called,
                 zbrowser_engine_content_active_before_complete,
                 zbrowser_engine_content_active_after_complete,
                 zbrowser_engine_content_error_detail,
                 zbrowser_lainos_schedule_count() + zbrowser_lainos_fetch_pending_count(),
                 script_handles,
                 script_total,
                 zbrowser_lainos_js_exec_successes(),
                 zbrowser_lainos_js_execs(),
                 parse_complete,
                 css_loaded,
                 css_total,
                 obj_loaded,
                 obj_total,
                 zbrowser_lainos_net_http_successes(),
                 zbrowser_lainos_net_http_fetches(),
                 zbrowser_lainos_net_http_errors(),
                 zbrowser_lainos_net_content_type_rejects(),
                 zbrowser_lainos_net_http_last_status(),
                 zbrowser_lainos_net_http_last_error(),
                 zbrowser_lainos_net_http_last_bytes(),
                 zbrowser_engine_form_key_count,
                 zbrowser_engine_form_mouse_count,
                 zbrowser_lainos_navigation_creates(),
                 zbrowser_lainos_navigation_consumes());
        zbrowser_engine_status_detail[sizeof(zbrowser_engine_status_detail) - 1u] = 0;
        return zbrowser_engine_status_detail;
    }
    snprintf((char *)zbrowser_engine_status_detail,
             sizeof(zbrowser_engine_status_detail),
             "NS %s %s(%d) act%u dc%u/%u %u>%u sched%u js%u/%u jse%u/%u p%u n%u i%u pc%u jt%u css%u/%u obj%u/%u box%u txt%u/%u objb%u ext%dx%d wh%dx%d http%u/%u fail%u tr%u hit%u/%u ce%u/%u/%u cb%u st%u err%d bytes%u img%u fb%u ie%u br%u/%u/%u frm%u/%u/%u/%u %s",
             phase != 0 ? phase : "content",
             status_name,
             status_value,
             zbrowser_engine_content != 0 ? zbrowser_engine_content->active : 0u,
             zbrowser_engine_content_data_complete_ok,
             zbrowser_engine_content_data_complete_called,
             zbrowser_engine_content_active_before_complete,
             zbrowser_engine_content_active_after_complete,
             zbrowser_lainos_schedule_count() + zbrowser_lainos_fetch_pending_count(),
             script_handles,
             script_total,
             zbrowser_lainos_js_exec_successes(),
             zbrowser_lainos_js_execs(),
             script_pending,
             script_not_started,
             script_inline,
             parse_complete,
             has_jsthread,
             css_loaded,
             css_total,
             obj_loaded,
             obj_total,
             box_stats.boxes,
             box_stats.text_boxes,
             box_stats.text_bytes,
             box_stats.object_boxes,
             box_stats.max_right,
             box_stats.max_bottom,
             zbrowser_engine_content != 0 ? zbrowser_engine_content->width : 0,
             zbrowser_engine_content != 0 ? zbrowser_engine_content->height : 0,
             zbrowser_lainos_net_http_successes(),
             zbrowser_lainos_net_http_fetches(),
             zbrowser_lainos_net_http_errors(),
             zbrowser_lainos_net_content_type_rejects(),
             zbrowser_lainos_net_cache_hits(),
             zbrowser_lainos_net_cache_entries(),
             zbrowser_lainos_net_css_compat_transforms(),
             zbrowser_lainos_net_css_compat_vars(),
             zbrowser_lainos_net_css_compat_groups(),
             zbrowser_lainos_net_cache_kib(),
             zbrowser_lainos_net_http_last_status(),
             zbrowser_lainos_net_http_last_error(),
             zbrowser_lainos_net_http_last_bytes(),
             zbrowser_lainos_net_image_decodes(),
             zbrowser_lainos_net_image_fallbacks(),
             zbrowser_lainos_net_image_errors(),
             zbrowser_lainos_net_bitmap_render_successes(),
             zbrowser_lainos_net_bitmap_renders(),
             zbrowser_lainos_net_bitmap_render_errors(),
             zbrowser_engine_form_key_count,
             zbrowser_engine_form_mouse_count,
             zbrowser_lainos_navigation_creates(),
             zbrowser_lainos_navigation_consumes(),
             zbrowser_engine_content_error_detail);
    zbrowser_engine_status_detail[sizeof(zbrowser_engine_status_detail) - 1u] = 0;
    return zbrowser_engine_status_detail;
}

static int zbrowser_engine_prepare_content_pipeline(const uint8_t *url,
        const uint8_t *html,
        uint32_t size,
        uint32_t viewport_width,
        uint32_t viewport_height) {
    lwc_string *mime = 0;
    nserror error;

    if (zbrowser_engine_content_init() != 0) {
        return -1;
    }
    zbrowser_engine_release_content();
    zbrowser_engine_content_source_html = html;
    zbrowser_engine_content_source_size = size;
    zbrowser_engine_content_source_url[0] = 0;
    if (url != 0) {
        size_t i = 0;
        while (url[i] != 0 && i + 1u < sizeof(zbrowser_engine_content_source_url)) {
            zbrowser_engine_content_source_url[i] = (char)url[i];
            ++i;
        }
        zbrowser_engine_content_source_url[i] = 0;
    }
    zbrowser_lainos_net_stats_reset();
    zbrowser_engine_form_key_count = 0;
    zbrowser_engine_form_mouse_count = 0;
    zbrowser_engine_content_viewport_width = viewport_width;
    zbrowser_engine_content_viewport_height = viewport_height;
    error = nsurl_create(zbrowser_engine_url_for_nsurl(url),
                         &zbrowser_engine_content_url);
    if (error != NSERROR_OK || zbrowser_engine_content_url == 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_html_layout_base_url_failed;
        return -1;
    }
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    if (lwc_intern_string("text/html", 9, &mime) != lwc_error_ok) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_create_failed;
        return -1;
    }
    memset(&zbrowser_engine_content_object, 0, sizeof(zbrowser_engine_content_object));
    memset(&zbrowser_engine_content_llcache, 0, sizeof(zbrowser_engine_content_llcache));
    zbrowser_engine_content_headers[0].name = "Content-Type";
    zbrowser_engine_content_headers[0].value = "text/html; charset=utf-8";
    zbrowser_engine_content_object.url = zbrowser_engine_content_url;
    zbrowser_engine_content_object.source_data = (uint8_t *)html;
    zbrowser_engine_content_object.source_len = size;
    zbrowser_engine_content_object.source_alloc = size;
    zbrowser_engine_content_object.store_state = 0;
    zbrowser_engine_content_object.headers = zbrowser_engine_content_headers;
    zbrowser_engine_content_object.num_headers = 1u;
    zbrowser_engine_content_llcache.object = &zbrowser_engine_content_object;

    zbrowser_engine_content = content_factory_create_content(
        &zbrowser_engine_content_llcache,
        "UTF-8",
        false,
        mime);
    lwc_string_unref(mime);
    if (zbrowser_engine_content == 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_create_failed;
        return -1;
    }
    zbrowser_engine_content_prepare_pending = 1;
    zbrowser_engine_content_needs_reformat = 1;
    zbrowser_engine_content_needs_redraw = 1;
    zbrowser_engine_content_rendered_once = 0;
    zbrowser_engine_content_data_complete_called = 0;
    zbrowser_engine_content_data_complete_ok = 0;
    zbrowser_engine_content_active_before_complete = 0;
    zbrowser_engine_content_active_after_complete = 0;
    if (!content_add_user(zbrowser_engine_content,
                          zbrowser_engine_content_user,
                          0)) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_create_failed;
        return -1;
    }
    if (zbrowser_engine_content->handler == 0 ||
        zbrowser_engine_content->handler->process_data == 0 ||
        zbrowser_engine_content->handler->data_complete == 0 ||
        zbrowser_engine_content->handler->redraw == 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_parse_failed;
        return -1;
    }
    if (!zbrowser_engine_content->handler->process_data(
            zbrowser_engine_content,
            (const char *)html,
            size) &&
        (zbrowser_engine_content->status == CONTENT_STATUS_ERROR ||
         zbrowser_engine_content->active == 0u)) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_parse_failed;
        return -1;
    }
    zbrowser_engine_run_scheduled_until_ready(zbrowser_engine_content);
    zbrowser_engine_content_active_before_complete = zbrowser_engine_content->active;
    ++zbrowser_engine_content_data_complete_called;
    if (!zbrowser_engine_content->handler->data_complete(zbrowser_engine_content)) {
        zbrowser_engine_content_active_after_complete = zbrowser_engine_content->active;
        zbrowser_engine_status_text = zbrowser_engine_status_content_convert_failed;
        return -1;
    }
    zbrowser_engine_content_data_complete_ok = 1;
    zbrowser_engine_content_active_after_complete = zbrowser_engine_content->active;
    zbrowser_engine_run_scheduled_until_ready(zbrowser_engine_content);
    zbrowser_engine_kick_html_conversion(zbrowser_engine_content);
    zbrowser_engine_run_scheduled_until_ready(zbrowser_engine_content);
    if (zbrowser_engine_content->status != CONTENT_STATUS_READY &&
        zbrowser_engine_content->status != CONTENT_STATUS_DONE) {
        zbrowser_engine_status_text = zbrowser_engine_content_detail_status("waiting");
        return 0;
    }
    zbrowser_engine_content_prepare_pending = 0;
    zbrowser_engine_status_text = zbrowser_engine_content_detail_status("prepared");
    return 0;
#else
    if (lwc_intern_string("text/html", 9, &mime) != lwc_error_ok) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_create_failed;
        return -1;
    }
    zbrowser_engine_content_llcache.url = zbrowser_engine_content_url;
    zbrowser_engine_content_llcache.source_data = html;
    zbrowser_engine_content_llcache.source_len = size;
    zbrowser_engine_content_llcache.content_type = "text/html; charset=utf-8";

    zbrowser_engine_content = content_factory_create_content(
        &zbrowser_engine_content_llcache,
        "UTF-8",
        false,
        mime);
    lwc_string_unref(mime);
    if (zbrowser_engine_content == 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_create_failed;
        return -1;
    }
    zbrowser_engine_content_llcache.content = zbrowser_engine_content;
    zbrowser_engine_content_prepare_pending = 1;
    zbrowser_engine_content_needs_reformat = 1;
    zbrowser_engine_content_needs_redraw = 1;
    zbrowser_engine_content_rendered_once = 0;
    if (!content_add_user(zbrowser_engine_content,
                          zbrowser_engine_content_user,
                          0)) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_create_failed;
        return -1;
    }
    if (zbrowser_engine_content->handler == 0 ||
        zbrowser_engine_content->handler->process_data == 0 ||
        zbrowser_engine_content->handler->data_complete == 0 ||
        zbrowser_engine_content->handler->redraw == 0 ||
        !zbrowser_engine_content->handler->process_data(
            zbrowser_engine_content,
            (const char *)html,
            size)) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_parse_failed;
        return -1;
    }
    zbrowser_engine_run_scheduled_until_ready(zbrowser_engine_content);
    if (!zbrowser_engine_content->handler->data_complete(zbrowser_engine_content)) {
        zbrowser_engine_status_text = zbrowser_engine_status_content_convert_failed;
        return -1;
    }
    zbrowser_engine_run_scheduled_until_ready(zbrowser_engine_content);
    if (zbrowser_engine_content->status != CONTENT_STATUS_READY &&
        zbrowser_engine_content->status != CONTENT_STATUS_DONE) {
        zbrowser_engine_status_text = zbrowser_engine_content_detail_status("waiting");
        return 0;
    }
    zbrowser_engine_status_text = zbrowser_engine_content_detail_status("prepared");
    return 0;
#endif
}

static int zbrowser_engine_redraw_content_view(uint32_t x,
        uint32_t y,
        uint32_t viewport_width,
        uint32_t viewport_height,
        uint32_t scroll_px) {
    struct content_redraw_data data;
    struct rect clip;
    struct redraw_context redraw;
    zbrowser_engine_plot_ctx_t plot_ctx;
    int y_offset;
    unsigned int css_total = 0;
    unsigned int css_loaded = 0;
    unsigned int obj_total = 0;
    unsigned int obj_loaded = 0;

    if (zbrowser_engine_content == 0 ||
        (zbrowser_engine_content->status != CONTENT_STATUS_READY &&
         zbrowser_engine_content->status != CONTENT_STATUS_DONE) ||
        zbrowser_engine_content->handler == 0 ||
        zbrowser_engine_content->handler->redraw == 0) {
        return -1;
    }
    zbrowser_engine_content_viewport_width = viewport_width;
    zbrowser_engine_content_viewport_height = viewport_height;
    if (!zbrowser_engine_content_opened &&
        zbrowser_engine_content->handler->open != 0 &&
        zbrowser_engine_content->handler->open(zbrowser_engine_content,
                                               (struct browser_window *)&zbrowser_engine_content_browser_window_cookie,
                                               0,
                                               0) != NSERROR_OK) {
        return -1;
    }
    zbrowser_engine_content_opened = 1;
    if (zbrowser_engine_content_needs_reformat != 0 ||
        zbrowser_engine_content->available_width != (int)viewport_width ||
        zbrowser_engine_content->available_height != (int)viewport_height) {
        zbrowser_engine_content_needs_reformat = 0;
        content__reformat(zbrowser_engine_content,
                          false,
                          (int)viewport_width,
                          (int)viewport_height);
    }

    y_offset = (int)y - (int)scroll_px;
    clip.x0 = (int)x;
    clip.y0 = (int)y;
    clip.x1 = (int)(x + viewport_width);
    clip.y1 = (int)(y + viewport_height);
    memset(&plot_ctx, 0, sizeof(plot_ctx));
    zbrowser_engine_redraw_box_visits = 0;
    zbrowser_engine_redraw_text_box_visits = 0;
    zbrowser_engine_redraw_text_box_paths = 0;
    zbrowser_engine_redraw_clip_skips = 0;
    zbrowser_engine_redraw_child_clip_skips = 0;
    zbrowser_engine_redraw_first_child_skip_type = -1;
    zbrowser_engine_redraw_first_child_skip_x = 0;
    zbrowser_engine_redraw_first_child_skip_y = 0;
    zbrowser_engine_redraw_first_child_skip_w = 0;
    zbrowser_engine_redraw_first_child_skip_h = 0;
    zbrowser_engine_redraw_first_child_skip_r0 = 0;
    zbrowser_engine_redraw_first_child_skip_r1 = 0;
    zbrowser_engine_redraw_first_child_skip_c0 = 0;
    zbrowser_engine_redraw_first_child_skip_c1 = 0;
    plot_ctx.clip = clip;
    redraw.interactive = true;
    redraw.background_images = true;
    redraw.plot = &zbrowser_engine_plotters;
    redraw.priv = &plot_ctx;

    memset(&data, 0, sizeof(data));
    data.x = (int)x;
    data.y = y_offset;
    data.width = zbrowser_engine_content->width > 0
        ? zbrowser_engine_content->width
        : (int)viewport_width;
    data.height = zbrowser_engine_content->height > 0
        ? zbrowser_engine_content->height
        : (int)viewport_height;
    data.background_colour = 0xffffffu;
    data.scale = 1.0f;

    gfx_fill_rect(x,
                  y,
                  viewport_width,
                  viewport_height,
                  0xffffffu);
    if (!zbrowser_engine_content->handler->redraw(zbrowser_engine_content,
                                                  &data,
                                                  &clip,
                                                  &redraw)) {
        return -1;
    }
    zbrowser_engine_html_resource_stats(&css_total,
                                        &css_loaded,
                                        &obj_total,
                                        &obj_loaded);
    if (plot_ctx.visible_texts == 0u && plot_ctx.visible_bitmaps == 0u) {
        zbrowser_engine_box_stats_t box_stats;
        zbrowser_engine_html_box_stats(&box_stats);
        zbrowser_engine_viewport_box_stats((int)x, y_offset, &clip, &box_stats);
        snprintf((char *)zbrowser_engine_status_detail,
                 sizeof(zbrowser_engine_status_detail),
                 "NS blank box%u grid%u gl %d,%d %dx%d c%u cy%d..%d g0 %d,%d %dx%d c%u cy%d..%d txtbox%u viewtxt%u/%u vis%u/%u hid%u zero%u rect%u line%u path%u/%u poly%u/%u disc%u/%u arc%u/%u bmp%u/%u ext%dx%d wh%dx%d css%u/%u obj%u/%u http%u/%u fail%u tr%u hit%u ce%u/%u/%u img%u fb%u ie%u br%u/%u/%u st%u jse%u/%u frm%u/%u/%u/%u",
                 box_stats.boxes,
                 box_stats.grid_boxes,
                 box_stats.largest_grid_x,
                 box_stats.largest_grid_y,
                 box_stats.largest_grid_width,
                 box_stats.largest_grid_height,
                 box_stats.largest_grid_child_count,
                 box_stats.largest_grid_child_min_y,
                 box_stats.largest_grid_child_max_bottom,
                 box_stats.first_grid_x,
                 box_stats.first_grid_y,
                 box_stats.first_grid_width,
                 box_stats.first_grid_height,
                 box_stats.first_grid_child_count,
                 box_stats.first_grid_child_min_y,
                 box_stats.first_grid_child_max_bottom,
                 box_stats.text_boxes,
                 box_stats.viewport_text_boxes,
                 box_stats.viewport_text_bytes,
                 box_stats.viewport_visible_text_boxes,
                 box_stats.viewport_visible_text_bytes,
                 box_stats.viewport_hidden_text_boxes,
                 box_stats.viewport_zero_text_boxes,
                 plot_ctx.rectangles,
                 plot_ctx.lines,
                 plot_ctx.visible_paths,
                 plot_ctx.paths,
                 plot_ctx.visible_polygons,
                 plot_ctx.polygons,
                 plot_ctx.visible_discs,
                 plot_ctx.discs,
                 plot_ctx.visible_arcs,
                 plot_ctx.arcs,
                 plot_ctx.visible_bitmaps,
                 plot_ctx.bitmaps,
                 box_stats.max_right,
                 box_stats.max_bottom,
                 zbrowser_engine_content->width,
                 zbrowser_engine_content->height,
                 css_loaded,
                 css_total,
                 obj_loaded,
                 obj_total,
                 zbrowser_lainos_net_http_successes(),
                 zbrowser_lainos_net_http_fetches(),
                 zbrowser_lainos_net_http_errors(),
                 zbrowser_lainos_net_content_type_rejects(),
                 zbrowser_lainos_net_cache_hits(),
                 zbrowser_lainos_net_css_compat_transforms(),
                 zbrowser_lainos_net_css_compat_vars(),
                 zbrowser_lainos_net_css_compat_groups(),
                 zbrowser_lainos_net_image_decodes(),
                 zbrowser_lainos_net_image_fallbacks(),
                 zbrowser_lainos_net_image_errors(),
                 zbrowser_lainos_net_bitmap_render_successes(),
                 zbrowser_lainos_net_bitmap_renders(),
                 zbrowser_lainos_net_bitmap_render_errors(),
                 zbrowser_lainos_net_http_last_status(),
                 zbrowser_lainos_js_exec_successes(),
                 zbrowser_lainos_js_execs(),
                 zbrowser_engine_form_key_count,
                 zbrowser_engine_form_mouse_count,
                 zbrowser_lainos_navigation_creates(),
                 zbrowser_lainos_navigation_consumes());
        zbrowser_engine_status_detail[sizeof(zbrowser_engine_status_detail) - 1u] = 0;
        zbrowser_engine_status_text = zbrowser_engine_status_detail;
    } else {
        zbrowser_engine_box_stats_t box_stats;
        zbrowser_engine_html_box_stats(&box_stats);
        zbrowser_engine_viewport_box_stats((int)x, y_offset, &clip, &box_stats);
        snprintf((char *)zbrowser_engine_status_detail,
                 sizeof(zbrowser_engine_status_detail),
                 "NS rendered box%u grid%u gl %d,%d %dx%d c%u cy%d..%d g0 %d,%d %dx%d c%u cy%d..%d txt%u/%u txtbox%u viewtxt%u/%u vis%u/%u hid%u zero%u ty%d..%d rect%u line%u path%u/%u poly%u/%u disc%u/%u arc%u/%u bmp%u/%u ext%dx%d wh%dx%d css%u/%u obj%u/%u http%u/%u fail%u tr%u hit%u ce%u/%u/%u img%u fb%u ie%u br%u/%u/%u st%u jse%u/%u frm%u/%u/%u/%u",
                 box_stats.boxes,
                 box_stats.grid_boxes,
                 box_stats.largest_grid_x,
                 box_stats.largest_grid_y,
                 box_stats.largest_grid_width,
                 box_stats.largest_grid_height,
                 box_stats.largest_grid_child_count,
                 box_stats.largest_grid_child_min_y,
                 box_stats.largest_grid_child_max_bottom,
                 box_stats.first_grid_x,
                 box_stats.first_grid_y,
                 box_stats.first_grid_width,
                 box_stats.first_grid_height,
                 box_stats.first_grid_child_count,
                 box_stats.first_grid_child_min_y,
                 box_stats.first_grid_child_max_bottom,
                 plot_ctx.visible_texts,
                 plot_ctx.texts,
                 box_stats.text_boxes,
                 box_stats.viewport_text_boxes,
                 box_stats.viewport_text_bytes,
                 box_stats.viewport_visible_text_boxes,
                 box_stats.viewport_visible_text_bytes,
                 box_stats.viewport_hidden_text_boxes,
                 box_stats.viewport_zero_text_boxes,
                 box_stats.text_min_y,
                 box_stats.text_max_y,
                 plot_ctx.rectangles,
                 plot_ctx.lines,
                 plot_ctx.visible_paths,
                 plot_ctx.paths,
                 plot_ctx.visible_polygons,
                 plot_ctx.polygons,
                 plot_ctx.visible_discs,
                 plot_ctx.discs,
                 plot_ctx.visible_arcs,
                 plot_ctx.arcs,
                 plot_ctx.visible_bitmaps,
                 plot_ctx.bitmaps,
                 box_stats.max_right,
                 box_stats.max_bottom,
                 zbrowser_engine_content->width,
                 zbrowser_engine_content->height,
                 css_loaded,
                 css_total,
                 obj_loaded,
                 obj_total,
                 zbrowser_lainos_net_http_successes(),
                 zbrowser_lainos_net_http_fetches(),
                 zbrowser_lainos_net_http_errors(),
                 zbrowser_lainos_net_content_type_rejects(),
                 zbrowser_lainos_net_cache_hits(),
                 zbrowser_lainos_net_css_compat_transforms(),
                 zbrowser_lainos_net_css_compat_vars(),
                 zbrowser_lainos_net_css_compat_groups(),
                 zbrowser_lainos_net_image_decodes(),
                 zbrowser_lainos_net_image_fallbacks(),
                 zbrowser_lainos_net_image_errors(),
                 zbrowser_lainos_net_bitmap_render_successes(),
                 zbrowser_lainos_net_bitmap_renders(),
                 zbrowser_lainos_net_bitmap_render_errors(),
                 zbrowser_lainos_net_http_last_status(),
                 zbrowser_lainos_js_exec_successes(),
                 zbrowser_lainos_js_execs(),
                 zbrowser_engine_form_key_count,
                 zbrowser_engine_form_mouse_count,
                 zbrowser_lainos_navigation_creates(),
                 zbrowser_lainos_navigation_consumes());
        zbrowser_engine_status_detail[sizeof(zbrowser_engine_status_detail) - 1u] = 0;
        zbrowser_engine_status_text = zbrowser_engine_status_detail;
    }
    zbrowser_engine_content_needs_redraw = 0;
    zbrowser_engine_content_rendered_once = 1;
    return 0;
}

static int zbrowser_engine_redraw_content_pipeline(uint32_t scroll_line,
        uint32_t viewport_width,
        uint32_t viewport_height) {
    if (viewport_height <= 58u) {
        return -1;
    }
    return zbrowser_engine_redraw_content_view(0u,
                                              58u,
                                              viewport_width,
                                              viewport_height - 58u,
                                              scroll_line * 16u);
}

int zbrowser_engine_poll_c(void) {
    unsigned int pending;
    unsigned int ran;
    unsigned int pending_after;
    int needs_document_redraw;

    if (zbrowser_engine_content == 0) {
        return 0;
    }
    pending = zbrowser_lainos_schedule_count() +
              zbrowser_lainos_fetch_pending_count() +
              zbrowser_engine_content->active +
              (zbrowser_engine_content_prepare_pending != 0 ? 1u : 0u) +
              (zbrowser_engine_content_needs_reformat != 0 ? 1u : 0u) +
              (zbrowser_engine_content_needs_redraw != 0 ? 1u : 0u) +
              (zbrowser_engine_content_needs_chrome != 0 ? 1u : 0u);
    if (pending == 0u) {
        return 0;
    }
    ran = zbrowser_lainos_run_scheduled_budget(ZBROWSER_ENGINE_POLL_CALLBACK_BUDGET);
    ran += zbrowser_lainos_pump_fetchers();
    zbrowser_engine_kick_html_conversion(zbrowser_engine_content);
    if (zbrowser_engine_content_prepare_pending != 0 &&
        (zbrowser_engine_content->status == CONTENT_STATUS_READY ||
         zbrowser_engine_content->status == CONTENT_STATUS_DONE)) {
        zbrowser_engine_content_prepare_pending = 0;
        zbrowser_engine_content_needs_reformat = 1;
        zbrowser_engine_content_needs_redraw = 1;
    }
    if (zbrowser_engine_content_needs_reformat != 0 &&
        (zbrowser_engine_content->status == CONTENT_STATUS_READY ||
         zbrowser_engine_content->status == CONTENT_STATUS_DONE)) {
        zbrowser_engine_content_needs_reformat = 0;
        content__reformat(zbrowser_engine_content,
                          false,
                          (int)zbrowser_engine_content_viewport_width,
                          (int)zbrowser_engine_content_viewport_height);
        zbrowser_engine_content_needs_redraw = 1;
    }
    pending_after = zbrowser_lainos_schedule_count() +
                    zbrowser_lainos_fetch_pending_count() +
                    zbrowser_engine_content->active +
                    (zbrowser_engine_content_prepare_pending != 0 ? 1u : 0u) +
                    (zbrowser_engine_content_needs_reformat != 0 ? 1u : 0u) +
                    (zbrowser_engine_content_needs_chrome != 0 ? 1u : 0u);
    if (pending_after == 0u &&
        zbrowser_engine_content_rendered_once != 0 &&
        zbrowser_engine_content_needs_redraw != 0 &&
        (zbrowser_engine_content->status == CONTENT_STATUS_READY ||
         zbrowser_engine_content->status == CONTENT_STATUS_DONE)) {
        zbrowser_engine_content_needs_redraw = 0;
    }
    needs_document_redraw =
        zbrowser_engine_content_needs_reformat != 0 ||
        zbrowser_engine_content_needs_redraw != 0 ||
        (zbrowser_engine_content_prepare_pending != 0 &&
         (zbrowser_engine_content->status == CONTENT_STATUS_READY ||
          zbrowser_engine_content->status == CONTENT_STATUS_DONE));
    if (zbrowser_engine_content->status == CONTENT_STATUS_READY ||
        zbrowser_engine_content->status == CONTENT_STATUS_DONE) {
        zbrowser_engine_status_text = zbrowser_engine_content_detail_status("poll");
    } else {
        zbrowser_engine_status_text = zbrowser_engine_content_detail_status("waiting");
    }
    if (needs_document_redraw != 0) {
        return 2;
    }
    if (zbrowser_engine_content_needs_chrome != 0) {
        zbrowser_engine_content_needs_chrome = 0;
        return 1;
    }
    return ran != 0u || zbrowser_engine_content_prepare_pending != 0 ? 1 : 0;
}
#endif

void zbrowser_engine_invalidate_cache_c(void) {
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
    zbrowser_engine_release_document();
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
    zbrowser_lainos_net_stats_reset();
    zbrowser_engine_form_key_count = 0;
    zbrowser_engine_form_mouse_count = 0;
    zbrowser_engine_content_source_html = 0;
    zbrowser_engine_content_source_size = 0;
    zbrowser_engine_content_source_url[0] = 0;
#endif
#endif
}

static int zbrowser_engine_prepare_html_layout(const uint8_t *url,
        uint32_t viewport_width, uint32_t viewport_height) {
    dom_element *root = 0;
    lwc_error lwc_error_value;
    nserror ns_error;

    zbrowser_engine_status_text = zbrowser_engine_status_html_layout_failed;
    if (zbrowser_engine_document == 0) {
        return -1;
    }
    if (dom_document_get_document_element(zbrowser_engine_document, &root) != DOM_NO_ERR ||
        root == 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_html_layout_no_root;
        return -1;
    }
    if (zbrowser_engine_create_select_ctx(&zbrowser_engine_html_select_ctx,
            zbrowser_engine_sheets, zbrowser_engine_sheet_count) != 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_html_layout_select_ctx_failed;
        dom_node_unref(root);
        return -1;
    }
    if (zbrowser_engine_select_root_style_with_ctx(zbrowser_engine_document,
            zbrowser_engine_html_select_ctx, viewport_width, viewport_height) != 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_html_layout_root_style_failed;
        dom_node_unref(root);
        return -1;
    }
    ns_error = nsurl_create(zbrowser_engine_url_for_nsurl(url),
                            &zbrowser_engine_base_url);
    if (ns_error != NSERROR_OK || zbrowser_engine_base_url == 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_html_layout_base_url_failed;
        dom_node_unref(root);
        return -1;
    }

    memset(&zbrowser_engine_html, 0, sizeof(zbrowser_engine_html));
    zbrowser_engine_html.document = zbrowser_engine_document;
    zbrowser_engine_html.quirks = DOM_DOCUMENT_QUIRKS_MODE_NONE;
    zbrowser_engine_html.encoding = "UTF-8";
    zbrowser_engine_html.base_url = zbrowser_engine_base_url;
    zbrowser_engine_html.background_colour = zbrowser_engine_root_background & 0x00ffffffu;
    zbrowser_engine_html.font_func = &zbrowser_lainos_layout_table;
    zbrowser_engine_html.select_ctx = zbrowser_engine_html_select_ctx;
    zbrowser_engine_html.enable_scripting = false;
    zbrowser_engine_html.base.width = (int)viewport_width;
    zbrowser_engine_html.base.height = (int)viewport_height;
    zbrowser_engine_html.base.available_width = (int)viewport_width;
    zbrowser_engine_html.base.available_height = (int)viewport_height;
    zbrowser_engine_html.box_conversion_context = 0;
    zbrowser_engine_setup_media(&zbrowser_engine_html, viewport_width, viewport_height);

    lwc_error_value = lwc_intern_string("*", 1, &zbrowser_engine_html.universal);
    if (lwc_error_value != lwc_error_ok) {
        zbrowser_engine_status_text = zbrowser_engine_status_html_layout_lwc_failed;
        dom_node_unref(root);
        return -1;
    }
    lwc_error_value = lwc_intern_string("light", 5,
        &zbrowser_engine_html.media.prefers_color_scheme);
    if (lwc_error_value != lwc_error_ok) {
        zbrowser_engine_status_text = zbrowser_engine_status_html_layout_lwc_failed;
        dom_node_unref(root);
        return -1;
    }
    zbrowser_engine_html.forms = html_forms_get_forms(zbrowser_engine_html.encoding,
        (dom_html_document *)zbrowser_engine_html.document);

    zbrowser_engine_html_ready = 0;
    ns_error = dom_to_box((dom_node *)root,
                          &zbrowser_engine_html,
                          zbrowser_engine_dom_to_box_done,
                          &zbrowser_engine_html.box_conversion_context);
    dom_node_unref(root);
    if (ns_error != NSERROR_OK ||
        zbrowser_engine_html_ready <= 0 ||
        zbrowser_engine_html.layout == 0) {
        zbrowser_engine_status_text = zbrowser_engine_detail_status(
            "NetSurf HTML layout failed: DOM to box conversion",
            "; DOM painter fallback",
            0,
            0,
            0);
        return -1;
    }
    if (!layout_document(&zbrowser_engine_html, (int)viewport_width, (int)viewport_height)) {
        zbrowser_engine_html_ready = 0;
        zbrowser_engine_status_text = zbrowser_engine_status_html_layout_document_failed;
        return -1;
    }
    zbrowser_engine_html.base.width = (int)viewport_width;
    zbrowser_engine_html.base.height = zbrowser_engine_html.layout->height;
    return 0;
}

static uint32_t zbrowser_engine_ns_colour(colour c) {
    return ((c & 0x0000ffu) << 16) |
           (c & 0x00ff00u) |
           ((c & 0xff0000u) >> 16);
}

static int zbrowser_engine_style_width(const plot_style_t *style) {
    int width;

    if (style == 0 || style->stroke_width <= 0) {
        return 1;
    }
    width = plot_style_fixed_to_int(style->stroke_width);
    if (width < 1) {
        width = 1;
    } else if (width > 16) {
        width = 16;
    }
    return width;
}

static int zbrowser_engine_clip_rect(const struct redraw_context *ctx,
        int *x0, int *y0, int *x1, int *y1) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;
    struct rect clip;

    if (x0 == 0 || y0 == 0 || x1 == 0 || y1 == 0 || *x1 <= *x0 || *y1 <= *y0) {
        return 0;
    }
    if (plot != 0) {
        clip = plot->clip;
        if (*x0 < clip.x0) {
            *x0 = clip.x0;
        }
        if (*y0 < clip.y0) {
            *y0 = clip.y0;
        }
        if (*x1 > clip.x1) {
            *x1 = clip.x1;
        }
        if (*y1 > clip.y1) {
            *y1 = clip.y1;
        }
    }
    if (*x0 < 0) {
        *x0 = 0;
    }
    if (*y0 < 0) {
        *y0 = 0;
    }
    return *x1 > *x0 && *y1 > *y0;
}

static void zbrowser_engine_fill_rect_clipped(const struct redraw_context *ctx,
        int x0, int y0, int x1, int y1, uint32_t colour) {
    if (zbrowser_engine_clip_rect(ctx, &x0, &y0, &x1, &y1)) {
        gfx_fill_rect((uint32_t)x0,
                      (uint32_t)y0,
                      (uint32_t)(x1 - x0),
                      (uint32_t)(y1 - y0),
                      colour);
    }
}

static int zbrowser_engine_rect_visible(const struct redraw_context *ctx,
        int x0, int y0, int x1, int y1) {
    return zbrowser_engine_clip_rect(ctx, &x0, &y0, &x1, &y1);
}

static int zbrowser_engine_positive_mod(int value, int divisor) {
    int result;

    if (divisor <= 0) {
        return 0;
    }
    result = value % divisor;
    return result < 0 ? result + divisor : result;
}

static uint32_t zbrowser_engine_blend_rgb(uint32_t dst, uint32_t src, uint32_t alpha) {
    uint32_t inv = 255u - alpha;
    uint32_t r = (((dst >> 16) & 0xffu) * inv + ((src >> 16) & 0xffu) * alpha + 127u) / 255u;
    uint32_t g = (((dst >> 8) & 0xffu) * inv + ((src >> 8) & 0xffu) * alpha + 127u) / 255u;
    uint32_t b = ((dst & 0xffu) * inv + (src & 0xffu) * alpha + 127u) / 255u;

    return (r << 16) | (g << 8) | b;
}

static uint32_t zbrowser_engine_lerp_u8(uint32_t a, uint32_t b, uint32_t weight) {
    return (a * (256u - weight) + b * weight + 128u) >> 8;
}

static void zbrowser_engine_sample_bitmap_rgba(const uint8_t *pixels,
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

    r0 = zbrowser_engine_lerp_u8((uint32_t)p00[0], (uint32_t)p10[0], wx);
    r1 = zbrowser_engine_lerp_u8((uint32_t)p01[0], (uint32_t)p11[0], wx);
    g0 = zbrowser_engine_lerp_u8((uint32_t)p00[1], (uint32_t)p10[1], wx);
    g1 = zbrowser_engine_lerp_u8((uint32_t)p01[1], (uint32_t)p11[1], wx);
    b0 = zbrowser_engine_lerp_u8((uint32_t)p00[2], (uint32_t)p10[2], wx);
    b1 = zbrowser_engine_lerp_u8((uint32_t)p01[2], (uint32_t)p11[2], wx);
    a0 = zbrowser_engine_lerp_u8((uint32_t)p00[3], (uint32_t)p10[3], wx);
    a1 = zbrowser_engine_lerp_u8((uint32_t)p01[3], (uint32_t)p11[3], wx);

    *out_r = zbrowser_engine_lerp_u8(r0, r1, wy);
    *out_g = zbrowser_engine_lerp_u8(g0, g1, wy);
    *out_b = zbrowser_engine_lerp_u8(b0, b1, wy);
    *out_a = zbrowser_engine_lerp_u8(a0, a1, wy);
}

static void zbrowser_engine_draw_rect_clipped(const struct redraw_context *ctx,
        int x0, int y0, int x1, int y1, int width, uint32_t colour) {
    for (int i = 0; i < width; ++i) {
        zbrowser_engine_fill_rect_clipped(ctx, x0, y0 + i, x1, y0 + i + 1, colour);
        zbrowser_engine_fill_rect_clipped(ctx, x0, y1 - 1 - i, x1, y1 - i, colour);
        zbrowser_engine_fill_rect_clipped(ctx, x0 + i, y0, x0 + i + 1, y1, colour);
        zbrowser_engine_fill_rect_clipped(ctx, x1 - 1 - i, y0, x1 - i, y1, colour);
    }
}

static void zbrowser_engine_draw_diagonal_line_clipped(const struct redraw_context *ctx,
        int x0, int y0, int x1, int y1, int width, uint32_t colour);

static void zbrowser_engine_draw_line_clipped(const struct redraw_context *ctx,
        int x0, int y0, int x1, int y1, int width, uint32_t colour) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;
    int min_x = x0 < x1 ? x0 : x1;
    int max_x = x0 > x1 ? x0 : x1;
    int min_y = y0 < y1 ? y0 : y1;
    int max_y = y0 > y1 ? y0 : y1;

    if (width < 1) {
        width = 1;
    }
    if (plot != 0 &&
        (max_x + width / 2 < plot->clip.x0 ||
         max_y + width / 2 < plot->clip.y0 ||
         min_x - width / 2 >= plot->clip.x1 ||
         min_y - width / 2 >= plot->clip.y1)) {
        return;
    }
    if (y0 == y1) {
        zbrowser_engine_fill_rect_clipped(ctx,
                                          min_x,
                                          y0 - width / 2,
                                          max_x + 1,
                                          y0 - width / 2 + width,
                                          colour);
        return;
    }
    if (x0 == x1) {
        zbrowser_engine_fill_rect_clipped(ctx,
                                          x0 - width / 2,
                                          min_y,
                                          x0 - width / 2 + width,
                                          max_y + 1,
                                          colour);
        return;
    }
    zbrowser_engine_draw_diagonal_line_clipped(ctx, x0, y0, x1, y1, width, colour);
}

static void zbrowser_engine_plot_pixel(const struct redraw_context *ctx,
        int x,
        int y,
        uint32_t colour) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;

    if (x < 0 || y < 0 || x >= (int)gfx_width() || y >= (int)gfx_height()) {
        return;
    }
    if (plot != 0 &&
        (x < plot->clip.x0 || y < plot->clip.y0 ||
         x >= plot->clip.x1 || y >= plot->clip.y1)) {
        return;
    }
    put_pixel((uint32_t)x, (uint32_t)y, colour);
}

static void zbrowser_engine_plot_line_brush(const struct redraw_context *ctx,
        int x,
        int y,
        int width,
        uint32_t colour) {
    if (width <= 1) {
        zbrowser_engine_plot_pixel(ctx, x, y, colour);
        return;
    }
    zbrowser_engine_fill_rect_clipped(ctx,
                                      x - width / 2,
                                      y - width / 2,
                                      x - width / 2 + width,
                                      y - width / 2 + width,
                                      colour);
}

static void zbrowser_engine_draw_line_clipped(const struct redraw_context *ctx,
        int x0,
        int y0,
        int x1,
        int y1,
        int width,
        uint32_t colour);

static int zbrowser_engine_clamped_disc_radius(int radius) {
    if (radius < 1) {
        return 0;
    }
    if (radius > 512) {
        return 512;
    }
    return radius;
}

static void zbrowser_engine_fill_disc_clipped(const struct redraw_context *ctx,
        int x,
        int y,
        int radius,
        uint32_t colour) {
    int r = zbrowser_engine_clamped_disc_radius(radius);
    int r2 = r * r;

    if (r == 0) {
        return;
    }
    for (int dy = -r; dy <= r; ++dy) {
        int span = 0;
        int dy2 = dy * dy;
        while ((span + 1) * (span + 1) + dy2 <= r2) {
            ++span;
        }
        zbrowser_engine_fill_rect_clipped(ctx,
                                          x - span,
                                          y + dy,
                                          x + span + 1,
                                          y + dy + 1,
                                          colour);
    }
}

static void zbrowser_engine_draw_disc_outline_clipped(const struct redraw_context *ctx,
        int x,
        int y,
        int radius,
        int width,
        uint32_t colour) {
    int r = zbrowser_engine_clamped_disc_radius(radius);
    int inner;
    int r2;
    int inner2;

    if (r == 0) {
        return;
    }
    if (width < 1) {
        width = 1;
    }
    if (width > r) {
        width = r;
    }
    inner = r - width;
    r2 = r * r;
    inner2 = inner * inner;
    for (int dy = -r; dy <= r; ++dy) {
        int dy2 = dy * dy;
        for (int dx = -r; dx <= r; ++dx) {
            int d2 = dx * dx + dy2;
            if (d2 <= r2 && d2 >= inner2) {
                zbrowser_engine_plot_pixel(ctx, x + dx, y + dy, colour);
            }
        }
    }
}

static int zbrowser_engine_normalize_degrees(int degrees) {
    int result = degrees % 360;
    return result < 0 ? result + 360 : result;
}

static int zbrowser_engine_sin_deg_1024(int degrees) {
    int d = zbrowser_engine_normalize_degrees(degrees);
    int sign = 1;
    int numerator;
    int denominator;

    if (d > 180) {
        d -= 180;
        sign = -1;
    }
    if (d > 90) {
        d = 180 - d;
    }
    numerator = 4 * d * (180 - d);
    denominator = 40500 - d * (180 - d);
    if (denominator == 0) {
        return 0;
    }
    return sign * (numerator * 1024 + denominator / 2) / denominator;
}

static int zbrowser_engine_cos_deg_1024(int degrees) {
    return zbrowser_engine_sin_deg_1024(degrees + 90);
}

static void zbrowser_engine_draw_arc_clipped(const struct redraw_context *ctx,
        int x,
        int y,
        int radius,
        int angle1,
        int angle2,
        int width,
        uint32_t colour) {
    int r = zbrowser_engine_clamped_disc_radius(radius);
    int start = zbrowser_engine_normalize_degrees(angle1);
    int end = zbrowser_engine_normalize_degrees(angle2);
    int span = end - start;
    int last_x = 0;
    int last_y = 0;
    bool have_last = false;

    if (r == 0) {
        return;
    }
    if (span <= 0) {
        span += 360;
    }
    if (width < 1) {
        width = 1;
    }
    for (int step = 0; step <= span; ++step) {
        int degrees = start + step + 90;
        int px = x + (zbrowser_engine_cos_deg_1024(degrees) * r + 512) / 1024;
        int py = y + (zbrowser_engine_sin_deg_1024(degrees) * r + 512) / 1024;
        if (have_last) {
            zbrowser_engine_draw_line_clipped(ctx, last_x, last_y, px, py, width, colour);
        } else {
            zbrowser_engine_plot_line_brush(ctx, px, py, width, colour);
        }
        last_x = px;
        last_y = py;
        have_last = true;
    }
}

static void zbrowser_engine_draw_diagonal_line_clipped(const struct redraw_context *ctx,
        int x0,
        int y0,
        int x1,
        int y1,
        int width,
        uint32_t colour) {
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        int e2;

        zbrowser_engine_plot_line_brush(ctx, x0, y0, width, colour);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        e2 = err * 2;
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

static void zbrowser_engine_fill_polygon_points(const struct redraw_context *ctx,
        const int *p,
        unsigned int n,
        uint32_t colour) {
    int min_y;
    int max_y;
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;

    if (p == 0 || n < 3u) {
        return;
    }
    min_y = p[1];
    max_y = p[1];
    for (unsigned int i = 1u; i < n; ++i) {
        int y = p[i * 2u + 1u];
        if (y < min_y) {
            min_y = y;
        }
        if (y > max_y) {
            max_y = y;
        }
    }
    if (plot != 0) {
        if (min_y < plot->clip.y0) {
            min_y = plot->clip.y0;
        }
        if (max_y >= plot->clip.y1) {
            max_y = plot->clip.y1 - 1;
        }
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_y >= (int)gfx_height()) {
        max_y = (int)gfx_height() - 1;
    }

    for (int y = min_y; y <= max_y; ++y) {
        int intersections[64];
        unsigned int count = 0u;

        for (unsigned int i = 0u; i < n; ++i) {
            unsigned int j = (i + 1u) % n;
            int x0 = p[i * 2u];
            int y0 = p[i * 2u + 1u];
            int x1 = p[j * 2u];
            int y1 = p[j * 2u + 1u];

            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                if (count < sizeof(intersections) / sizeof(intersections[0])) {
                    intersections[count++] = x0 + (int)(((int64_t)(y - y0) * (int64_t)(x1 - x0)) /
                                                        (int64_t)(y1 - y0));
                }
            }
        }
        for (unsigned int i = 1u; i < count; ++i) {
            int value = intersections[i];
            unsigned int j = i;
            while (j > 0u && intersections[j - 1u] > value) {
                intersections[j] = intersections[j - 1u];
                --j;
            }
            intersections[j] = value;
        }
        for (unsigned int i = 0u; i + 1u < count; i += 2u) {
            int x0 = intersections[i];
            int x1 = intersections[i + 1u];
            if (x1 > x0) {
                zbrowser_engine_fill_rect_clipped(ctx, x0, y, x1 + 1, y + 1, colour);
            }
        }
    }
}

static nserror zbrowser_engine_plot_clip(const struct redraw_context *ctx,
        const struct rect *clip) {
    zbrowser_engine_plot_ctx_t *plot = (zbrowser_engine_plot_ctx_t *)ctx->priv;
    if (plot != 0 && clip != 0) {
        plot->clip = *clip;
    }
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_rectangle(const struct redraw_context *ctx,
        const plot_style_t *style, const struct rect *rectangle) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;

    if (plot != 0) {
        ++plot->rectangles;
    }
    if (rectangle == 0 || style == 0) {
        return NSERROR_OK;
    }
    if (style->fill_type != PLOT_OP_TYPE_NONE &&
        rectangle->x1 > rectangle->x0 && rectangle->y1 > rectangle->y0) {
        zbrowser_engine_fill_rect_clipped(ctx,
                                          rectangle->x0,
                                          rectangle->y0,
                                          rectangle->x1,
                                          rectangle->y1,
                                          zbrowser_engine_ns_colour(style->fill_colour));
    }
    if (style->stroke_type != PLOT_OP_TYPE_NONE &&
        rectangle->x1 > rectangle->x0 && rectangle->y1 > rectangle->y0) {
        zbrowser_engine_draw_rect_clipped(ctx,
                                          rectangle->x0,
                                          rectangle->y0,
                                          rectangle->x1,
                                          rectangle->y1,
                                          zbrowser_engine_style_width(style),
                                          zbrowser_engine_ns_colour(style->stroke_colour));
    }
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_line(const struct redraw_context *ctx,
        const plot_style_t *style, const struct rect *line) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;

    if (plot != 0) {
        ++plot->lines;
    }
    if (style != 0 && line != 0) {
        zbrowser_engine_draw_line_clipped(ctx,
                                          line->x0,
                                          line->y0,
                                          line->x1,
                                          line->y1,
                                          zbrowser_engine_style_width(style),
                                          zbrowser_engine_ns_colour(style->stroke_colour));
    }
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_disc(const struct redraw_context *ctx,
        const plot_style_t *style, int x, int y, int radius) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;
    if (plot != 0) {
        ++plot->discs;
    }
    if (style == 0 || radius <= 0) {
        return NSERROR_OK;
    }
    if (plot != 0 &&
        zbrowser_engine_rect_visible(ctx, x - radius, y - radius, x + radius, y + radius) != 0) {
        ++plot->visible_discs;
    }
    if (style->fill_type != PLOT_OP_TYPE_NONE &&
        style->fill_colour != NS_TRANSPARENT) {
        zbrowser_engine_fill_disc_clipped(ctx,
                                          x,
                                          y,
                                          radius,
                                          zbrowser_engine_ns_colour(style->fill_colour));
    }
    if (style->stroke_type != PLOT_OP_TYPE_NONE &&
        style->stroke_colour != NS_TRANSPARENT) {
        zbrowser_engine_draw_disc_outline_clipped(ctx,
                                                  x,
                                                  y,
                                                  radius,
                                                  zbrowser_engine_style_width(style),
                                                  zbrowser_engine_ns_colour(style->stroke_colour));
    }
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_noop(const struct redraw_context *ctx) {
    (void)ctx;
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_path(const struct redraw_context *ctx,
        const plot_style_t *style, const float *p, unsigned int n,
        const float transform[6]) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;
    int points[128 * 2u];
    unsigned int point_count = 0u;
    unsigned int moves = 0u;
    unsigned int i = 0u;
    bool valid = true;
    float current_x = 0.0f;
    float current_y = 0.0f;

    if (plot != 0) {
        ++plot->paths;
    }
    if (style == 0 || p == 0 || n == 0u || p[0] != PLOTTER_PATH_MOVE) {
        return NSERROR_OK;
    }

    while (i < n && valid) {
        if (p[i] == PLOTTER_PATH_MOVE) {
            float tx;
            float ty;
            int x;
            int y;
            if (i + 2u >= n) {
                valid = false;
                break;
            }
            tx = p[i + 1u];
            ty = p[i + 2u];
            if (transform != 0) {
                float ox = tx;
                float oy = ty;
                tx = ox * transform[0] + oy * transform[2] + transform[4];
                ty = ox * transform[1] + oy * transform[3] + transform[5];
            }
            x = (int)(tx >= 0.0f ? tx + 0.5f : tx - 0.5f);
            y = (int)(ty >= 0.0f ? ty + 0.5f : ty - 0.5f);
            current_x = p[i + 1u];
            current_y = p[i + 2u];
            ++moves;
            if (moves == 1u && point_count < 128u) {
                points[point_count * 2u] = x;
                points[point_count * 2u + 1u] = y;
                ++point_count;
            }
            i += 3u;
        } else if (p[i] == PLOTTER_PATH_CLOSE) {
            i += 1u;
        } else if (p[i] == PLOTTER_PATH_LINE) {
            float tx;
            float ty;
            int x;
            int y;
            if (i + 2u >= n) {
                valid = false;
                break;
            }
            tx = p[i + 1u];
            ty = p[i + 2u];
            if (transform != 0) {
                float ox = tx;
                float oy = ty;
                tx = ox * transform[0] + oy * transform[2] + transform[4];
                ty = ox * transform[1] + oy * transform[3] + transform[5];
            }
            x = (int)(tx >= 0.0f ? tx + 0.5f : tx - 0.5f);
            y = (int)(ty >= 0.0f ? ty + 0.5f : ty - 0.5f);
            current_x = p[i + 1u];
            current_y = p[i + 2u];
            if (moves == 1u && point_count < 128u) {
                points[point_count * 2u] = x;
                points[point_count * 2u + 1u] = y;
                ++point_count;
            }
            i += 3u;
        } else if (p[i] == PLOTTER_PATH_BEZIER) {
            float x0 = current_x;
            float y0 = current_y;
            if (i + 6u >= n) {
                valid = false;
                break;
            }
            for (unsigned int step = 1u; step <= 8u; ++step) {
                float t = (float)step / 8.0f;
                float mt = 1.0f - t;
                float bx = mt * mt * mt * x0 +
                    3.0f * mt * mt * t * p[i + 1u] +
                    3.0f * mt * t * t * p[i + 3u] +
                    t * t * t * p[i + 5u];
                float by = mt * mt * mt * y0 +
                    3.0f * mt * mt * t * p[i + 2u] +
                    3.0f * mt * t * t * p[i + 4u] +
                    t * t * t * p[i + 6u];
                if (transform != 0) {
                    float ox = bx;
                    float oy = by;
                    bx = ox * transform[0] + oy * transform[2] + transform[4];
                    by = ox * transform[1] + oy * transform[3] + transform[5];
                }
                if (moves == 1u && point_count < 128u) {
                    points[point_count * 2u] = (int)(bx >= 0.0f ? bx + 0.5f : bx - 0.5f);
                    points[point_count * 2u + 1u] = (int)(by >= 0.0f ? by + 0.5f : by - 0.5f);
                    ++point_count;
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
    if (plot != 0 && point_count != 0u) {
        int min_x = points[0];
        int max_x = points[0];
        int min_y = points[1];
        int max_y = points[1];
        for (unsigned int j = 1u; j < point_count; ++j) {
            int px = points[j * 2u];
            int py = points[j * 2u + 1u];
            if (px < min_x) {
                min_x = px;
            }
            if (px > max_x) {
                max_x = px;
            }
            if (py < min_y) {
                min_y = py;
            }
            if (py > max_y) {
                max_y = py;
            }
        }
        if (zbrowser_engine_rect_visible(ctx, min_x, min_y, max_x + 1, max_y + 1) != 0) {
            ++plot->visible_paths;
        }
    }
    if (style->fill_type != PLOT_OP_TYPE_NONE &&
        style->fill_colour != NS_TRANSPARENT &&
        moves == 1u &&
        point_count >= 3u) {
        zbrowser_engine_fill_polygon_points(ctx,
                                            points,
                                            point_count,
                                            zbrowser_engine_ns_colour(style->fill_colour));
    }
    if (style->stroke_type != PLOT_OP_TYPE_NONE &&
        style->stroke_colour != NS_TRANSPARENT) {
        int last_x = 0;
        int last_y = 0;
        int start_x = 0;
        int start_y = 0;
        float source_x = 0.0f;
        float source_y = 0.0f;
        bool have_point = false;
        uint32_t stroke = zbrowser_engine_ns_colour(style->stroke_colour);
        int width = zbrowser_engine_style_width(style);

        i = 0u;
        while (i < n) {
            float tx;
            float ty;
            int x;
            int y;

            if (p[i] == PLOTTER_PATH_MOVE) {
                tx = p[i + 1u];
                ty = p[i + 2u];
                if (transform != 0) {
                    float ox = tx;
                    float oy = ty;
                    tx = ox * transform[0] + oy * transform[2] + transform[4];
                    ty = ox * transform[1] + oy * transform[3] + transform[5];
                }
                last_x = (int)(tx >= 0.0f ? tx + 0.5f : tx - 0.5f);
                last_y = (int)(ty >= 0.0f ? ty + 0.5f : ty - 0.5f);
                start_x = last_x;
                start_y = last_y;
                source_x = p[i + 1u];
                source_y = p[i + 2u];
                have_point = true;
                i += 3u;
            } else if (p[i] == PLOTTER_PATH_CLOSE) {
                if (have_point) {
                    zbrowser_engine_draw_line_clipped(ctx, last_x, last_y, start_x, start_y, width, stroke);
                }
                i += 1u;
            } else if (p[i] == PLOTTER_PATH_LINE) {
                tx = p[i + 1u];
                ty = p[i + 2u];
                if (transform != 0) {
                    float ox = tx;
                    float oy = ty;
                    tx = ox * transform[0] + oy * transform[2] + transform[4];
                    ty = ox * transform[1] + oy * transform[3] + transform[5];
                }
                x = (int)(tx >= 0.0f ? tx + 0.5f : tx - 0.5f);
                y = (int)(ty >= 0.0f ? ty + 0.5f : ty - 0.5f);
                if (have_point) {
                    zbrowser_engine_draw_line_clipped(ctx, last_x, last_y, x, y, width, stroke);
                }
                last_x = x;
                last_y = y;
                source_x = p[i + 1u];
                source_y = p[i + 2u];
                have_point = true;
                i += 3u;
            } else if (p[i] == PLOTTER_PATH_BEZIER) {
                float x0 = source_x;
                float y0 = source_y;
                for (unsigned int step = 1u; step <= 8u; ++step) {
                    float t = (float)step / 8.0f;
                    float mt = 1.0f - t;
                    float bx = mt * mt * mt * x0 +
                        3.0f * mt * mt * t * p[i + 1u] +
                        3.0f * mt * t * t * p[i + 3u] +
                        t * t * t * p[i + 5u];
                    float by = mt * mt * mt * y0 +
                        3.0f * mt * mt * t * p[i + 2u] +
                        3.0f * mt * t * t * p[i + 4u] +
                        t * t * t * p[i + 6u];
                    if (transform != 0) {
                        float ox = bx;
                        float oy = by;
                        bx = ox * transform[0] + oy * transform[2] + transform[4];
                        by = ox * transform[1] + oy * transform[3] + transform[5];
                    }
                    x = (int)(bx >= 0.0f ? bx + 0.5f : bx - 0.5f);
                    y = (int)(by >= 0.0f ? by + 0.5f : by - 0.5f);
                    if (have_point) {
                        zbrowser_engine_draw_line_clipped(ctx, last_x, last_y, x, y, width, stroke);
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

static nserror zbrowser_engine_plot_polygon(const struct redraw_context *ctx,
        const plot_style_t *style, const int *p, unsigned int n) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;

    if (plot != 0) {
        ++plot->polygons;
    }
    if (style != 0 && p != 0 && n >= 2u) {
        if (plot != 0) {
            int min_x = p[0];
            int max_x = p[0];
            int min_y = p[1];
            int max_y = p[1];
            for (unsigned int i = 1u; i < n; ++i) {
                int px = p[i * 2u];
                int py = p[i * 2u + 1u];
                if (px < min_x) {
                    min_x = px;
                }
                if (px > max_x) {
                    max_x = px;
                }
                if (py < min_y) {
                    min_y = py;
                }
                if (py > max_y) {
                    max_y = py;
                }
            }
            if (zbrowser_engine_rect_visible(ctx, min_x, min_y, max_x + 1, max_y + 1) != 0) {
                ++plot->visible_polygons;
            }
        }
        if (style->fill_type != PLOT_OP_TYPE_NONE &&
            style->fill_colour != NS_TRANSPARENT) {
            zbrowser_engine_fill_polygon_points(ctx,
                                                p,
                                                n,
                                                zbrowser_engine_ns_colour(style->fill_colour));
        }
        if (style->stroke_type != PLOT_OP_TYPE_NONE &&
            style->stroke_colour != NS_TRANSPARENT) {
            uint32_t stroke = zbrowser_engine_ns_colour(style->stroke_colour);
            int width = zbrowser_engine_style_width(style);
            for (unsigned int i = 0u; i < n; ++i) {
                unsigned int next = (i + 1u) % n;
                zbrowser_engine_draw_line_clipped(ctx,
                                                  p[i * 2u],
                                                  p[i * 2u + 1u],
                                                  p[next * 2u],
                                                  p[next * 2u + 1u],
                                                  width,
                                                  stroke);
            }
        }
    }
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_arc(const struct redraw_context *ctx,
        const plot_style_t *style, int x, int y, int radius,
        int angle1, int angle2) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;
    if (plot != 0) {
        ++plot->arcs;
    }
    if (style == 0 || radius <= 0) {
        return NSERROR_OK;
    }
    if (plot != 0 &&
        zbrowser_engine_rect_visible(ctx, x - radius, y - radius, x + radius, y + radius) != 0) {
        ++plot->visible_arcs;
    }
    if (style->fill_type != PLOT_OP_TYPE_NONE &&
        style->fill_colour != NS_TRANSPARENT) {
        zbrowser_engine_draw_arc_clipped(ctx,
                                         x,
                                         y,
                                         radius,
                                         angle1,
                                         angle2,
                                         zbrowser_engine_style_width(style),
                                         zbrowser_engine_ns_colour(style->fill_colour));
    } else if (style->stroke_type != PLOT_OP_TYPE_NONE &&
               style->stroke_colour != NS_TRANSPARENT) {
        zbrowser_engine_draw_arc_clipped(ctx,
                                         x,
                                         y,
                                         radius,
                                         angle1,
                                         angle2,
                                         zbrowser_engine_style_width(style),
                                         zbrowser_engine_ns_colour(style->stroke_colour));
    }
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_bitmap(const struct redraw_context *ctx,
        struct bitmap *bitmap, int x, int y, int width, int height,
        colour bg, bitmap_flags_t flags) {
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;
    int source_width = zbrowser_lainos_bitmap_width(bitmap);
    int source_height = zbrowser_lainos_bitmap_height(bitmap);
    int rowstride = zbrowser_lainos_bitmap_rowstride(bitmap);
    uint8_t *pixels = zbrowser_lainos_bitmap_buffer(bitmap);
    int x0 = x;
    int y0 = y;
    int x1 = x + width;
    int y1 = y + height;
    bool scaled = width != source_width || height != source_height;
    bool opaque = zbrowser_lainos_bitmap_opaque(bitmap) != 0;

    if (plot != 0) {
        ++plot->bitmaps;
    }
    if (pixels == 0 ||
        source_width <= 0 ||
        source_height <= 0 ||
        rowstride < source_width * 4 ||
        width <= 0 ||
        height <= 0) {
        return NSERROR_OK;
    }
    if ((flags & BITMAPF_REPEAT_X) != 0u && ctx != 0 && ctx->priv != 0) {
        zbrowser_engine_plot_ctx_t *plot = (zbrowser_engine_plot_ctx_t *)ctx->priv;
        x0 = plot->clip.x0;
        x1 = plot->clip.x1;
    }
    if ((flags & BITMAPF_REPEAT_Y) != 0u && ctx != 0 && ctx->priv != 0) {
        zbrowser_engine_plot_ctx_t *plot = (zbrowser_engine_plot_ctx_t *)ctx->priv;
        y0 = plot->clip.y0;
        y1 = plot->clip.y1;
    }
    if (!zbrowser_engine_clip_rect(ctx, &x0, &y0, &x1, &y1)) {
        return NSERROR_OK;
    }
    if (plot != 0) {
        ++plot->visible_bitmaps;
    }

    for (int py = y0; py < y1; ++py) {
        int rel_y = py - y;
        int tile_y = (flags & BITMAPF_REPEAT_Y) != 0u
            ? zbrowser_engine_positive_mod(rel_y, height)
            : rel_y;
        int src_y_fp;

        if (tile_y < 0 || tile_y >= height) {
            continue;
        }
        if (scaled && height > 1 && source_height > 1) {
            src_y_fp = (int)(((int64_t)tile_y * (int64_t)(source_height - 1) * 256) /
                             (int64_t)(height - 1));
        } else {
            src_y_fp = (int)(((int64_t)tile_y * (int64_t)source_height * 256) /
                             (int64_t)height);
        }
        for (int px = x0; px < x1; ++px) {
            int rel_x = px - x;
            int tile_x = (flags & BITMAPF_REPEAT_X) != 0u
                ? zbrowser_engine_positive_mod(rel_x, width)
                : rel_x;
            uint32_t colour;
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
                    src_x_fp = (int)(((int64_t)tile_x * (int64_t)(source_width - 1) * 256) /
                                     (int64_t)(width - 1));
                } else {
                    src_x_fp = (int)(((int64_t)tile_x * (int64_t)source_width * 256) /
                                     (int64_t)width);
                }
                zbrowser_engine_sample_bitmap_rgba(pixels,
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
                const uint8_t *src;
                src = pixels +
                    (size_t)tile_y * (size_t)rowstride +
                    (size_t)tile_x * 4u;
                r = (uint32_t)src[0];
                g = (uint32_t)src[1];
                b = (uint32_t)src[2];
                a = (uint32_t)src[3];
            }
            alpha = opaque ? 255u : a;
            colour = (r << 16) | (g << 8) | b;
            if (alpha != 255u) {
                uint32_t dst = bg != NS_TRANSPARENT
                    ? zbrowser_engine_ns_colour(bg)
                    : gfx_get_pixel((uint32_t)px, (uint32_t)py);
                colour = zbrowser_engine_blend_rgb(dst, colour, alpha);
            }
            put_pixel((uint32_t)px, (uint32_t)py, colour);
        }
    }
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_text(const struct redraw_context *ctx,
        const plot_font_style_t *fstyle, int x, int y,
        const char *text, size_t length) {
    char buffer[256];
    uint32_t font_px;
    uint32_t fg;
    uint32_t bg;
    uint32_t copy_len;
    int bold;
    int italic;
    int measured_width = 0;
    int draw_width;
    int top;
    const struct rect *clip = 0;
    zbrowser_engine_plot_ctx_t *plot = ctx != 0 ? (zbrowser_engine_plot_ctx_t *)ctx->priv : 0;

    if (plot != 0) {
        ++plot->texts;
        plot->text_bytes += (unsigned int)length;
    }
    if (text == 0 || fstyle == 0 || length == 0u) {
        return NSERROR_OK;
    }
    if (zbrowser_lainos_layout_table.width(fstyle, text, length, &measured_width) != NSERROR_OK) {
        measured_width = 0;
    }
    if (plot != 0) {
        if (measured_width > 0) {
            plot->text_width += (unsigned int)measured_width;
        }
    }
    copy_len = length >= sizeof(buffer) ? sizeof(buffer) - 1u : (uint32_t)length;
    memcpy(buffer, text, copy_len);
    buffer[copy_len] = 0;
    font_px = (uint32_t)plot_style_fixed_to_int(fstyle->size);
    if (font_px < 8u) {
        font_px = 8u;
    } else if (font_px > 96u) {
        font_px = 96u;
    }
    draw_width = measured_width > 0
        ? measured_width
        : (int)(copy_len * (font_px / 2u + 2u));
    top = y - (int)font_px;
    if (top < 0) {
        top = 0;
    }
    fg = zbrowser_engine_ns_colour(fstyle->foreground);
    bg = fstyle->background == NS_TRANSPARENT
        ? 0xffffffffu
        : zbrowser_engine_ns_colour(fstyle->background);
    bold = fstyle->weight >= 600;
    italic = (fstyle->flags & (FONTF_ITALIC | FONTF_OBLIQUE)) != 0;
    if (plot != 0) {
        clip = &plot->clip;
        if (x >= plot->clip.x1 || top >= plot->clip.y1 ||
            x + draw_width <= plot->clip.x0 || top + (int)font_px <= plot->clip.y0) {
            return NSERROR_OK;
        }
        ++plot->visible_texts;
    }
    if (zbrowser_lainos_plot_text_ttf(fstyle, x, y, text, length, fg, bold, italic, clip) > 0) {
        return NSERROR_OK;
    }
    draw_text_sized_at_pixel((uint32_t)x,
                             (uint32_t)(y > (int)font_px ? y - (int)font_px : y),
                             (const uint8_t *)buffer,
                             fg,
                             bg,
                             font_px / 2u + 2u,
                             font_px,
                             font_px / 2u + 2u,
                             bold,
                             italic);
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_group_start(const struct redraw_context *ctx,
        const char *name) {
    (void)ctx;
    (void)name;
    return NSERROR_OK;
}

static nserror zbrowser_engine_plot_group_end(const struct redraw_context *ctx) {
    (void)ctx;
    return NSERROR_OK;
}

static const struct plotter_table zbrowser_engine_plotters = {
    .clip = zbrowser_engine_plot_clip,
    .arc = zbrowser_engine_plot_arc,
    .disc = zbrowser_engine_plot_disc,
    .line = zbrowser_engine_plot_line,
    .rectangle = zbrowser_engine_plot_rectangle,
    .polygon = zbrowser_engine_plot_polygon,
    .path = zbrowser_engine_plot_path,
    .bitmap = zbrowser_engine_plot_bitmap,
    .text = zbrowser_engine_plot_text,
    .group_start = zbrowser_engine_plot_group_start,
    .group_end = zbrowser_engine_plot_group_end,
    .flush = zbrowser_engine_plot_noop,
    .option_knockout = true
};

static int zbrowser_engine_redraw_html(uint32_t scroll_line,
        uint32_t viewport_width, uint32_t viewport_height) {
    struct content_redraw_data data;
    struct rect clip;
    struct redraw_context redraw;
    zbrowser_engine_plot_ctx_t plot_ctx;
    int y_offset;

    if (zbrowser_engine_html_ready <= 0 || zbrowser_engine_html.layout == 0) {
        return -1;
    }
    if ((uint32_t)zbrowser_engine_html.base.available_width != viewport_width) {
        zbrowser_engine_setup_media(&zbrowser_engine_html, viewport_width, viewport_height);
        if (!layout_document(&zbrowser_engine_html, (int)viewport_width, (int)viewport_height)) {
            return -1;
        }
        zbrowser_engine_html.base.available_width = (int)viewport_width;
    }

    y_offset = 76 - (int)(scroll_line * 16u);
    clip.x0 = 0;
    clip.y0 = 58;
    clip.x1 = (int)viewport_width;
    clip.y1 = (int)viewport_height;
    memset(&plot_ctx, 0, sizeof(plot_ctx));
    plot_ctx.clip = clip;
    redraw.interactive = true;
    redraw.background_images = true;
    redraw.plot = &zbrowser_engine_plotters;
    redraw.priv = &plot_ctx;

    data.x = 0;
    data.y = y_offset;
    data.width = (int)viewport_width;
    data.height = zbrowser_engine_html.base.height > 0
        ? zbrowser_engine_html.base.height
        : (int)viewport_height;
    data.background_colour = zbrowser_engine_root_background & 0x00ffffffu;
    data.scale = 1.0f;
    data.repeat_x = false;
    data.repeat_y = false;

    gfx_fill_rect(0u,
                  58u,
                  viewport_width,
                  viewport_height > 82u ? viewport_height - 82u : 0u,
                  zbrowser_engine_root_background & 0x00ffffffu);
    if (!html_redraw((struct content *)&zbrowser_engine_html, &data, &clip, &redraw)) {
        zbrowser_engine_status_text = (const uint8_t *)"NS html redraw failed; DOM painter fallback";
        return -1;
    }
    snprintf((char *)zbrowser_engine_status_detail,
             sizeof(zbrowser_engine_status_detail),
             "NS html redraw txt%u/%u bmp%u/%u rect%u line%u path%u/%u poly%u/%u disc%u/%u arc%u/%u bytes%u width%u wh%dx%d css%u/%u",
             plot_ctx.visible_texts,
             plot_ctx.texts,
             plot_ctx.visible_bitmaps,
             plot_ctx.bitmaps,
             plot_ctx.rectangles,
             plot_ctx.lines,
             plot_ctx.visible_paths,
             plot_ctx.paths,
             plot_ctx.visible_polygons,
             plot_ctx.polygons,
             plot_ctx.visible_discs,
             plot_ctx.discs,
             plot_ctx.visible_arcs,
             plot_ctx.arcs,
             plot_ctx.text_bytes,
             plot_ctx.text_width,
             zbrowser_engine_html.base.width,
             zbrowser_engine_html.base.height,
             zbrowser_engine_css_blocks,
             zbrowser_engine_sheet_count);
    zbrowser_engine_status_detail[sizeof(zbrowser_engine_status_detail) - 1u] = 0;
    zbrowser_engine_status_text = zbrowser_engine_status_detail;
    return 0;
}

static int zbrowser_engine_prepare_dom(const uint8_t *url, const uint8_t *html, uint32_t size) {
    dom_hubbub_parser_params params;
    dom_hubbub_parser *parser = 0;
    dom_document *document = 0;
    dom_hubbub_error error;

    zbrowser_engine_release_document();
    for (uint32_t i = 0; i < ZBROWSER_ENGINE_MAX_STYLESHEETS; ++i) {
        zbrowser_engine_sheets[i] = 0;
    }

    params.enc = "UTF-8";
    params.fix_enc = false;
    params.enable_script = false;
    params.script = 0;
    params.msg = 0;
    params.ctx = 0;
    params.daf = 0;

    error = dom_hubbub_parser_create(&params, &parser, &document);
    if (error != DOM_HUBBUB_OK || parser == 0 || document == 0) {
        zbrowser_engine_status_text =
            zbrowser_engine_dom_create_status(error, parser, document);
        if (parser != 0) {
            dom_hubbub_parser_destroy(parser);
        }
        if (document != 0) {
            dom_node_unref(document);
        }
        return 0;
    }

    error = dom_hubbub_parser_parse_chunk(parser, html, size);
    if (error != DOM_HUBBUB_OK) {
        dom_hubbub_parser_destroy(parser);
        dom_node_unref(document);
        zbrowser_engine_status_text = zbrowser_engine_status_dom_chunk_failed;
        return 0;
    }
    error = dom_hubbub_parser_completed(parser);
    dom_hubbub_parser_destroy(parser);

    if (error != DOM_HUBBUB_OK) {
        dom_node_unref(document);
        zbrowser_engine_status_text = zbrowser_engine_status_dom_complete_failed;
        return 0;
    }

    zbrowser_engine_document = document;
    zbrowser_engine_copy_root_name(zbrowser_engine_document);
    zbrowser_engine_prepare_ua_sheet();
    (void)zbrowser_engine_collect_css_blocks(html,
                                             size,
                                             zbrowser_engine_sheets,
                                             ZBROWSER_ENGINE_MAX_STYLESHEETS,
                                             &zbrowser_engine_sheet_count);
    if (zbrowser_engine_prepare_html_layout(url, 1024u, 768u) == 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_html_layout_ready;
    } else if (zbrowser_engine_css_selected == 0) {
        zbrowser_engine_status_text = zbrowser_engine_status_css_select_failed;
    }
    return 0;
}

static uint32_t zbrowser_engine_skip_ignored_raw(const uint8_t *html,
        uint32_t size, uint32_t pos, const char *tag_name) {
    while (pos < size) {
        if (html[pos] == '<' &&
            pos + 2u < size &&
            html[pos + 1u] == '/' &&
            zbrowser_engine_ci_starts(html, size, pos + 2u, tag_name)) {
            return zbrowser_engine_skip_tag(html, size, pos + 2u);
        }
        ++pos;
    }
    return size;
}

static int zbrowser_engine_emit_raw_line(char *line,
        uint32_t *line_len,
        uint32_t *line_index,
        uint32_t scroll_line,
        uint32_t visible_lines,
        uint32_t fg,
        uint32_t bg) {
    uint32_t draw_line;

    if (*line_len == 0) {
        return 0;
    }
    line[*line_len] = 0;
    if (*line_index >= scroll_line) {
        draw_line = *line_index - scroll_line;
        if (draw_line < visible_lines) {
            draw_text_at_pixel(10u,
                               76u + draw_line * 16u,
                               (const uint8_t *)line,
                               fg,
                               bg);
        }
    }
    *line_len = 0;
    *line_index = *line_index + 1u;
    return *line_index >= scroll_line + visible_lines ? 1 : 0;
}

static int zbrowser_engine_paint_raw_html(const uint8_t *html,
        uint32_t size,
        uint32_t scroll_line,
        uint32_t viewport_width,
        uint32_t viewport_height) {
    char line[160];
    uint32_t line_len = 0;
    uint32_t line_index = 0;
    uint32_t max_cols;
    uint32_t visible_lines;
    int pending_space = 0;

    if (html == 0 || size == 0 || viewport_height <= 96u) {
        return -1;
    }

    max_cols = viewport_width > 40u ? (viewport_width - 20u) / 8u : 20u;
    if (max_cols >= sizeof(line)) {
        max_cols = sizeof(line) - 1u;
    }
    if (max_cols < 20u) {
        max_cols = 20u;
    }
    visible_lines = (viewport_height - 96u) / 16u;
    if (visible_lines == 0) {
        return -1;
    }

    for (uint32_t pos = 0; pos < size;) {
        uint8_t ch = html[pos];

        if (ch == '<') {
            uint32_t tag_start = pos + 1u;
            uint32_t next = zbrowser_engine_skip_tag(html, size, tag_start);
            if (zbrowser_engine_tag_name_is(html, size, tag_start, "script")) {
                pos = zbrowser_engine_skip_ignored_raw(html, size, next, "script");
                pending_space = 1;
                continue;
            }
            if (zbrowser_engine_tag_name_is(html, size, tag_start, "style")) {
                pos = zbrowser_engine_skip_ignored_raw(html, size, next, "style");
                pending_space = 1;
                continue;
            }
            if (zbrowser_engine_tag_name_is(html, size, tag_start, "br") ||
                zbrowser_engine_tag_name_is(html, size, tag_start, "p") ||
                zbrowser_engine_tag_name_is(html, size, tag_start, "p") ||
                zbrowser_engine_tag_name_is(html, size, tag_start, "div") ||
                zbrowser_engine_tag_name_is(html, size, tag_start, "div") ||
                zbrowser_engine_tag_name_is(html, size, tag_start, "h1") ||
                zbrowser_engine_tag_name_is(html, size, tag_start, "h2") ||
                zbrowser_engine_tag_name_is(html, size, tag_start, "h3") ||
                zbrowser_engine_tag_name_is(html, size, tag_start, "li") ||
                zbrowser_engine_tag_name_is(html, size, tag_start, "tr")) {
                if (zbrowser_engine_emit_raw_line(line, &line_len, &line_index,
                        scroll_line, visible_lines, 0xd8e1e8u, 0x101820u)) {
                    return 0;
                }
            } else {
                pending_space = 1;
            }
            pos = next;
            continue;
        }

        if (ch == '&') {
            if (zbrowser_engine_ci_starts(html, size, pos, "&amp;")) {
                ch = '&';
                pos += 5u;
            } else if (zbrowser_engine_ci_starts(html, size, pos, "&lt;")) {
                ch = '<';
                pos += 4u;
            } else if (zbrowser_engine_ci_starts(html, size, pos, "&gt;")) {
                ch = '>';
                pos += 4u;
            } else if (zbrowser_engine_ci_starts(html, size, pos, "&nbsp;")) {
                ch = ' ';
                pos += 6u;
            } else {
                ch = ' ';
                ++pos;
            }
        } else {
            ++pos;
        }

        if (zbrowser_engine_is_space(ch)) {
            pending_space = 1;
            continue;
        }
        if (ch < 32u) {
            continue;
        }
        if (pending_space && line_len != 0) {
            if (line_len + 1u >= max_cols &&
                zbrowser_engine_emit_raw_line(line, &line_len, &line_index,
                    scroll_line, visible_lines, 0xd8e1e8u, 0x101820u)) {
                return 0;
            }
            if (line_len != 0) {
                line[line_len++] = ' ';
            }
        }
        pending_space = 0;
        line[line_len++] = (char)ch;
        if (line_len >= max_cols &&
            zbrowser_engine_emit_raw_line(line, &line_len, &line_index,
                scroll_line, visible_lines, 0xd8e1e8u, 0x101820u)) {
            return 0;
        }
    }
    (void)zbrowser_engine_emit_raw_line(line, &line_len, &line_index,
        scroll_line, visible_lines, 0xd8e1e8u, 0x101820u);
    return 0;
}

static int zbrowser_engine_node_name_equals(dom_node *node, const char *want) {
    dom_string *name = 0;
    const char *data;
    size_t len;
    size_t i = 0;
    int result = 0;

    if (node == 0 || want == 0 ||
        dom_node_get_node_name(node, &name) != DOM_NO_ERR || name == 0) {
        return 0;
    }
    data = dom_string_data(name);
    len = dom_string_byte_length(name);
    while (want[i] != '\0') {
        if (i >= len || zbrowser_engine_lower((uint8_t)data[i]) != (uint8_t)want[i]) {
            dom_string_unref(name);
            return 0;
        }
        ++i;
    }
    result = i == len;
    dom_string_unref(name);
    return result;
}

static int zbrowser_engine_node_is_block(dom_node *node) {
    return zbrowser_engine_node_name_equals(node, "body") ||
           zbrowser_engine_node_name_equals(node, "div") ||
           zbrowser_engine_node_name_equals(node, "p") ||
           zbrowser_engine_node_name_equals(node, "section") ||
           zbrowser_engine_node_name_equals(node, "article") ||
           zbrowser_engine_node_name_equals(node, "header") ||
           zbrowser_engine_node_name_equals(node, "footer") ||
           zbrowser_engine_node_name_equals(node, "nav") ||
           zbrowser_engine_node_name_equals(node, "main") ||
           zbrowser_engine_node_name_equals(node, "form") ||
           zbrowser_engine_node_name_equals(node, "ul") ||
           zbrowser_engine_node_name_equals(node, "ol") ||
           zbrowser_engine_node_name_equals(node, "li") ||
           zbrowser_engine_node_name_equals(node, "table") ||
           zbrowser_engine_node_name_equals(node, "tr") ||
           zbrowser_engine_node_name_equals(node, "h1") ||
           zbrowser_engine_node_name_equals(node, "h2") ||
           zbrowser_engine_node_name_equals(node, "h3") ||
           zbrowser_engine_node_name_equals(node, "h4") ||
           zbrowser_engine_node_name_equals(node, "h5") ||
           zbrowser_engine_node_name_equals(node, "h6");
}

static uint32_t zbrowser_engine_fixed_px(css_computed_style *style,
        const css_unit_ctx *unit_ctx, css_fixed length, css_unit unit) {
    css_fixed px = css_unit_len2device_px(style, unit_ctx, length, unit);

    if (px <= 0) {
        return 0;
    }
    return (uint32_t)FIXTOINT(px);
}

static uint32_t zbrowser_engine_lines_for_px(uint32_t px) {
    return px == 0u ? 0u : (px + 15u) / 16u;
}

static uint32_t zbrowser_engine_style_line_px(const zbrowser_engine_style_t *style) {
    uint32_t font_px = style->font_px == 0u ? 16u : style->font_px;
    uint32_t line_px = style->line_height_px;

    if (line_px < font_px + 4u) {
        line_px = font_px + 4u;
    }
    if (line_px < 12u) {
        line_px = 12u;
    }
    if (line_px > 96u) {
        line_px = 96u;
    }
    return line_px;
}

static uint32_t zbrowser_engine_style_glyph_width(const zbrowser_engine_style_t *style) {
    uint32_t font_px = style->font_px == 0u ? 16u : style->font_px;
    uint32_t width = (font_px + 1u) / 2u;

    if (width < 6u) {
        width = 6u;
    }
    if (width > 32u) {
        width = 32u;
    }
    return width;
}

static uint32_t zbrowser_engine_visible_y(const zbrowser_engine_paint_t *paint,
        uint32_t logical_line) {
    return 76u + (logical_line - paint->scroll_line) * 16u;
}

static int zbrowser_engine_line_visible(const zbrowser_engine_paint_t *paint,
        uint32_t logical_line, uint32_t height) {
    uint32_t y;

    if (logical_line < paint->scroll_line) {
        return 0;
    }
    y = zbrowser_engine_visible_y(paint, logical_line);
    return y + height < paint->viewport_height;
}

static int zbrowser_engine_display_is_block(uint8_t display) {
    return display == CSS_DISPLAY_BLOCK ||
           display == CSS_DISPLAY_LIST_ITEM ||
           display == CSS_DISPLAY_TABLE ||
           display == CSS_DISPLAY_TABLE_ROW_GROUP ||
           display == CSS_DISPLAY_TABLE_HEADER_GROUP ||
           display == CSS_DISPLAY_TABLE_FOOTER_GROUP ||
           display == CSS_DISPLAY_TABLE_ROW ||
           display == CSS_DISPLAY_TABLE_CELL ||
           display == CSS_DISPLAY_TABLE_CAPTION ||
           display == CSS_DISPLAY_FLEX ||
           display == CSS_DISPLAY_GRID;
}

static void zbrowser_engine_paint_flush(zbrowser_engine_paint_t *paint,
        const zbrowser_engine_style_t *style) {
    uint32_t visible_line;
    uint32_t y;
    uint32_t x = 10u + style->indent_px;
    uint32_t fg = style->fg;
    uint32_t bg = style->bg;
    uint32_t line_px = zbrowser_engine_style_line_px(style);
    uint32_t font_px = style->font_px == 0u ? 16u : style->font_px;
    uint32_t char_w = zbrowser_engine_style_glyph_width(style);
    uint32_t advance = char_w;
    uint32_t line_step = zbrowser_engine_lines_for_px(line_px);
    uint32_t text_w;

    if (line_step == 0u) {
        line_step = 1u;
    }
    if (paint->line_len == 0u) {
        return;
    }
    paint->line[paint->line_len] = '\0';
    if ((fg & 0x00ffffffu) == (bg & 0x00ffffffu)) {
        fg = (bg & 0x00ffffffu) == 0x000000u ? 0xffffffu : 0x000000u;
    }
    if (paint->line_index >= paint->scroll_line) {
        visible_line = paint->line_index - paint->scroll_line;
        y = 76u + visible_line * 16u;
        text_w = paint->line_len * char_w;
        if ((style->text_align == CSS_TEXT_ALIGN_CENTER ||
             style->text_align == CSS_TEXT_ALIGN_LIBCSS_CENTER) &&
            paint->viewport_width > text_w + x) {
            x = (paint->viewport_width - text_w) / 2u;
        } else if ((style->text_align == CSS_TEXT_ALIGN_RIGHT ||
                    style->text_align == CSS_TEXT_ALIGN_LIBCSS_RIGHT) &&
                   paint->viewport_width > text_w + 10u) {
            x = paint->viewport_width - text_w - 10u;
        }
        if (y + line_px < paint->viewport_height) {
            uint32_t bg_width = paint->viewport_width > x + style->padding_right_px + 10u
                ? paint->viewport_width - x - style->padding_right_px - 10u
                : text_w;
            gfx_fill_rect(x,
                          y,
                          bg_width,
                          line_px,
                          bg & 0x00ffffffu);
            if (font_px >= 9u) {
                draw_text_sized_at_pixel(x,
                                         y,
                                         (const uint8_t *)paint->line,
                                         fg,
                                         bg,
                                         char_w,
                                         font_px,
                                         advance,
                                         style->bold != 0u,
                                         style->italic != 0u);
            } else if (style->scale > 1u) {
                draw_text_scaled_at_pixel(x, y, (const uint8_t *)paint->line, fg, bg, style->scale);
            } else {
                draw_text_at_pixel(x, y, (const uint8_t *)paint->line, fg, bg);
            }
        }
    }
    paint->line_len = 0;
    paint->line_index += line_step;
}

static int zbrowser_engine_element_attr(dom_node *node,
        const char *name_text, char *out, uint32_t out_size) {
    dom_string *name = 0;
    dom_string *value = 0;
    const char *data;
    size_t len;

    if (out_size == 0u) {
        return 0;
    }
    out[0] = '\0';
    if (node == 0 || name_text == 0 ||
        dom_string_create_interned((const uint8_t *)name_text,
                                   strlen(name_text),
                                   &name) != DOM_NO_ERR) {
        return 0;
    }
    if (dom_element_get_attribute((dom_element *)node, name, &value) != DOM_NO_ERR || value == 0) {
        dom_string_unref(name);
        return 0;
    }
    data = dom_string_data(value);
    len = dom_string_byte_length(value);
    if (len >= out_size) {
        len = out_size - 1u;
    }
    memcpy(out, data, len);
    out[len] = '\0';
    dom_string_unref(value);
    dom_string_unref(name);
    return len != 0u;
}

static int zbrowser_engine_hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int zbrowser_engine_parse_legacy_color(const char *text, uint32_t *out) {
    uint32_t pos = 0;
    uint32_t color = 0;

    if (text == 0 || out == 0) {
        return 0;
    }
    while (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n') {
        ++pos;
    }
    if (text[pos] == '#') {
        ++pos;
    }
    for (uint32_t i = 0; i < 6u; ++i) {
        int digit = zbrowser_engine_hex_digit(text[pos + i]);
        if (digit < 0) {
            return 0;
        }
        color = (color << 4) | (uint32_t)digit;
    }
    *out = color;
    return 1;
}

static int zbrowser_engine_attr_color(dom_node *node, const char *attr, uint32_t *out) {
    char value[32];

    if (!zbrowser_engine_element_attr(node, attr, value, sizeof(value))) {
        return 0;
    }
    return zbrowser_engine_parse_legacy_color(value, out);
}

static int zbrowser_engine_attr_contains_ci(dom_node *node, const char *attr, const char *needle) {
    char value[128];
    uint32_t i;

    if (!zbrowser_engine_element_attr(node, attr, value, sizeof(value)) || needle == 0 || needle[0] == '\0') {
        return 0;
    }
    for (i = 0; value[i] != '\0'; ++i) {
        uint32_t j = 0;
        while (needle[j] != '\0' &&
               zbrowser_engine_lower((uint8_t)value[i + j]) == zbrowser_engine_lower((uint8_t)needle[j])) {
            ++j;
        }
        if (needle[j] == '\0') {
            return 1;
        }
    }
    return 0;
}

static void zbrowser_engine_apply_legacy_attrs(dom_node *node,
        zbrowser_engine_style_t *style) {
    uint32_t color;

    if (zbrowser_engine_attr_color(node, "bgcolor", &color)) {
        style->bg = color;
    }
    if (zbrowser_engine_attr_color(node, "text", &color)) {
        style->fg = color;
    }
    if (zbrowser_engine_attr_color(node, "color", &color)) {
        style->fg = color;
    }
    if (zbrowser_engine_attr_contains_ci(node, "align", "center") ||
        zbrowser_engine_node_name_equals(node, "center")) {
        style->text_align = CSS_TEXT_ALIGN_CENTER;
    }
    if (zbrowser_engine_attr_contains_ci(node, "align", "right")) {
        style->text_align = CSS_TEXT_ALIGN_RIGHT;
    }
    if (zbrowser_engine_attr_contains_ci(node, "style", "display:none") ||
        zbrowser_engine_attr_contains_ci(node, "style", "display: none") ||
        zbrowser_engine_attr_contains_ci(node, "style", "visibility:hidden") ||
        zbrowser_engine_attr_contains_ci(node, "style", "visibility: hidden") ||
        zbrowser_engine_attr_contains_ci(node, "style", "left:-9999") ||
        zbrowser_engine_attr_contains_ci(node, "style", "left: -9999") ||
        zbrowser_engine_attr_contains_ci(node, "type", "hidden")) {
        style->display_none = 1u;
    }
}

static void zbrowser_engine_paint_replaced_box(zbrowser_engine_paint_t *paint,
        dom_node *node, const zbrowser_engine_style_t *style, const char *fallback,
        uint32_t box_width, uint32_t box_height) {
    char label[96];
    uint32_t x;
    uint32_t y;
    uint32_t fill;

    zbrowser_engine_paint_flush(paint, style);
    if (!zbrowser_engine_element_attr(node, "alt", label, sizeof(label)) &&
        !zbrowser_engine_element_attr(node, "value", label, sizeof(label)) &&
        !zbrowser_engine_element_attr(node, "placeholder", label, sizeof(label)) &&
        !zbrowser_engine_element_attr(node, "name", label, sizeof(label))) {
        memcpy(label, fallback, strlen(fallback) + 1u);
    }

    if (zbrowser_engine_line_visible(paint, paint->line_index, box_height)) {
        x = 10u + style->indent_px;
        y = zbrowser_engine_visible_y(paint, paint->line_index);
        if (x + box_width + 10u > paint->viewport_width && paint->viewport_width > x + 20u) {
            box_width = paint->viewport_width - x - 10u;
        }
        fill = zbrowser_engine_node_name_equals(node, "input") ? 0xf6fbffu : 0xeef6eau;
        gfx_fill_rect(x, y, box_width, box_height, 0x32444eu);
        if (box_width > 2u && box_height > 2u) {
            gfx_fill_rect(x + 1u, y + 1u, box_width - 2u, box_height - 2u, fill);
        }
        gfx_draw_rect(x, y, box_width, box_height, 0x9fb3c1u);
        draw_text_at_pixel(x + 6u,
                           y + (box_height > 22u ? 7u : 4u),
                           (const uint8_t *)label,
                           0x1d3948u,
                           fill);
    }
    paint->line_index += (box_height + 15u) / 16u;
}

static void zbrowser_engine_paint_word(zbrowser_engine_paint_t *paint,
        const char *word, uint32_t len, const zbrowser_engine_style_t *style) {
    uint32_t char_w = zbrowser_engine_style_glyph_width(style);
    uint32_t reserved = 20u + style->indent_px + style->padding_right_px;
    uint32_t max_chars = paint->viewport_width > reserved + char_w
        ? (paint->viewport_width - reserved) / char_w
        : 1u;

    if (max_chars >= sizeof(paint->line)) {
        max_chars = sizeof(paint->line) - 1u;
    }
    if (len == 0u) {
        return;
    }
    if (paint->line_len != 0u && paint->line_len + 1u + len > max_chars) {
        zbrowser_engine_paint_flush(paint, style);
    }
    if (paint->line_len != 0u && paint->line_len + 1u < sizeof(paint->line)) {
        paint->line[paint->line_len++] = ' ';
    }
    for (uint32_t i = 0; i < len && paint->line_len + 1u < sizeof(paint->line); ++i) {
        paint->line[paint->line_len++] = word[i];
        if (paint->line_len >= max_chars) {
            zbrowser_engine_paint_flush(paint, style);
        }
    }
}

static void zbrowser_engine_paint_text(zbrowser_engine_paint_t *paint,
        const char *data, size_t len, const zbrowser_engine_style_t *style) {
    char word[96];
    uint32_t word_len = 0;

    for (size_t i = 0; i < len; ++i) {
        char ch = data[i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            zbrowser_engine_paint_word(paint, word, word_len, style);
            word_len = 0;
            continue;
        }
        if (word_len + 1u < sizeof(word)) {
            word[word_len++] = ch;
        } else {
            zbrowser_engine_paint_word(paint, word, word_len, style);
            word_len = 0;
        }
    }
    zbrowser_engine_paint_word(paint, word, word_len, style);
}

static void zbrowser_engine_select_node_style(zbrowser_engine_paint_t *paint,
        dom_node *node, zbrowser_engine_style_t *style) {
    css_select_results *results = 0;
    css_computed_style *computed;
    css_color color;
    css_color background;
    css_fixed length;
    css_unit unit;
    css_error error;

    if (paint->select_ctx == 0 || node == 0) {
        return;
    }
    error = css_select_style(paint->select_ctx,
                             node,
                             &paint->unit_ctx,
                             &paint->media,
                             0,
                             &zbrowser_engine_select_handler,
                             0,
                             &results);
    if (error != CSS_OK || results == 0) {
        return;
    }
    computed = results->styles[CSS_PSEUDO_ELEMENT_NONE];
    if (computed != 0) {
        color = style->fg | 0xff000000u;
        background = style->bg | 0xff000000u;
        (void)css_computed_color(computed, &color);
        if (css_computed_background_color(computed, &background) == CSS_BACKGROUND_COLOR_COLOR) {
            style->bg = background & 0x00ffffffu;
        }
        if (css_computed_font_size(computed, &length, &unit) == CSS_FONT_SIZE_DIMENSION) {
            uint32_t font_px = zbrowser_engine_fixed_px(computed, &paint->unit_ctx, length, unit);
            if (font_px >= 8u && font_px <= 96u) {
                style->font_px = font_px;
                style->scale = font_px >= 22u ? 2u : 1u;
            }
        }
        {
            uint8_t line_height = css_computed_line_height(computed, &length, &unit);
            if (line_height == CSS_LINE_HEIGHT_DIMENSION) {
                uint32_t line_px = zbrowser_engine_fixed_px(computed, &paint->unit_ctx, length, unit);
                if (line_px >= 8u && line_px <= 128u) {
                    style->line_height_px = line_px;
                }
            } else if (line_height == CSS_LINE_HEIGHT_NUMBER) {
                uint32_t font_px = style->font_px == 0u ? 16u : style->font_px;
                uint32_t ratio = (uint32_t)FIXTOINT(FMUL(length, INTTOFIX(100)));
                uint32_t line_px = (font_px * ratio) / 100u;
                if (line_px >= 8u && line_px <= 128u) {
                    style->line_height_px = line_px;
                }
            } else if (line_height == CSS_LINE_HEIGHT_NORMAL) {
                uint32_t font_px = style->font_px == 0u ? 16u : style->font_px;
                style->line_height_px = font_px + 4u;
            }
        }
        {
            uint8_t font_weight = css_computed_font_weight(computed);
            uint8_t font_style = css_computed_font_style(computed);
            style->bold = font_weight == CSS_FONT_WEIGHT_BOLD ||
                          font_weight == CSS_FONT_WEIGHT_BOLDER ||
                          font_weight == CSS_FONT_WEIGHT_600 ||
                          font_weight == CSS_FONT_WEIGHT_700 ||
                          font_weight == CSS_FONT_WEIGHT_800 ||
                          font_weight == CSS_FONT_WEIGHT_900;
            style->italic = font_style == CSS_FONT_STYLE_ITALIC ||
                            font_style == CSS_FONT_STYLE_OBLIQUE;
        }
        if (css_computed_margin_top(computed, &length, &unit) == CSS_MARGIN_SET) {
            style->margin_top_lines = zbrowser_engine_lines_for_px(
                zbrowser_engine_fixed_px(computed, &paint->unit_ctx, length, unit));
        }
        if (css_computed_margin_bottom(computed, &length, &unit) == CSS_MARGIN_SET) {
            style->margin_bottom_lines = zbrowser_engine_lines_for_px(
                zbrowser_engine_fixed_px(computed, &paint->unit_ctx, length, unit));
        }
        if (css_computed_padding_left(computed, &length, &unit) == CSS_PADDING_SET) {
            style->indent_px += zbrowser_engine_fixed_px(computed, &paint->unit_ctx, length, unit);
        }
        if (css_computed_padding_right(computed, &length, &unit) == CSS_PADDING_SET) {
            style->padding_right_px += zbrowser_engine_fixed_px(computed, &paint->unit_ctx, length, unit);
        }
        style->display = css_computed_display(computed, false);
        style->text_align = css_computed_text_align(computed);
        style->fg = color & 0x00ffffffu;
        style->display_none = style->display == CSS_DISPLAY_NONE ||
            css_computed_visibility(computed) == CSS_VISIBILITY_HIDDEN ||
            css_computed_visibility(computed) == CSS_VISIBILITY_COLLAPSE;
    }
    css_select_results_destroy(results);
}

static void zbrowser_engine_paint_node(zbrowser_engine_paint_t *paint,
        dom_node *node, zbrowser_engine_style_t style) {
    dom_node_type type = DOM_DOCUMENT_NODE;
    dom_node *child = 0;
    dom_node *next = 0;
    dom_string *value = 0;
    int block;

    if (node == 0) {
        return;
    }
    if (dom_node_get_node_type(node, &type) != DOM_NO_ERR) {
        return;
    }
    if (type == DOM_TEXT_NODE) {
        if (dom_node_get_node_value(node, &value) == DOM_NO_ERR && value != 0) {
            zbrowser_engine_paint_text(paint,
                                       dom_string_data(value),
                                       dom_string_byte_length(value),
                                       &style);
            dom_string_unref(value);
        }
        return;
    }
    if (type == DOM_ELEMENT_NODE) {
        if (zbrowser_engine_node_name_equals(node, "script") ||
            zbrowser_engine_node_name_equals(node, "style") ||
            zbrowser_engine_node_name_equals(node, "head")) {
            return;
        }
        if (zbrowser_engine_node_name_equals(node, "br")) {
            zbrowser_engine_paint_flush(paint, &style);
            return;
        }
        zbrowser_engine_select_node_style(paint, node, &style);
        zbrowser_engine_apply_legacy_attrs(node, &style);
        if (style.display_none) {
            return;
        }
        if (zbrowser_engine_node_name_equals(node, "img")) {
            zbrowser_engine_paint_replaced_box(paint, node, &style, "[image]", 64u, 48u);
            return;
        }
        if (zbrowser_engine_node_name_equals(node, "input") ||
            zbrowser_engine_node_name_equals(node, "textarea") ||
            zbrowser_engine_node_name_equals(node, "select")) {
            zbrowser_engine_paint_replaced_box(paint, node, &style, "[____]", 220u, 28u);
            return;
        }
        block = zbrowser_engine_display_is_block(style.display) || zbrowser_engine_node_is_block(node);
        if (block) {
            zbrowser_engine_paint_flush(paint, &style);
            paint->line_index += style.margin_top_lines;
        }
    } else {
        block = 0;
    }

    if (dom_node_get_first_child(node, &child) == DOM_NO_ERR && child != 0) {
        while (child != 0) {
            zbrowser_engine_paint_node(paint, child, style);
            if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) {
                next = 0;
            }
            dom_node_unref(child);
            child = next;
        }
    }
    if (block) {
        zbrowser_engine_paint_flush(paint, &style);
        paint->line_index += style.margin_bottom_lines;
    }
}

static int zbrowser_engine_paint_dom(uint32_t scroll_line,
        uint32_t viewport_width, uint32_t viewport_height) {
    zbrowser_engine_paint_t paint;
    zbrowser_engine_style_t root_style;
    dom_element *root = 0;

    if (zbrowser_engine_document == 0 ||
        dom_document_get_document_element(zbrowser_engine_document, &root) != DOM_NO_ERR ||
        root == 0) {
        return -1;
    }
    memset(&paint, 0, sizeof(paint));
    paint.viewport_width = viewport_width;
    paint.viewport_height = viewport_height;
    paint.scroll_line = scroll_line;
    paint.unit_ctx.viewport_width = INTTOFIX(viewport_width);
    paint.unit_ctx.viewport_height = INTTOFIX(viewport_height);
    paint.unit_ctx.font_size_default = INTTOFIX(16);
    paint.unit_ctx.font_size_minimum = INTTOFIX(6);
    paint.unit_ctx.device_dpi = INTTOFIX(96);
    paint.media.type = CSS_MEDIA_SCREEN;
    paint.media.width = paint.unit_ctx.viewport_width;
    paint.media.height = paint.unit_ctx.viewport_height;
    paint.media.aspect_ratio = viewport_height != 0u
        ? FDIV(paint.media.width, paint.media.height)
        : INTTOFIX(1);
    paint.media.orientation = CSS_MEDIA_ORIENTATION_LANDSCAPE;
    paint.media.resolution.value = INTTOFIX(96);
    paint.media.resolution.unit = CSS_UNIT_PX;
    paint.media.color = INTTOFIX(24);
    paint.media.pointer = CSS_MEDIA_POINTER_FINE;
    paint.media.any_pointer = CSS_MEDIA_POINTER_FINE;
    paint.media.hover = CSS_MEDIA_HOVER_HOVER;
    paint.media.any_hover = CSS_MEDIA_HOVER_HOVER;
    paint.media.scripting = CSS_MEDIA_SCRIPTING_NONE;

    paint.select_ctx = zbrowser_engine_html_select_ctx;
    if (paint.select_ctx == 0) {
        dom_node_unref(root);
        return -1;
    }

    gfx_fill_rect(0u,
                  58u,
                  viewport_width,
                  viewport_height > 82u ? viewport_height - 82u : 0u,
                  zbrowser_engine_root_background & 0x00ffffffu);
    memset(&root_style, 0, sizeof(root_style));
    root_style.fg = zbrowser_engine_root_color & 0x00ffffffu;
    root_style.bg = zbrowser_engine_root_background & 0x00ffffffu;
    root_style.scale = 1u;
    root_style.font_px = 16u;
    root_style.line_height_px = 20u;
    root_style.display = CSS_DISPLAY_BLOCK;
    root_style.text_align = CSS_TEXT_ALIGN_LEFT;
    zbrowser_engine_paint_node(&paint, (dom_node *)root, root_style);
    zbrowser_engine_paint_flush(&paint, &root_style);
    dom_node_unref(root);
    snprintf((char *)zbrowser_engine_status_detail,
             sizeof(zbrowser_engine_status_detail),
             "NS dom fallback css%u/%u selected%u lines%u root %s",
             zbrowser_engine_css_blocks,
             zbrowser_engine_sheet_count,
             zbrowser_engine_css_selected,
             paint.line_index,
             zbrowser_engine_root_name);
    zbrowser_engine_status_detail[sizeof(zbrowser_engine_status_detail) - 1u] = 0;
    zbrowser_engine_status_text = zbrowser_engine_status_detail;
    return 0;
}
#endif

int zbrowser_engine_init_c(const zbrowser_engine_host_t *host) {
    (void)host;
    zbrowser_engine_ready = 1;
    zbrowser_engine_status_text = zbrowser_engine_status_initial;
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
    zbrowser_engine_core_initialised = 0;
    (void)zbrowser_engine_core_init();
    zbrowser_engine_document = 0;
    zbrowser_engine_css_blocks = 0;
    zbrowser_engine_css_selected = 0;
    zbrowser_engine_css_compat_blocks = 0;
    zbrowser_engine_css_compat_vars = 0;
    zbrowser_engine_css_compat_groups = 0;
    zbrowser_engine_sheet_count = 0;
    zbrowser_engine_css_skipped_blocks = 0;
    zbrowser_engine_ua_sheet = 0;
    zbrowser_engine_html_ready = 0;
    zbrowser_engine_html_select_ctx = 0;
    zbrowser_engine_base_url = 0;
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
    zbrowser_engine_content_initialised = 0;
    zbrowser_engine_content = 0;
    zbrowser_engine_content_url = 0;
    zbrowser_engine_content_source_html = 0;
    zbrowser_engine_content_source_size = 0;
    zbrowser_engine_content_source_url[0] = 0;
#ifdef ZBROWSER_ENGINE_USE_REAL_NETSURF_CACHE
    memset(&zbrowser_engine_content_object, 0, sizeof(zbrowser_engine_content_object));
    memset(&zbrowser_engine_content_llcache, 0, sizeof(zbrowser_engine_content_llcache));
    memset(zbrowser_engine_content_headers, 0, sizeof(zbrowser_engine_content_headers));
#else
    memset(&zbrowser_engine_content_llcache, 0, sizeof(zbrowser_engine_content_llcache));
#endif
#endif
    zbrowser_engine_root_name[0] = 0;
    memset(zbrowser_engine_sheets, 0, sizeof(zbrowser_engine_sheets));
    memset(&zbrowser_engine_html, 0, sizeof(zbrowser_engine_html));
#endif
    return 0;
}

const uint8_t *zbrowser_engine_status_c(void) {
    return zbrowser_engine_status_text;
}

int zbrowser_engine_prepare_c(const uint8_t *url, const uint8_t *html, uint32_t size) {
    if (url == 0 || html == 0 || size == 0) {
        return -1;
    }
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
    (void)zbrowser_engine_prepare_dom(url, html, size);
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
    zbrowser_engine_release_content();
    zbrowser_engine_content_source_html = html;
    zbrowser_engine_content_source_size = size;
    zbrowser_engine_content_source_url[0] = 0;
    if (url != 0) {
        size_t i = 0;
        while (url[i] != 0 && i + 1u < sizeof(zbrowser_engine_content_source_url)) {
            zbrowser_engine_content_source_url[i] = (char)url[i];
            ++i;
        }
        zbrowser_engine_content_source_url[i] = 0;
    }
#endif
    return 0;
#else
    zbrowser_engine_status_text = zbrowser_engine_status_ready;
    return 0;
#endif
}

int zbrowser_engine_prepare_html_view_c(const uint8_t *url,
                                        const uint8_t *html,
                                        uint32_t size,
                                        const zbrowser_engine_view_t *view) {
    if (url == 0 || html == 0 || size == 0 || view == 0 ||
        view->width < 16u || view->height < 16u) {
        return -1;
    }
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
    int needs_prepare = zbrowser_engine_content == 0 ||
        zbrowser_engine_content_source_html != html ||
        zbrowser_engine_content_source_size != size ||
        strcmp(zbrowser_engine_content_source_url,
               (const char *)(url != 0 ? url : (const uint8_t *)"about:blank")) != 0 ||
        zbrowser_engine_content_viewport_width != view->width ||
        zbrowser_engine_content_viewport_height != view->height;

    zbrowser_engine_content_source_html = html;
    zbrowser_engine_content_source_size = size;
    zbrowser_engine_content_source_url[0] = 0;
    for (size_t i = 0; url[i] != 0 && i + 1u < sizeof(zbrowser_engine_content_source_url); ++i) {
        zbrowser_engine_content_source_url[i] = (char)url[i];
        zbrowser_engine_content_source_url[i + 1u] = 0;
    }
    if (needs_prepare) {
        (void)zbrowser_engine_prepare_content_pipeline(url,
                                                       html,
                                                       size,
                                                       view->width,
                                                       view->height);
    }
    if (zbrowser_engine_content == 0 ||
        zbrowser_engine_content->status == CONTENT_STATUS_ERROR) {
        return -1;
    }
    if (zbrowser_engine_content->status == CONTENT_STATUS_READY ||
        zbrowser_engine_content->status == CONTENT_STATUS_DONE) {
        return 0;
    }
    zbrowser_engine_status_text = zbrowser_engine_content_detail_status("waiting");
    return -1;
#else
    (void)view;
    return zbrowser_engine_prepare_c(url, html, size);
#endif
#else
    (void)url;
    (void)html;
    (void)size;
    (void)view;
    return -1;
#endif
}

int zbrowser_engine_render_html_view_c(const uint8_t *url,
                                       const uint8_t *html,
                                       uint32_t size,
                                       const zbrowser_engine_view_t *view) {
    if (url == 0 || html == 0 || size == 0 || view == 0 ||
        view->width < 16u || view->height < 16u) {
        return -1;
    }
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
    if (zbrowser_engine_content == 0 ||
        zbrowser_engine_content_source_html != html ||
        zbrowser_engine_content_source_size != size ||
        zbrowser_engine_content_viewport_width != view->width ||
        zbrowser_engine_content_viewport_height != view->height) {
        if (zbrowser_engine_prepare_html_view_c(url, html, size, view) != 0) {
            return -1;
        }
    }
    return zbrowser_engine_redraw_content_view(view->x,
                                              view->y,
                                              view->width,
                                              view->height,
                                              zbrowser_engine_scroll_lines_to_px(view->scroll));
#else
    (void)url;
    (void)html;
    (void)size;
    (void)view;
    zbrowser_engine_status_text = zbrowser_engine_status_content_unavailable;
    return -1;
#endif
#else
    (void)url;
    (void)html;
    (void)size;
    (void)view;
    return -1;
#endif
}

uint32_t zbrowser_engine_root_color_c(void) {
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
    return zbrowser_engine_root_color;
#else
    return 0xff000000u;
#endif
}

uint32_t zbrowser_engine_root_background_c(void) {
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
    return zbrowser_engine_root_background;
#else
    return 0x00000000u;
#endif
}

uint32_t zbrowser_engine_root_display_c(void) {
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
    return zbrowser_engine_root_display;
#else
    return 0u;
#endif
}

int zbrowser_engine_draw_c(const uint8_t *html,
                           uint32_t size,
                           uint32_t scroll_line,
                           uint32_t viewport_width,
                           uint32_t viewport_height) {
    if (!zbrowser_engine_ready || viewport_height < 96u) {
        return -1;
    }

#ifdef ZBROWSER_ENGINE_ENABLE_DOM
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
    int content_attempted = zbrowser_engine_content != 0 ||
        zbrowser_engine_content_source_html != 0;

    if ((zbrowser_engine_content == 0 ||
         zbrowser_engine_content_viewport_width != viewport_width ||
         zbrowser_engine_content_viewport_height != viewport_height) &&
        zbrowser_engine_content_source_html != 0 &&
        zbrowser_engine_content_source_size != 0) {
        (void)zbrowser_engine_prepare_content_pipeline(
            (const uint8_t *)(zbrowser_engine_content_source_url[0] != 0
                ? zbrowser_engine_content_source_url
                : "about:blank"),
            zbrowser_engine_content_source_html,
            zbrowser_engine_content_source_size,
            viewport_width,
            viewport_height);
    }
    if (zbrowser_engine_redraw_content_pipeline(scroll_line, viewport_width, viewport_height) == 0) {
        return 0;
    }
#endif
    if (zbrowser_engine_redraw_html(scroll_line, viewport_width, viewport_height) == 0) {
        return 0;
    }

    if (zbrowser_engine_paint_dom(scroll_line, viewport_width, viewport_height) == 0) {
        return 0;
    }

    if (zbrowser_engine_paint_raw_html(html,
                                       size,
                                       scroll_line,
                                       viewport_width,
                                       viewport_height) == 0) {
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
        if (content_attempted) {
            zbrowser_engine_status_text = zbrowser_engine_content_detail_status("raw-fallback");
        }
#endif
        return 0;
    }
#else
    (void)html;
    (void)size;
    (void)scroll_line;
    (void)viewport_width;
#endif

    draw_text_at_pixel(10,
                       76,
                       (const uint8_t *)"C browser engine ABI is staged outside the kernel.",
                       0xd8e1e8u,
                       0x101820u);
    draw_text_at_pixel(10,
                       98,
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
                       (const uint8_t *)"Staged NetSurf DOM parser is linked behind this boundary.",
#else
                       (const uint8_t *)"Next: replace the raw renderer with the staged NetSurf units.",
#endif
                       0x9fb3c1u,
                       0x101820u);
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
    if (zbrowser_engine_root_name[0] != 0 && viewport_height >= 128u) {
        draw_text_at_pixel(10,
                           120,
                           (const uint8_t *)zbrowser_engine_root_name,
                           0xf2c86bu,
                           0x101820u);
    }
    if (zbrowser_engine_css_blocks != 0 && viewport_height >= 150u) {
        draw_text_at_pixel(10,
                           142,
                           (const uint8_t *)"libcss accepted inline stylesheet blocks",
                           0xa7d99bu,
                           0x101820u);
    }
    if (zbrowser_engine_css_selected != 0 && viewport_height >= 172u) {
        draw_text_at_pixel(10,
                           164,
                           (const uint8_t *)"libcss computed root style",
                           zbrowser_engine_root_color & 0x00ffffffu,
                           zbrowser_engine_root_background & 0x00ffffffu);
    }
#endif
    return 0;
}

int zbrowser_engine_init(void) {
    return zbrowser_engine_init_c(0);
}

uint8_t *zbrowser_engine_status(void) {
    return (uint8_t *)zbrowser_engine_status_c();
}

int zbrowser_engine_poll(void) {
#ifdef ZBROWSER_ENGINE_USE_NETSURF_CONTENT
    return zbrowser_engine_poll_c();
#else
    return 0;
#endif
}

int zbrowser_engine_prepare(const uint8_t *url, const uint8_t *html, uint32_t size) {
    return zbrowser_engine_prepare_c(url, html, size);
}

int zbrowser_engine_prepare_html_view(const uint8_t *url,
                                      const uint8_t *html,
                                      uint32_t size,
                                      const zbrowser_engine_view_t *view) {
    return zbrowser_engine_prepare_html_view_c(url, html, size, view);
}

int zbrowser_engine_render_html_view(const uint8_t *url,
                                     const uint8_t *html,
                                     uint32_t size,
                                     const zbrowser_engine_view_t *view) {
    return zbrowser_engine_render_html_view_c(url, html, size, view);
}

void zbrowser_engine_invalidate_cache(void) {
    zbrowser_engine_invalidate_cache_c();
}

int netsurf_browser_prepare_html_view(const uint8_t *url,
                                      const uint8_t *html,
                                      uint32_t size,
                                      const zbrowser_engine_view_t *view) {
    return zbrowser_engine_prepare_html_view_c(url, html, size, view);
}

int netsurf_browser_render_html_view(const uint8_t *url,
                                     const uint8_t *html,
                                     uint32_t size,
                                     const zbrowser_engine_view_t *view) {
    return zbrowser_engine_render_html_view_c(url, html, size, view);
}

uint8_t *netsurf_browser_status(void) {
    return (uint8_t *)zbrowser_engine_status_c();
}

void netsurf_browser_invalidate_cache(void) {
    zbrowser_engine_invalidate_cache_c();
}

int netsurf_browser_poll(void) {
    return zbrowser_engine_poll();
}

int netsurf_browser_key_event(uint32_t key) {
#if defined(ZBROWSER_ENGINE_ENABLE_DOM) && defined(ZBROWSER_ENGINE_USE_NETSURF_CONTENT)
    if (zbrowser_engine_content == 0 ||
        (zbrowser_engine_content->status != CONTENT_STATUS_READY &&
         zbrowser_engine_content->status != CONTENT_STATUS_DONE) ||
        zbrowser_engine_content->handler == 0 ||
        zbrowser_engine_content->handler->keypress == 0) {
        return 0;
    }
    if (zbrowser_engine_content_opened == 0 &&
        zbrowser_engine_content->handler->open != 0 &&
        zbrowser_engine_content->handler->open(zbrowser_engine_content,
                                               (struct browser_window *)&zbrowser_engine_content_browser_window_cookie,
                                               0,
                                               0) != NSERROR_OK) {
        return 0;
    }
    zbrowser_engine_content_opened = 1;
    ++zbrowser_engine_form_key_count;
    if (zbrowser_engine_content->handler->keypress(zbrowser_engine_content, key)) {
        zbrowser_engine_content_needs_redraw = 1;
        return 1;
    }
#else
    (void)key;
#endif
    return 0;
}

int netsurf_browser_mouse_html_view(const zbrowser_engine_view_t *view,
                                    uint32_t x,
                                    uint32_t y,
                                    uint32_t mouse_state) {
#if defined(ZBROWSER_ENGINE_ENABLE_DOM) && defined(ZBROWSER_ENGINE_USE_NETSURF_CONTENT)
    int content_x;
    int content_y;

    if (view == 0 ||
        zbrowser_engine_content == 0 ||
        (zbrowser_engine_content->status != CONTENT_STATUS_READY &&
         zbrowser_engine_content->status != CONTENT_STATUS_DONE) ||
        zbrowser_engine_content->handler == 0) {
        return 0;
    }
    if (x < view->x || y < view->y ||
        x >= view->x + view->width ||
        y >= view->y + view->height) {
        return 0;
    }
    if (zbrowser_engine_content_opened == 0 &&
        zbrowser_engine_content->handler->open != 0 &&
        zbrowser_engine_content->handler->open(zbrowser_engine_content,
                                               (struct browser_window *)&zbrowser_engine_content_browser_window_cookie,
                                               0,
                                               0) != NSERROR_OK) {
        return 0;
    }
    zbrowser_engine_content_opened = 1;
    content_x = (int)x - (int)view->x;
    content_y = (int)y - (int)view->y +
                (int)zbrowser_engine_scroll_lines_to_px(view->scroll);
    if (zbrowser_engine_content->handler->mouse_action == 0) {
        return 0;
    }
    ++zbrowser_engine_form_mouse_count;
    if (zbrowser_engine_content->handler->mouse_action(
            zbrowser_engine_content,
            (struct browser_window *)&zbrowser_engine_content_browser_window_cookie,
            (browser_mouse_state)mouse_state,
            content_x,
            content_y) == NSERROR_OK) {
        ++zbrowser_engine_form_mouse_count;
        zbrowser_engine_content_needs_redraw = 1;
        return 1;
    }
#else
    (void)view;
    (void)x;
    (void)y;
    (void)mouse_state;
#endif
    return 0;
}


int netsurf_browser_consume_navigation(uint8_t *out, uint32_t capacity) {
    return zbrowser_lainos_consume_navigation(out, capacity);
}

uint32_t netsurf_port_dom_status(void) {
#ifdef ZBROWSER_ENGINE_ENABLE_DOM
    uint32_t status = 0;
    if (zbrowser_engine_document != 0 || zbrowser_engine_content != 0) {
        status |= 1u;
    }
    if (zbrowser_engine_css_selected != 0 ||
        (zbrowser_engine_content != 0 &&
         (zbrowser_engine_content->status == CONTENT_STATUS_READY ||
          zbrowser_engine_content->status == CONTENT_STATUS_DONE))) {
        status |= 2u;
    }
    return status;
#else
    return 0;
#endif
}

uint8_t *netsurf_port_status(void) {
    return (uint8_t *)zbrowser_engine_status_c();
}

uint32_t netsurf_kernel_frontend_smoke(void) {
    return zbrowser_lainos_frontend_smoke();
}

uint8_t *netsurf_kernel_frontend_status(void) {
    return (uint8_t *)zbrowser_lainos_frontend_status();
}

int zbrowser_engine_draw(const uint8_t *html,
                         uint32_t size,
                         uint32_t scroll_line,
                         uint32_t viewport_width,
                         uint32_t viewport_height) {
    return zbrowser_engine_draw_c(html, size, scroll_line, viewport_width, viewport_height);
}
