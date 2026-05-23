#include "weblayout.h"

#include <stdbool.h>
#include <stddef.h>

#include "kernel.h"
#include "libc.h"
#include "netsurf_port.h"
#include "libcss/computed.h"
#include "libcss/fpmath.h"
#include "libcss/properties.h"
#include "libcss/select.h"
#include "libcss/stylesheet.h"
#include "libcss/unit.h"
#include "libwapcaplet/libwapcaplet.h"

enum {
    WEB_PROP_DISPLAY = 0,
    WEB_PROP_VISIBILITY,
    WEB_PROP_TEXT_ALIGN,
    WEB_PROP_COLOR,
    WEB_PROP_POSITION,
    WEB_PROP_LEFT,
    WEB_PROP_RIGHT,
    WEB_PROP_TOP,
    WEB_PROP_BOTTOM,
    WEB_PROP_WIDTH,
    WEB_PROP_HEIGHT,
    WEB_PROP_MIN_WIDTH,
    WEB_PROP_MAX_WIDTH,
    WEB_PROP_MIN_HEIGHT,
    WEB_PROP_MAX_HEIGHT,
    WEB_PROP_MARGIN_AUTO_X,
    WEB_PROP_CENTER_X,
    WEB_PROP_BACKGROUND_COLOR,
    WEB_PROP_MARGIN_LEFT,
    WEB_PROP_MARGIN_RIGHT,
    WEB_PROP_MARGIN_TOP,
    WEB_PROP_MARGIN_BOTTOM,
    WEB_PROP_PADDING_LEFT,
    WEB_PROP_PADDING_RIGHT,
    WEB_PROP_PADDING_TOP,
    WEB_PROP_PADDING_BOTTOM,
    WEB_PROP_BORDER_LEFT,
    WEB_PROP_BORDER_RIGHT,
    WEB_PROP_BORDER_TOP,
    WEB_PROP_BORDER_BOTTOM,
    WEB_PROP_BORDER_COLOR,
    WEB_PROP_FLOAT,
    WEB_PROP_FONT_WEIGHT,
    WEB_PROP_FONT_STYLE,
    WEB_PROP_TEXT_DECORATION,
    WEB_PROP_FONT_SIZE,
    WEB_PROP_LINE_HEIGHT,
    WEB_PROP_TEXT_TRANSFORM,
    WEB_PROP_WHITE_SPACE,
    WEB_PROP_LIST_STYLE_TYPE,
    WEB_PROP_OVERFLOW_X,
    WEB_PROP_OVERFLOW_Y,
    WEB_PROP_BOX_SIZING,
    WEB_PROP_BORDER_COLLAPSE,
    WEB_PROP_BORDER_SPACING,
    WEB_PROP_TEXT_INDENT,
    WEB_PROP_CLEAR,
    WEB_PROP_VERTICAL_ALIGN,
    WEB_PROP_LIST_STYLE_POSITION,
    WEB_PROP_CAPTION_SIDE,
    WEB_PROP_DIRECTION,
    WEB_PROP_TABLE_LAYOUT,
    WEB_PROP_EMPTY_CELLS,
    WEB_PROP_OPACITY,
    WEB_PROP_Z_INDEX,
    WEB_PROP_LETTER_SPACING,
    WEB_PROP_WORD_SPACING,
    WEB_PROP_BACKGROUND_REPEAT,
    WEB_PROP_BACKGROUND_POSITION,
    WEB_PROP_COUNT
};

typedef struct {
    web_style_t style;
    uint32_t score[WEB_PROP_COUNT];
    uint32_t order;
} web_style_state_t;

#define WEB_STYLE_MAX_CACHED_RULES 1024u
#define WEB_CSS_MAX_NODES 8192u
#define WEB_CSS_MAX_STACK 96u
#define WEB_CSS_MAX_CLASSES 16u
#define WEB_CSS_TRACE_DETAIL_NODE_LIMIT 32u
#define WEB_CSS_TRACE_EARLY_NODES 2u
#define WEB_CSS_TRACE_PROGRESS_STRIDE 64u

typedef struct {
    uint32_t selector_start;
    uint32_t selector_end;
    uint32_t block_start;
    uint32_t block_end;
} web_cached_rule_t;

static const uint8_t *web_cached_html;
static uint32_t web_cached_viewport_width;
static uint32_t web_cached_viewport_height;
static uint32_t web_cached_rule_count;
static uint32_t web_cached_rule_saturated;
static unsigned long long web_css_select_total_ticks;
static unsigned long long web_css_apply_total_ticks;
static unsigned long long web_css_trace_total_ticks;
static uint32_t web_css_trace_lines_emitted;
static uint32_t web_css_trace_lines_suppressed;
static web_cached_rule_t web_cached_rules[WEB_STYLE_MAX_CACHED_RULES];

typedef struct {
    uint32_t tag_pos;
    int32_t parent;
    int32_t prev_sibling;
    int32_t next_sibling;
    uint32_t child_count;
    uint32_t text_child_count;
    uint32_t style_cached;
    uint32_t style_viewport_width;
    uint32_t style_viewport_height;
    web_style_t cached_style;
    lwc_string *name;
    lwc_string *id;
    lwc_string *classes[WEB_CSS_MAX_CLASSES];
    lwc_string *class_refs[WEB_CSS_MAX_CLASSES];
    uint32_t class_count;
    css_stylesheet *inline_style;
    void *libcss_node_data;
} web_css_node_t;

static css_stylesheet *web_css_sheet;
static css_stylesheet *web_css_ua_sheet;
static css_select_ctx *web_css_select_ctx;
static uint32_t web_css_ready;
static uint32_t web_css_rule_blocks;
static uint32_t web_css_node_count;
static uint32_t web_css_node_saturated;
static web_css_node_t web_css_nodes[WEB_CSS_MAX_NODES];

static void web_css_precompute_styles(const uint8_t *html,
                                      uint32_t viewport_width,
                                      uint32_t viewport_height);

static uint8_t web_lower(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (uint8_t)(ch + ('a' - 'A'));
    }
    return ch;
}

static int web_is_space(uint8_t ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

static int web_is_name_char(uint8_t ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch >= 128u;
}

static int web_is_digit(uint8_t ch) {
    return ch >= '0' && ch <= '9';
}

static uint32_t web_cstr_len(const char *text) {
    uint32_t len = 0;
    while (text[len] != '\0') {
        ++len;
    }
    return len;
}

static uint32_t web_cstrn_len_u8(const uint8_t *text, uint32_t capacity) {
    uint32_t len = 0;
    while (len < capacity && text[len] != 0) {
        ++len;
    }
    return len;
}

static uint32_t web_trim_start(const uint8_t *text, uint32_t start, uint32_t end) {
    while (start < end && web_is_space(text[start])) {
        ++start;
    }
    return start;
}

static uint32_t web_trim_end(const uint8_t *text, uint32_t start, uint32_t end) {
    (void)start;
    while (end > 0 && web_is_space(text[end - 1u])) {
        --end;
    }
    return end;
}

static int web_range_equal_ci(const uint8_t *a,
                              uint32_t a_start,
                              uint32_t a_end,
                              const uint8_t *b,
                              uint32_t b_start,
                              uint32_t b_end) {
    uint32_t a_len = a_end - a_start;
    uint32_t b_len = b_end - b_start;
    if (a_len != b_len) {
        return 0;
    }
    for (uint32_t i = 0; i < a_len; ++i) {
        if (web_lower(a[a_start + i]) != web_lower(b[b_start + i])) {
            return 0;
        }
    }
    return 1;
}

static int web_range_equal_cstr_ci(const uint8_t *text, uint32_t start, uint32_t end, const char *needle) {
    uint32_t len = web_cstr_len(needle);
    if (end - start != len) {
        return 0;
    }
    for (uint32_t i = 0; i < len; ++i) {
        if (web_lower(text[start + i]) != web_lower((uint8_t)needle[i])) {
            return 0;
        }
    }
    return 1;
}

static int web_range_starts_cstr_ci(const uint8_t *text, uint32_t start, uint32_t end, const char *needle) {
    uint32_t len = web_cstr_len(needle);
    if (end - start < len) {
        return 0;
    }
    for (uint32_t i = 0; i < len; ++i) {
        if (web_lower(text[start + i]) != web_lower((uint8_t)needle[i])) {
            return 0;
        }
    }
    return 1;
}

static int web_range_contains_cstr_ci(const uint8_t *text, uint32_t start, uint32_t end, const char *needle) {
    uint32_t len = web_cstr_len(needle);
    if (len == 0 || end - start < len) {
        return 0;
    }
    for (uint32_t i = start; i + len <= end; ++i) {
        uint32_t j = 0;
        while (j < len && web_lower(text[i + j]) == web_lower((uint8_t)needle[j])) {
            ++j;
        }
        if (j == len) {
            return 1;
        }
    }
    return 0;
}

static int web_range_contains_range_ci(const uint8_t *haystack,
                                       uint32_t hay_start,
                                       uint32_t hay_end,
                                       const uint8_t *needle,
                                       uint32_t needle_start,
                                       uint32_t needle_end) {
    uint32_t len = needle_end - needle_start;
    if (len == 0 || hay_end - hay_start < len) {
        return 0;
    }
    for (uint32_t i = hay_start; i + len <= hay_end; ++i) {
        if (web_range_equal_ci(haystack, i, i + len, needle, needle_start, needle_end)) {
            return 1;
        }
    }
    return 0;
}

static int web_range_ends_range_ci(const uint8_t *haystack,
                                   uint32_t hay_start,
                                   uint32_t hay_end,
                                   const uint8_t *needle,
                                   uint32_t needle_start,
                                   uint32_t needle_end) {
    uint32_t len = needle_end - needle_start;
    if (len == 0 || hay_end - hay_start < len) {
        return 0;
    }
    return web_range_equal_ci(haystack, hay_end - len, hay_end, needle, needle_start, needle_end);
}

static int web_tag_delim(uint8_t ch) {
    return ch == 0 || web_is_space(ch) || ch == '/' || ch == '>';
}

static int web_is_closing_tag(const uint8_t *html, uint32_t pos) {
    return html[pos] == '<' && html[pos + 1u] == '/';
}

static uint32_t web_skip_tag(const uint8_t *html, uint32_t pos) {
    while (html[pos] != 0 && html[pos] != '>') {
        ++pos;
    }
    if (html[pos] == '>') {
        ++pos;
    }
    return pos;
}

static int web_tag_name_range(const uint8_t *html, uint32_t pos, uint32_t *out_start, uint32_t *out_end) {
    if (html[pos] != '<') {
        return 0;
    }
    ++pos;
    if (html[pos] == '/') {
        ++pos;
    }
    if (!web_is_name_char(html[pos])) {
        return 0;
    }
    *out_start = pos;
    while (web_is_name_char(html[pos])) {
        ++pos;
    }
    *out_end = pos;
    return 1;
}

static int web_tag_name_is(const uint8_t *html, uint32_t pos, const char *name) {
    uint32_t start = 0;
    uint32_t end = 0;
    if (!web_tag_name_range(html, pos, &start, &end)) {
        return 0;
    }
    if (!web_tag_delim(html[end])) {
        return 0;
    }
    return web_range_equal_cstr_ci(html, start, end, name);
}

static int web_tag_name_matches_range(const uint8_t *html,
                                      uint32_t tag_pos,
                                      const uint8_t *name,
                                      uint32_t name_start,
                                      uint32_t name_end) {
    uint32_t tag_start = 0;
    uint32_t tag_end = 0;
    if (!web_tag_name_range(html, tag_pos, &tag_start, &tag_end)) {
        return 0;
    }
    return web_range_equal_ci(html, tag_start, tag_end, name, name_start, name_end);
}

static int web_attr_value_range_name(const uint8_t *html,
                                     uint32_t tag_pos,
                                     const uint8_t *name,
                                     uint32_t name_start,
                                     uint32_t name_end,
                                     uint32_t *out_start,
                                     uint32_t *out_end) {
    uint32_t pos = tag_pos;
    if (html[pos] != '<' || html[pos + 1u] == '/') {
        return 0;
    }
    ++pos;
    while (html[pos] != 0 && html[pos] != '>' && !web_is_space(html[pos]) && html[pos] != '/') {
        ++pos;
    }
    while (html[pos] != 0 && html[pos] != '>') {
        while (web_is_space(html[pos])) {
            ++pos;
        }
        if (html[pos] == '/' || html[pos] == '>') {
            if (html[pos] == '/') {
                ++pos;
                continue;
            }
            break;
        }
        uint32_t attr_start = pos;
        while (html[pos] != 0 && html[pos] != '>' && html[pos] != '=' && !web_is_space(html[pos]) && html[pos] != '/') {
            ++pos;
        }
        uint32_t attr_end = pos;
        while (web_is_space(html[pos])) {
            ++pos;
        }
        uint32_t value_start = pos;
        uint32_t value_end = pos;
        if (html[pos] == '=') {
            ++pos;
            while (web_is_space(html[pos])) {
                ++pos;
            }
            uint8_t quote = 0;
            if (html[pos] == '"' || html[pos] == '\'') {
                quote = html[pos];
                ++pos;
            }
            value_start = pos;
            if (quote != 0) {
                while (html[pos] != 0 && html[pos] != quote) {
                    ++pos;
                }
                value_end = pos;
                if (html[pos] == quote) {
                    ++pos;
                }
            } else {
                while (html[pos] != 0 && html[pos] != '>' && !web_is_space(html[pos])) {
                    ++pos;
                }
                value_end = pos;
            }
        }
        if (web_range_equal_ci(html, attr_start, attr_end, name, name_start, name_end)) {
            *out_start = value_start;
            *out_end = value_end;
            return 1;
        }
    }
    return 0;
}

static int web_attr_value_range_cstr(const uint8_t *html,
                                     uint32_t tag_pos,
                                     const char *name,
                                     uint32_t *out_start,
                                     uint32_t *out_end) {
    const uint8_t *name_u8 = (const uint8_t *)name;
    return web_attr_value_range_name(html, tag_pos, name_u8, 0, web_cstr_len(name), out_start, out_end);
}

static int web_attr_token_contains_range(const uint8_t *html,
                                         uint32_t tag_pos,
                                         const char *attr,
                                         const uint8_t *token,
                                         uint32_t token_start,
                                         uint32_t token_end) {
    uint32_t start = 0;
    uint32_t end = 0;
    if (!web_attr_value_range_cstr(html, tag_pos, attr, &start, &end)) {
        return 0;
    }
    uint32_t pos = start;
    while (pos < end) {
        while (pos < end && web_is_space(html[pos])) {
            ++pos;
        }
        uint32_t part_start = pos;
        while (pos < end && !web_is_space(html[pos])) {
            ++pos;
        }
        if (part_start < pos && web_range_equal_ci(html, part_start, pos, token, token_start, token_end)) {
            return 1;
        }
    }
    return 0;
}

static int web_attr_value_exact_range_ci(const uint8_t *html,
                                         uint32_t tag_pos,
                                         const char *attr,
                                         const uint8_t *value,
                                         uint32_t value_start,
                                         uint32_t value_end) {
    uint32_t start = 0;
    uint32_t end = 0;
    if (!web_attr_value_range_cstr(html, tag_pos, attr, &start, &end)) {
        return 0;
    }
    start = web_trim_start(html, start, end);
    end = web_trim_end(html, start, end);
    return web_range_equal_ci(html, start, end, value, value_start, value_end);
}

static css_select_handler web_css_select_handler;

static int web_css_qname_is_universal(const css_qname *qname) {
    return qname != NULL &&
           qname->name != NULL &&
           lwc_string_length(qname->name) == 1u &&
           lwc_string_data(qname->name)[0] == '*';
}

static int web_css_lwc_equal_ci(lwc_string *a, lwc_string *b) {
    bool match = false;
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (lwc_string_caseless_isequal(a, b, &match) != lwc_error_ok) {
        return 0;
    }
    return match ? 1 : 0;
}

static web_css_node_t *web_css_node(void *node) {
    return (web_css_node_t *)node;
}

static int web_css_trace_step_equals(const char *step, const char *expected) {
    uint32_t i = 0;

    if (step == NULL || expected == NULL) {
        return 0;
    }
    while (step[i] != 0 && expected[i] != 0) {
        if (step[i] != expected[i]) {
            return 0;
        }
        ++i;
    }
    return step[i] == 0 && expected[i] == 0;
}

static int web_css_trace_detailed_enabled(void) {
    return web_css_node_count <= WEB_CSS_TRACE_DETAIL_NODE_LIMIT;
}

static int web_css_trace_step_enabled(const web_css_node_t *node, const char *step) {
    uint32_t index;

    if (node == NULL || step == NULL) {
        return 1;
    }

    index = (uint32_t)(node - web_css_nodes);
    if (web_css_trace_step_equals(step, "select-fail") ||
        web_css_trace_step_equals(step, "inline-style-fail")) {
        return 1;
    }
    if (web_css_trace_step_equals(step, "precompute-node")) {
        if (index < WEB_CSS_TRACE_EARLY_NODES || index + 1u >= web_css_node_count) {
            return 1;
        }
        return WEB_CSS_TRACE_PROGRESS_STRIDE != 0u &&
               (index % WEB_CSS_TRACE_PROGRESS_STRIDE) == 0u;
    }
    if (web_css_trace_detailed_enabled() &&
        (index < WEB_CSS_TRACE_EARLY_NODES || index + 1u >= web_css_node_count)) {
        return 1;
    }

    return 0;
}

static void web_css_trace_reset_stats(void) {
    web_css_trace_total_ticks = 0;
    web_css_trace_lines_emitted = 0;
    web_css_trace_lines_suppressed = 0;
}

static void web_css_trace_node_step(const web_css_node_t *node, const char *step) {
    char name_buf[32];
    uint32_t len = 0;
    unsigned long long started;

    if (!web_css_trace_step_enabled(node, step)) {
        ++web_css_trace_lines_suppressed;
        return;
    }

    started = timer_ticks();
    console_puts("css-trace ");
    console_puts(step);
    console_puts(" node=");
    if (node == NULL) {
        console_puts("null\n");
        ++web_css_trace_lines_emitted;
        web_css_trace_total_ticks += timer_ticks() - started;
        return;
    }

    console_put_dec64((unsigned long long)(node - web_css_nodes));
    console_puts(" tag=");
    if (node->name != NULL) {
        len = (uint32_t)lwc_string_length(node->name);
        if (len >= sizeof(name_buf)) {
            len = sizeof(name_buf) - 1u;
        }
        memcpy(name_buf, lwc_string_data(node->name), len);
    }
    name_buf[len] = 0;
    console_puts(name_buf);
    console_puts(" pos=");
    console_put_dec64(node->tag_pos);
    console_puts("\n");
    ++web_css_trace_lines_emitted;
    web_css_trace_total_ticks += timer_ticks() - started;
}

static css_error web_css_resolve_url(void *pw,
                                     const char *base,
                                     lwc_string *rel,
                                     lwc_string **abs) {
    (void)pw;
    (void)base;
    if (rel == NULL || abs == NULL) {
        return CSS_BADPARM;
    }
    *abs = lwc_string_ref(rel);
    return CSS_OK;
}

static css_error web_css_node_name(void *pw, void *node, css_qname *qname) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (n == NULL || qname == NULL || n->name == NULL) {
        return CSS_BADPARM;
    }
    qname->ns = NULL;
    qname->name = lwc_string_ref(n->name);
    return CSS_OK;
}

static css_error web_css_node_classes(void *pw,
                                      void *node,
                                      lwc_string ***classes,
                                      uint32_t *n_classes) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (classes == NULL || n_classes == NULL) {
        return CSS_BADPARM;
    }
    *classes = NULL;
    *n_classes = 0;
    if (n == NULL || n->class_count == 0) {
        return CSS_OK;
    }
    for (uint32_t i = 0; i < n->class_count; ++i) {
        n->class_refs[i] = lwc_string_ref(n->classes[i]);
    }
    *classes = n->class_refs;
    *n_classes = n->class_count;
    return CSS_OK;
}

static css_error web_css_node_id(void *pw, void *node, lwc_string **id) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (id == NULL) {
        return CSS_BADPARM;
    }
    *id = NULL;
    if (n != NULL && n->id != NULL) {
        *id = lwc_string_ref(n->id);
    }
    return CSS_OK;
}

static int web_css_node_has_qname(web_css_node_t *node, const css_qname *qname) {
    if (node == NULL || qname == NULL) {
        return 0;
    }
    if (web_css_qname_is_universal(qname)) {
        return 1;
    }
    return web_css_lwc_equal_ci(node->name, qname->name);
}

static css_error web_css_named_ancestor_node(void *pw,
                                             void *node,
                                             const css_qname *qname,
                                             void **ancestor) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (ancestor == NULL) {
        return CSS_BADPARM;
    }
    *ancestor = NULL;
    while (n != NULL && n->parent >= 0) {
        n = &web_css_nodes[(uint32_t)n->parent];
        if (web_css_node_has_qname(n, qname)) {
            *ancestor = n;
            return CSS_OK;
        }
    }
    return CSS_OK;
}

static css_error web_css_named_parent_node(void *pw,
                                           void *node,
                                           const css_qname *qname,
                                           void **parent) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (parent == NULL) {
        return CSS_BADPARM;
    }
    *parent = NULL;
    if (n != NULL && n->parent >= 0) {
        web_css_node_t *p = &web_css_nodes[(uint32_t)n->parent];
        if (web_css_node_has_qname(p, qname)) {
            *parent = p;
        }
    }
    return CSS_OK;
}

static css_error web_css_named_sibling_node(void *pw,
                                            void *node,
                                            const css_qname *qname,
                                            void **sibling) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (sibling == NULL) {
        return CSS_BADPARM;
    }
    *sibling = NULL;
    if (n != NULL && n->prev_sibling >= 0) {
        web_css_node_t *s = &web_css_nodes[(uint32_t)n->prev_sibling];
        if (web_css_node_has_qname(s, qname)) {
            *sibling = s;
        }
    }
    return CSS_OK;
}

static css_error web_css_named_generic_sibling_node(void *pw,
                                                    void *node,
                                                    const css_qname *qname,
                                                    void **sibling) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (sibling == NULL) {
        return CSS_BADPARM;
    }
    *sibling = NULL;
    while (n != NULL && n->prev_sibling >= 0) {
        n = &web_css_nodes[(uint32_t)n->prev_sibling];
        if (web_css_node_has_qname(n, qname)) {
            *sibling = n;
            return CSS_OK;
        }
    }
    return CSS_OK;
}

static css_error web_css_parent_node(void *pw, void *node, void **parent) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (parent == NULL) {
        return CSS_BADPARM;
    }
    *parent = NULL;
    if (n != NULL && n->parent >= 0) {
        *parent = &web_css_nodes[(uint32_t)n->parent];
    }
    return CSS_OK;
}

static css_error web_css_sibling_node(void *pw, void *node, void **sibling) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (sibling == NULL) {
        return CSS_BADPARM;
    }
    *sibling = NULL;
    if (n != NULL && n->prev_sibling >= 0) {
        *sibling = &web_css_nodes[(uint32_t)n->prev_sibling];
    }
    return CSS_OK;
}

static css_error web_css_node_has_name(void *pw,
                                       void *node,
                                       const css_qname *qname,
                                       bool *match) {
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = web_css_node_has_qname(web_css_node(node), qname) != 0;
    return CSS_OK;
}

static css_error web_css_node_has_class(void *pw,
                                        void *node,
                                        lwc_string *name,
                                        bool *match) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = false;
    if (n == NULL || name == NULL) {
        return CSS_OK;
    }
    for (uint32_t i = 0; i < n->class_count; ++i) {
        if (web_css_lwc_equal_ci(n->classes[i], name)) {
            *match = true;
            return CSS_OK;
        }
    }
    return CSS_OK;
}

static css_error web_css_node_has_id(void *pw,
                                     void *node,
                                     lwc_string *name,
                                     bool *match) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = n != NULL && web_css_lwc_equal_ci(n->id, name) != 0;
    return CSS_OK;
}

static int web_css_attr_value_range(web_css_node_t *node,
                                    const css_qname *qname,
                                    uint32_t *start,
                                    uint32_t *end) {
    if (node == NULL || qname == NULL || qname->name == NULL) {
        return 0;
    }
    return web_attr_value_range_name(web_cached_html,
                                     node->tag_pos,
                                     (const uint8_t *)lwc_string_data(qname->name),
                                     0,
                                     (uint32_t)lwc_string_length(qname->name),
                                     start,
                                     end);
}

static css_error web_css_node_has_attribute(void *pw,
                                            void *node,
                                            const css_qname *qname,
                                            bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = web_css_attr_value_range(web_css_node(node), qname, &start, &end) != 0;
    return CSS_OK;
}

static int web_css_lwc_value_equal_ci(const uint8_t *html,
                                      uint32_t start,
                                      uint32_t end,
                                      lwc_string *value) {
    if (value == NULL) {
        return 0;
    }
    return web_range_equal_ci(html,
                              start,
                              end,
                              (const uint8_t *)lwc_string_data(value),
                              0,
                              (uint32_t)lwc_string_length(value));
}

static css_error web_css_node_has_attribute_equal(void *pw,
                                                  void *node,
                                                  const css_qname *qname,
                                                  lwc_string *value,
                                                  bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = false;
    if (web_css_attr_value_range(web_css_node(node), qname, &start, &end)) {
        start = web_trim_start(web_cached_html, start, end);
        end = web_trim_end(web_cached_html, start, end);
        *match = web_css_lwc_value_equal_ci(web_cached_html, start, end, value) != 0;
    }
    return CSS_OK;
}

static css_error web_css_node_has_attribute_dashmatch(void *pw,
                                                      void *node,
                                                      const css_qname *qname,
                                                      lwc_string *value,
                                                      bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t len;
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = false;
    if (value == NULL || !web_css_attr_value_range(web_css_node(node), qname, &start, &end)) {
        return CSS_OK;
    }
    start = web_trim_start(web_cached_html, start, end);
    end = web_trim_end(web_cached_html, start, end);
    len = (uint32_t)lwc_string_length(value);
    if (web_css_lwc_value_equal_ci(web_cached_html, start, end, value) ||
        (end > start + len &&
         web_cached_html[start + len] == '-' &&
         web_range_equal_ci(web_cached_html,
                            start,
                            start + len,
                            (const uint8_t *)lwc_string_data(value),
                            0,
                            len))) {
        *match = true;
    }
    return CSS_OK;
}

static css_error web_css_node_has_attribute_includes(void *pw,
                                                     void *node,
                                                     const css_qname *qname,
                                                     lwc_string *value,
                                                     bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = false;
    if (value == NULL || !web_css_attr_value_range(web_css_node(node), qname, &start, &end)) {
        return CSS_OK;
    }
    uint32_t pos = start;
    while (pos < end) {
        while (pos < end && web_is_space(web_cached_html[pos])) {
            ++pos;
        }
        uint32_t part_start = pos;
        while (pos < end && !web_is_space(web_cached_html[pos])) {
            ++pos;
        }
        if (part_start < pos && web_css_lwc_value_equal_ci(web_cached_html, part_start, pos, value)) {
            *match = true;
            return CSS_OK;
        }
    }
    return CSS_OK;
}

static css_error web_css_node_has_attribute_prefix(void *pw,
                                                   void *node,
                                                   const css_qname *qname,
                                                   lwc_string *value,
                                                   bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t len;
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = false;
    if (value == NULL || !web_css_attr_value_range(web_css_node(node), qname, &start, &end)) {
        return CSS_OK;
    }
    len = (uint32_t)lwc_string_length(value);
    if (end - start >= len &&
        web_range_equal_ci(web_cached_html, start, start + len, (const uint8_t *)lwc_string_data(value), 0, len)) {
        *match = true;
    }
    return CSS_OK;
}

static css_error web_css_node_has_attribute_suffix(void *pw,
                                                   void *node,
                                                   const css_qname *qname,
                                                   lwc_string *value,
                                                   bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t len;
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = false;
    if (value == NULL || !web_css_attr_value_range(web_css_node(node), qname, &start, &end)) {
        return CSS_OK;
    }
    len = (uint32_t)lwc_string_length(value);
    if (end - start >= len &&
        web_range_equal_ci(web_cached_html, end - len, end, (const uint8_t *)lwc_string_data(value), 0, len)) {
        *match = true;
    }
    return CSS_OK;
}

static css_error web_css_node_has_attribute_substring(void *pw,
                                                      void *node,
                                                      const css_qname *qname,
                                                      lwc_string *value,
                                                      bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t len;
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = false;
    if (value == NULL || !web_css_attr_value_range(web_css_node(node), qname, &start, &end)) {
        return CSS_OK;
    }
    len = (uint32_t)lwc_string_length(value);
    if (len != 0 &&
        web_range_contains_range_ci(web_cached_html,
                                    start,
                                    end,
                                    (const uint8_t *)lwc_string_data(value),
                                    0,
                                    len)) {
        *match = true;
    }
    return CSS_OK;
}

static css_error web_css_node_is_root(void *pw, void *node, bool *match) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = n != NULL && n->parent < 0;
    return CSS_OK;
}

static css_error web_css_node_count_siblings(void *pw,
                                             void *node,
                                             bool same_name,
                                             bool after,
                                             int32_t *count) {
    web_css_node_t *n = web_css_node(node);
    web_css_node_t *start = n;
    int32_t out = 0;
    (void)pw;
    if (count == NULL) {
        return CSS_BADPARM;
    }
    if (n == NULL) {
        *count = 0;
        return CSS_OK;
    }
    if (after) {
        while (n->next_sibling >= 0) {
            n = &web_css_nodes[(uint32_t)n->next_sibling];
            if (!same_name || web_css_lwc_equal_ci(n->name, start->name)) {
                ++out;
            }
        }
    } else {
        while (n->prev_sibling >= 0) {
            n = &web_css_nodes[(uint32_t)n->prev_sibling];
            if (!same_name || web_css_lwc_equal_ci(n->name, start->name)) {
                ++out;
            }
        }
    }
    *count = out;
    return CSS_OK;
}

static css_error web_css_node_is_empty(void *pw, void *node, bool *match) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = n != NULL && n->child_count == 0 && n->text_child_count == 0;
    return CSS_OK;
}

static css_error web_css_node_is_link(void *pw, void *node, bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = n != NULL &&
             web_attr_value_range_cstr(web_cached_html, n->tag_pos, "href", &start, &end) != 0;
    return CSS_OK;
}

static css_error web_css_node_false(void *pw, void *node, bool *match) {
    (void)pw;
    (void)node;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = false;
    return CSS_OK;
}

static css_error web_css_node_is_enabled(void *pw, void *node, bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = n == NULL ||
             web_attr_value_range_cstr(web_cached_html, n->tag_pos, "disabled", &start, &end) == 0;
    return CSS_OK;
}

static css_error web_css_node_is_disabled(void *pw, void *node, bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = n != NULL &&
             web_attr_value_range_cstr(web_cached_html, n->tag_pos, "disabled", &start, &end) != 0;
    return CSS_OK;
}

static css_error web_css_node_is_checked(void *pw, void *node, bool *match) {
    uint32_t start = 0;
    uint32_t end = 0;
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = n != NULL &&
             web_attr_value_range_cstr(web_cached_html, n->tag_pos, "checked", &start, &end) != 0;
    return CSS_OK;
}

static css_error web_css_node_is_lang(void *pw, void *node, lwc_string *lang, bool *match) {
    (void)pw;
    (void)node;
    (void)lang;
    if (match == NULL) {
        return CSS_BADPARM;
    }
    *match = false;
    return CSS_OK;
}

static css_error web_css_node_presentational_hint(void *pw,
                                                  void *node,
                                                  uint32_t *nhints,
                                                  css_hint **hints) {
    (void)pw;
    (void)node;
    if (nhints == NULL || hints == NULL) {
        return CSS_BADPARM;
    }
    *nhints = 0;
    *hints = NULL;
    return CSS_OK;
}

static css_error web_css_ua_default_for_property(void *pw, uint32_t property, css_hint *hint) {
    (void)pw;
    if (hint == NULL) {
        return CSS_BADPARM;
    }
    if (property == CSS_PROP_COLOR) {
        hint->data.color = 0xff000000u;
        hint->status = CSS_COLOR_COLOR;
        return CSS_OK;
    }
    if (property == CSS_PROP_FONT_FAMILY) {
        hint->data.strings = NULL;
        hint->status = CSS_FONT_FAMILY_SANS_SERIF;
        return CSS_OK;
    }
    if (property == CSS_PROP_QUOTES) {
        hint->data.strings = NULL;
        hint->status = CSS_QUOTES_NONE;
        return CSS_OK;
    }
    if (property == CSS_PROP_VOICE_FAMILY) {
        hint->data.strings = NULL;
        hint->status = 0;
        return CSS_OK;
    }
    return CSS_INVALID;
}

static css_error web_css_set_node_data(void *pw, void *node, void *libcss_node_data) {
    web_css_node_t *n = web_css_node(node);
    if (n == NULL) {
        return CSS_BADPARM;
    }
    if (n->libcss_node_data != NULL && n->libcss_node_data != libcss_node_data) {
        css_libcss_node_data_handler(&web_css_select_handler,
                                     CSS_NODE_DELETED,
                                     pw,
                                     node,
                                     NULL,
                                     n->libcss_node_data);
    }
    n->libcss_node_data = libcss_node_data;
    return CSS_OK;
}

static css_error web_css_get_node_data(void *pw, void *node, void **libcss_node_data) {
    web_css_node_t *n = web_css_node(node);
    (void)pw;
    if (libcss_node_data == NULL) {
        return CSS_BADPARM;
    }
    *libcss_node_data = n != NULL ? n->libcss_node_data : NULL;
    return CSS_OK;
}

static css_select_handler web_css_select_handler = {
    CSS_SELECT_HANDLER_VERSION_1,
    web_css_node_name,
    web_css_node_classes,
    web_css_node_id,
    web_css_named_ancestor_node,
    web_css_named_parent_node,
    web_css_named_sibling_node,
    web_css_named_generic_sibling_node,
    web_css_parent_node,
    web_css_sibling_node,
    web_css_node_has_name,
    web_css_node_has_class,
    web_css_node_has_id,
    web_css_node_has_attribute,
    web_css_node_has_attribute_equal,
    web_css_node_has_attribute_dashmatch,
    web_css_node_has_attribute_includes,
    web_css_node_has_attribute_prefix,
    web_css_node_has_attribute_suffix,
    web_css_node_has_attribute_substring,
    web_css_node_is_root,
    web_css_node_count_siblings,
    web_css_node_is_empty,
    web_css_node_is_link,
    web_css_node_false,
    web_css_node_false,
    web_css_node_false,
    web_css_node_false,
    web_css_node_is_enabled,
    web_css_node_is_disabled,
    web_css_node_is_checked,
    web_css_node_false,
    web_css_node_is_lang,
    web_css_node_presentational_hint,
    web_css_ua_default_for_property,
    web_css_set_node_data,
    web_css_get_node_data,
};

static uint32_t web_cascade_score(uint32_t specificity, int important, uint32_t order) {
    if (specificity > 65535u) {
        specificity = 65535u;
    }
    if (order > 32767u) {
        order = 32767u;
    }
    return (important ? 0x80000000u : 0u) | (specificity << 15) | order;
}

static int web_cascade_allows(web_style_state_t *state, uint32_t property, uint32_t specificity, int important) {
    uint32_t score = web_cascade_score(specificity, important, state->order);
    if (score >= state->score[property]) {
        state->score[property] = score;
        return 1;
    }
    return 0;
}

static void web_set_display(web_style_state_t *state, const uint8_t *value, uint32_t start, uint32_t end, uint32_t specificity, int important) {
    if (!web_cascade_allows(state, WEB_PROP_DISPLAY, specificity, important)) {
        return;
    }
    state->style.display = WEB_STYLE_DISPLAY_INLINE;
    state->style.flags &= ~(WEB_STYLE_FLAG_DISPLAY_NONE |
                            WEB_STYLE_FLAG_DISPLAY_FLEX |
                            WEB_STYLE_FLAG_DISPLAY_TABLE |
                            WEB_STYLE_FLAG_DISPLAY_TABLE_ROW |
                            WEB_STYLE_FLAG_DISPLAY_TABLE_CELL);
    if (web_range_contains_cstr_ci(value, start, end, "none")) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_NONE;
    } else if (web_range_contains_cstr_ci(value, start, end, "flex")) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_FLEX;
        state->style.display = WEB_STYLE_DISPLAY_FLEX;
    } else if (web_range_contains_cstr_ci(value, start, end, "table-cell")) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_TABLE_CELL;
        state->style.display = WEB_STYLE_DISPLAY_TABLE_CELL;
    } else if (web_range_contains_cstr_ci(value, start, end, "table-row")) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_TABLE_ROW;
        state->style.display = WEB_STYLE_DISPLAY_TABLE_ROW;
    } else if (web_range_contains_cstr_ci(value, start, end, "table")) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_TABLE;
        state->style.display = WEB_STYLE_DISPLAY_TABLE;
    } else if (web_range_contains_cstr_ci(value, start, end, "list-item")) {
        state->style.display = WEB_STYLE_DISPLAY_LIST_ITEM;
    } else if (web_range_contains_cstr_ci(value, start, end, "block")) {
        state->style.display = WEB_STYLE_DISPLAY_BLOCK;
    }
}

static void web_set_display_hint(web_style_state_t *state, uint32_t display, uint32_t specificity, int important) {
    if (!web_cascade_allows(state, WEB_PROP_DISPLAY, specificity, important)) {
        return;
    }
    state->style.display = WEB_STYLE_DISPLAY_INLINE;
    state->style.flags &= ~(WEB_STYLE_FLAG_DISPLAY_NONE |
                            WEB_STYLE_FLAG_DISPLAY_FLEX |
                            WEB_STYLE_FLAG_DISPLAY_TABLE |
                            WEB_STYLE_FLAG_DISPLAY_TABLE_ROW |
                            WEB_STYLE_FLAG_DISPLAY_TABLE_CELL);
    if (display == NETSURF_PORT_HINT_DISPLAY_BLOCK) {
        state->style.display = WEB_STYLE_DISPLAY_BLOCK;
    } else if (display == NETSURF_PORT_HINT_DISPLAY_FLEX) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_FLEX;
        state->style.display = WEB_STYLE_DISPLAY_FLEX;
    } else if (display == NETSURF_PORT_HINT_DISPLAY_TABLE) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_TABLE;
        state->style.display = WEB_STYLE_DISPLAY_TABLE;
    } else if (display == NETSURF_PORT_HINT_DISPLAY_TABLE_ROW) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_TABLE_ROW;
        state->style.display = WEB_STYLE_DISPLAY_TABLE_ROW;
    } else if (display == NETSURF_PORT_HINT_DISPLAY_TABLE_CELL) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_TABLE_CELL;
        state->style.display = WEB_STYLE_DISPLAY_TABLE_CELL;
    } else if (display == NETSURF_PORT_HINT_DISPLAY_LIST_ITEM) {
        state->style.display = WEB_STYLE_DISPLAY_LIST_ITEM;
    }
}

static void web_set_float(web_style_state_t *state, const uint8_t *value, uint32_t start, uint32_t end, uint32_t specificity, int important) {
    if (!web_cascade_allows(state, WEB_PROP_FLOAT, specificity, important)) {
        return;
    }
    state->style.flags &= ~(WEB_STYLE_FLAG_FLOAT_LEFT | WEB_STYLE_FLAG_FLOAT_RIGHT);
    state->style.float_side = WEB_STYLE_FLOAT_NONE;
    if (web_range_contains_cstr_ci(value, start, end, "left")) {
        state->style.flags |= WEB_STYLE_FLAG_FLOAT_LEFT;
        state->style.float_side = WEB_STYLE_FLOAT_LEFT;
    } else if (web_range_contains_cstr_ci(value, start, end, "right")) {
        state->style.flags |= WEB_STYLE_FLAG_FLOAT_RIGHT;
        state->style.float_side = WEB_STYLE_FLOAT_RIGHT;
    }
}

static void web_set_visibility(web_style_state_t *state, const uint8_t *value, uint32_t start, uint32_t end, uint32_t specificity, int important) {
    if (!web_cascade_allows(state, WEB_PROP_VISIBILITY, specificity, important)) {
        return;
    }
    state->style.flags &= ~WEB_STYLE_FLAG_VISIBILITY_HIDDEN;
    if (web_range_contains_cstr_ci(value, start, end, "hidden") ||
        web_range_contains_cstr_ci(value, start, end, "collapse")) {
        state->style.flags |= WEB_STYLE_FLAG_VISIBILITY_HIDDEN;
    }
}

static void web_set_text_align(web_style_state_t *state, uint32_t align, uint32_t specificity, int important) {
    if (!web_cascade_allows(state, WEB_PROP_TEXT_ALIGN, specificity, important)) {
        return;
    }
    state->style.flags |= WEB_STYLE_FLAG_HAS_TEXT_ALIGN;
    state->style.text_align = align;
}

static int web_hex_value(uint8_t ch) {
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0');
    }
    if (ch >= 'A' && ch <= 'F') {
        return (int)(ch - 'A' + 10);
    }
    if (ch >= 'a' && ch <= 'f') {
        return (int)(ch - 'a' + 10);
    }
    return -1;
}

static int web_parse_hex_color(const uint8_t *text, uint32_t start, uint32_t end, uint32_t *out_color) {
    if (start >= end || text[start] != '#') {
        return 0;
    }
    if (start + 3u >= end) {
        return 0;
    }
    int r1 = web_hex_value(text[start + 1u]);
    int g1 = web_hex_value(text[start + 2u]);
    int b1 = web_hex_value(text[start + 3u]);
    if (r1 < 0 || g1 < 0 || b1 < 0) {
        return 0;
    }
    if (start + 6u < end) {
        int r2 = web_hex_value(text[start + 2u]);
        int g2 = web_hex_value(text[start + 4u]);
        int b2 = web_hex_value(text[start + 6u]);
        int g1_full = web_hex_value(text[start + 3u]);
        int b1_full = web_hex_value(text[start + 5u]);
        if (r2 >= 0 && g1_full >= 0 && g2 >= 0 && b1_full >= 0 && b2 >= 0) {
            *out_color = (uint32_t)(((r1 * 16 + r2) << 16) |
                                    ((g1_full * 16 + g2) << 8) |
                                    (b1_full * 16 + b2));
            return 1;
        }
    }
    *out_color = (uint32_t)(((r1 * 17) << 16) | ((g1 * 17) << 8) | (b1 * 17));
    return 1;
}

static void web_skip_color_separators(const uint8_t *text, uint32_t *pos, uint32_t end) {
    while (*pos < end && (web_is_space(text[*pos]) || text[*pos] == ',')) {
        ++*pos;
    }
}

static int web_parse_color_component(const uint8_t *text, uint32_t *pos, uint32_t end, uint32_t *out_value) {
    web_skip_color_separators(text, pos, end);
    if (*pos >= end || !web_is_digit(text[*pos])) {
        return 0;
    }
    uint32_t value = 0;
    while (*pos < end && web_is_digit(text[*pos])) {
        if (value < 100000u) {
            value = value * 10u + (uint32_t)(text[*pos] - '0');
        }
        ++*pos;
    }
    if (*pos < end && text[*pos] == '%') {
        value = (value * 255u) / 100u;
        ++*pos;
    }
    if (value > 255u) {
        value = 255u;
    }
    *out_value = value;
    return 1;
}

static int web_parse_rgb_color(const uint8_t *text, uint32_t start, uint32_t end, uint32_t *out_color) {
    if (!web_range_starts_cstr_ci(text, start, end, "rgb(") &&
        !web_range_starts_cstr_ci(text, start, end, "rgba(")) {
        return 0;
    }
    uint32_t pos = start;
    while (pos < end && text[pos] != '(') {
        ++pos;
    }
    if (pos >= end || text[pos] != '(') {
        return 0;
    }
    ++pos;
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    if (!web_parse_color_component(text, &pos, end, &r) ||
        !web_parse_color_component(text, &pos, end, &g) ||
        !web_parse_color_component(text, &pos, end, &b)) {
        return 0;
    }
    *out_color = (r << 16) | (g << 8) | b;
    return 1;
}

static int web_named_color(const uint8_t *text,
                           uint32_t start,
                           uint32_t end,
                           const char *name,
                           uint32_t color,
                           uint32_t *out_color) {
    if (web_range_equal_cstr_ci(text, start, end, name)) {
        *out_color = color;
        return 1;
    }
    return 0;
}

static int web_parse_named_color(const uint8_t *text, uint32_t start, uint32_t end, uint32_t *out_color) {
    return web_named_color(text, start, end, "black", 0x000000u, out_color) ||
           web_named_color(text, start, end, "white", 0xffffffu, out_color) ||
           web_named_color(text, start, end, "red", 0xff0000u, out_color) ||
           web_named_color(text, start, end, "green", 0x008000u, out_color) ||
           web_named_color(text, start, end, "blue", 0x0000ffu, out_color) ||
           web_named_color(text, start, end, "gray", 0x808080u, out_color) ||
           web_named_color(text, start, end, "grey", 0x808080u, out_color) ||
           web_named_color(text, start, end, "silver", 0xc0c0c0u, out_color) ||
           web_named_color(text, start, end, "maroon", 0x800000u, out_color) ||
           web_named_color(text, start, end, "purple", 0x800080u, out_color) ||
           web_named_color(text, start, end, "fuchsia", 0xff00ffu, out_color) ||
           web_named_color(text, start, end, "lime", 0x00ff00u, out_color) ||
           web_named_color(text, start, end, "olive", 0x808000u, out_color) ||
           web_named_color(text, start, end, "yellow", 0xffff00u, out_color) ||
           web_named_color(text, start, end, "navy", 0x000080u, out_color) ||
           web_named_color(text, start, end, "teal", 0x008080u, out_color) ||
           web_named_color(text, start, end, "aqua", 0x00ffffu, out_color) ||
           web_named_color(text, start, end, "orange", 0xffa500u, out_color);
}

static int web_parse_color_value(const uint8_t *text, uint32_t start, uint32_t end, uint32_t *out_color) {
    start = web_trim_start(text, start, end);
    end = web_trim_end(text, start, end);
    if (start >= end) {
        return 0;
    }
    uint32_t token_end = start;
    while (token_end < end && !web_is_space(text[token_end]) && text[token_end] != '!') {
        ++token_end;
    }
    if (text[start] == '#') {
        return web_parse_hex_color(text, start, end, out_color);
    }
    if (web_parse_rgb_color(text, start, end, out_color)) {
        return 1;
    }
    return web_parse_named_color(text, start, token_end, out_color);
}

static void web_set_color(web_style_state_t *state,
                          uint32_t color,
                          uint32_t specificity,
                          int important) {
    if (!web_cascade_allows(state, WEB_PROP_COLOR, specificity, important)) {
        return;
    }
    state->style.flags |= WEB_STYLE_FLAG_HAS_COLOR;
    state->style.color = color;
}

static void web_set_position(web_style_state_t *state, const uint8_t *value, uint32_t start, uint32_t end, uint32_t specificity, int important) {
    if (!web_cascade_allows(state, WEB_PROP_POSITION, specificity, important)) {
        return;
    }
    state->style.flags &= ~WEB_STYLE_FLAG_HAS_POSITION;
    state->style.position = WEB_STYLE_POS_STATIC;
    if (web_range_contains_cstr_ci(value, start, end, "absolute")) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_POSITION;
        state->style.position = WEB_STYLE_POS_ABSOLUTE;
    } else if (web_range_contains_cstr_ci(value, start, end, "fixed")) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_POSITION;
        state->style.position = WEB_STYLE_POS_FIXED;
    }
}

static void web_set_flag_property(web_style_state_t *state,
                                  uint32_t property,
                                  uint32_t flag,
                                  int enabled,
                                  uint32_t specificity,
                                  int important) {
    if (!web_cascade_allows(state, property, specificity, important)) {
        return;
    }
    if (enabled) {
        state->style.flags |= flag;
    } else {
        state->style.flags &= ~flag;
    }
}

static void web_set_length_property(web_style_state_t *state,
                                    uint32_t property,
                                    uint32_t flag,
                                    uint32_t *field,
                                    uint32_t value,
                                    uint32_t specificity,
                                    int important) {
    if (!web_cascade_allows(state, property, specificity, important)) {
        return;
    }
    state->style.flags |= flag;
    *field = value;
}

static void web_set_plain_property(web_style_state_t *state,
                                   uint32_t property,
                                   uint32_t *field,
                                   uint32_t value,
                                   uint32_t specificity,
                                   int important) {
    if (!web_cascade_allows(state, property, specificity, important)) {
        return;
    }
    *field = value;
}

static void web_set_background_color(web_style_state_t *state,
                                     uint32_t color,
                                     uint32_t specificity,
                                     int important) {
    if (!web_cascade_allows(state, WEB_PROP_BACKGROUND_COLOR, specificity, important)) {
        return;
    }
    state->style.flags |= WEB_STYLE_FLAG_HAS_BG_COLOR;
    state->style.background_color = color;
}

static void web_set_border_color(web_style_state_t *state,
                                 uint32_t color,
                                 uint32_t specificity,
                                 int important) {
    if (!web_cascade_allows(state, WEB_PROP_BORDER_COLOR, specificity, important)) {
        return;
    }
    state->style.flags |= WEB_STYLE_FLAG_HAS_BORDER_COLOR;
    state->style.border_color = color;
}

static void web_clear_length_property(web_style_state_t *state,
                                      uint32_t property,
                                      uint32_t flag,
                                      uint32_t specificity,
                                      int important) {
    if (!web_cascade_allows(state, property, specificity, important)) {
        return;
    }
    state->style.flags &= ~flag;
}

static int web_parse_decimal_scaled(const uint8_t *text, uint32_t *pos, uint32_t end, int64_t *out_scaled) {
    int negative = 0;
    int saw_digit = 0;
    int64_t whole = 0;
    int64_t frac = 0;
    int64_t frac_scale = 100;
    uint32_t i = *pos;
    if (i < end && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    while (i < end && web_is_digit(text[i])) {
        saw_digit = 1;
        if (whole < 1000000) {
            whole = whole * 10 + (int64_t)(text[i] - '0');
        }
        ++i;
    }
    if (i < end && text[i] == '.') {
        ++i;
        while (i < end && web_is_digit(text[i]) && frac_scale > 0) {
            saw_digit = 1;
            frac += (int64_t)(text[i] - '0') * frac_scale;
            frac_scale /= 10;
            ++i;
        }
        while (i < end && web_is_digit(text[i])) {
            ++i;
        }
    }
    if (!saw_digit) {
        return 0;
    }
    int64_t scaled = whole * 1000 + frac;
    if (negative) {
        scaled = -scaled;
    }
    *pos = i;
    *out_scaled = scaled;
    return 1;
}

static int web_scaled_to_px(int64_t scaled, uint32_t base) {
    int64_t px = (scaled * (int64_t)base) / 100000;
    if (px < 0) {
        return 0;
    }
    if (px > 1000000) {
        return 1000000;
    }
    return (int)px;
}

static int web_parse_one_length(const uint8_t *text,
                                uint32_t *pos,
                                uint32_t end,
                                uint32_t base,
                                uint32_t viewport_width,
                                uint32_t viewport_height,
                                int32_t *out_px) {
    uint32_t i = web_trim_start(text, *pos, end);
    int64_t scaled = 0;
    if (!web_parse_decimal_scaled(text, &i, end, &scaled)) {
        return 0;
    }
    while (i < end && web_is_space(text[i])) {
        ++i;
    }
    int64_t px = scaled / 1000;
    if (web_range_starts_cstr_ci(text, i, end, "%")) {
        px = web_scaled_to_px(scaled, base);
        ++i;
    } else if (web_range_starts_cstr_ci(text, i, end, "px")) {
        i += 2;
    } else if (web_range_starts_cstr_ci(text, i, end, "vw")) {
        px = web_scaled_to_px(scaled, viewport_width);
        i += 2;
    } else if (web_range_starts_cstr_ci(text, i, end, "vh")) {
        px = web_scaled_to_px(scaled, viewport_height);
        i += 2;
    } else if (web_range_starts_cstr_ci(text, i, end, "rem")) {
        px = (scaled * 16) / 1000;
        i += 3;
    } else if (web_range_starts_cstr_ci(text, i, end, "em")) {
        px = (scaled * 16) / 1000;
        i += 2;
    }
    if (px < 0) {
        px = 0;
    }
    if (px > 1000000) {
        px = 1000000;
    }
    *pos = i;
    *out_px = (int32_t)px;
    return 1;
}

static int web_parse_length_px(const uint8_t *text,
                               uint32_t start,
                               uint32_t end,
                               uint32_t base,
                               uint32_t viewport_width,
                               uint32_t viewport_height,
                               uint32_t *out_px) {
    start = web_trim_start(text, start, end);
    end = web_trim_end(text, start, end);
    if (start >= end || web_range_contains_cstr_ci(text, start, end, "auto")) {
        return 0;
    }
    int32_t value = 0;
    if (web_range_starts_cstr_ci(text, start, end, "calc(")) {
        uint32_t pos = start + 5u;
        if (!web_parse_one_length(text, &pos, end, base, viewport_width, viewport_height, &value)) {
            return 0;
        }
        while (pos < end && web_is_space(text[pos])) {
            ++pos;
        }
        if (pos < end && (text[pos] == '+' || text[pos] == '-')) {
            uint8_t op = text[pos];
            int32_t rhs = 0;
            ++pos;
            if (!web_parse_one_length(text, &pos, end, base, viewport_width, viewport_height, &rhs)) {
                return 0;
            }
            if (op == '+') {
                value += rhs;
            } else {
                value -= rhs;
            }
        }
    } else {
        uint32_t pos = start;
        if (!web_parse_one_length(text, &pos, end, base, viewport_width, viewport_height, &value)) {
            return 0;
        }
    }
    if (value < 0) {
        value = 0;
    }
    *out_px = (uint32_t)value;
    return 1;
}

static int web_parse_box_lengths(const uint8_t *text,
                                 uint32_t start,
                                 uint32_t end,
                                 uint32_t base,
                                 uint32_t viewport_width,
                                 uint32_t viewport_height,
                                 uint32_t *top,
                                 uint32_t *right,
                                 uint32_t *bottom,
                                 uint32_t *left) {
    uint32_t values[4] = {0, 0, 0, 0};
    uint32_t count = 0;
    uint32_t pos = start;
    while (count < 4) {
        pos = web_trim_start(text, pos, end);
        if (pos >= end || text[pos] == '!') {
            break;
        }
        uint32_t token_start = pos;
        while (pos < end && !web_is_space(text[pos]) && text[pos] != '!') {
            ++pos;
        }
        if (!web_parse_length_px(text, token_start, pos, base, viewport_width, viewport_height, &values[count])) {
            return 0;
        }
        ++count;
    }
    if (count == 0) {
        return 0;
    }
    if (count == 1) {
        *top = values[0];
        *right = values[0];
        *bottom = values[0];
        *left = values[0];
    } else if (count == 2) {
        *top = values[0];
        *right = values[1];
        *bottom = values[0];
        *left = values[1];
    } else if (count == 3) {
        *top = values[0];
        *right = values[1];
        *bottom = values[2];
        *left = values[1];
    } else {
        *top = values[0];
        *right = values[1];
        *bottom = values[2];
        *left = values[3];
    }
    return 1;
}

static int web_parse_border_width_value(const uint8_t *text,
                                        uint32_t start,
                                        uint32_t end,
                                        uint32_t base,
                                        uint32_t viewport_width,
                                        uint32_t viewport_height,
                                        uint32_t *out_px) {
    if (web_parse_length_px(text, start, end, base, viewport_width, viewport_height, out_px)) {
        return 1;
    }
    if (web_range_contains_cstr_ci(text, start, end, "thin")) {
        *out_px = 1;
        return 1;
    }
    if (web_range_contains_cstr_ci(text, start, end, "medium")) {
        *out_px = 2;
        return 1;
    }
    if (web_range_contains_cstr_ci(text, start, end, "thick")) {
        *out_px = 4;
        return 1;
    }
    if (web_range_contains_cstr_ci(text, start, end, "solid") ||
        web_range_contains_cstr_ci(text, start, end, "dotted") ||
        web_range_contains_cstr_ci(text, start, end, "dashed") ||
        web_range_contains_cstr_ci(text, start, end, "double")) {
        *out_px = 1;
        return 1;
    }
    return 0;
}

static int web_parse_opacity_value(const uint8_t *text, uint32_t start, uint32_t end, uint32_t *out_alpha) {
    uint32_t pos = web_trim_start(text, start, end);
    int64_t scaled = 0;
    if (!web_parse_decimal_scaled(text, &pos, end, &scaled)) {
        return 0;
    }
    while (pos < end && web_is_space(text[pos])) {
        ++pos;
    }
    if (pos < end && text[pos] == '%') {
        scaled /= 100;
    }
    if (scaled < 0) {
        scaled = 0;
    }
    if (scaled > 1000) {
        scaled = 1000;
    }
    *out_alpha = (uint32_t)((scaled * 255ll) / 1000ll);
    return 1;
}

static int web_parse_positive_integer_value(const uint8_t *text, uint32_t start, uint32_t end, uint32_t *out_value) {
    uint32_t pos = web_trim_start(text, start, end);
    int64_t scaled = 0;
    if (!web_parse_decimal_scaled(text, &pos, end, &scaled)) {
        return 0;
    }
    if (scaled <= 0) {
        *out_value = 0;
        return 1;
    }
    scaled /= 1000;
    if (scaled > 1000000) {
        scaled = 1000000;
    }
    *out_value = (uint32_t)scaled;
    return 1;
}

static int web_parse_line_height_value(const uint8_t *text,
                                       uint32_t start,
                                       uint32_t end,
                                       uint32_t font_size,
                                       uint32_t viewport_width,
                                       uint32_t viewport_height,
                                       uint32_t *out_px) {
    uint32_t pos = web_trim_start(text, start, end);
    uint32_t trimmed_end = web_trim_end(text, pos, end);
    int64_t scaled = 0;

    if (web_range_contains_cstr_ci(text, pos, trimmed_end, "normal")) {
        *out_px = (font_size * 6u) / 5u;
        return 1;
    }
    if (web_parse_decimal_scaled(text, &pos, trimmed_end, &scaled)) {
        while (pos < trimmed_end && web_is_space(text[pos])) {
            ++pos;
        }
        if (pos >= trimmed_end || text[pos] == '!') {
            if (scaled < 0) {
                scaled = 0;
            }
            *out_px = (uint32_t)(((int64_t)font_size * scaled) / 1000ll);
            return 1;
        }
        if (text[pos] == '%') {
            *out_px = (uint32_t)(((int64_t)font_size * scaled) / 100000ll);
            return 1;
        }
    }
    if (web_parse_length_px(text, start, trimmed_end, font_size, viewport_width, viewport_height, out_px)) {
        return 1;
    }
    return 0;
}

static void web_set_background_repeat(web_style_state_t *state,
                                      const uint8_t *value,
                                      uint32_t start,
                                      uint32_t end,
                                      uint32_t specificity,
                                      int important) {
    uint32_t repeat = WEB_STYLE_BG_REPEAT;
    if (web_range_contains_cstr_ci(value, start, end, "no-repeat")) {
        repeat = WEB_STYLE_BG_NO_REPEAT;
    } else if (web_range_contains_cstr_ci(value, start, end, "repeat-x")) {
        repeat = WEB_STYLE_BG_REPEAT_X;
    } else if (web_range_contains_cstr_ci(value, start, end, "repeat-y")) {
        repeat = WEB_STYLE_BG_REPEAT_Y;
    }
    web_set_plain_property(state, WEB_PROP_BACKGROUND_REPEAT, &state->style.background_repeat, repeat, specificity, important);
}

static void web_set_background_position(web_style_state_t *state,
                                        const uint8_t *value,
                                        uint32_t start,
                                        uint32_t end,
                                        uint32_t viewport_width,
                                        uint32_t viewport_height,
                                        uint32_t specificity,
                                        int important) {
    uint32_t top = 0;
    uint32_t right = 0;
    uint32_t bottom = 0;
    uint32_t left = 0;
    uint32_t x = 0;
    uint32_t y = 0;

    if (!web_cascade_allows(state, WEB_PROP_BACKGROUND_POSITION, specificity, important)) {
        return;
    }
    if (web_parse_box_lengths(value, start, end, viewport_width, viewport_width, viewport_height, &top, &right, &bottom, &left)) {
        state->style.background_position_x = top;
        state->style.background_position_y = right;
        return;
    }
    if (web_range_contains_cstr_ci(value, start, end, "center")) {
        x = viewport_width / 2u;
        y = viewport_height / 2u;
    }
    if (web_range_contains_cstr_ci(value, start, end, "right")) {
        x = viewport_width;
    } else if (web_range_contains_cstr_ci(value, start, end, "left")) {
        x = 0;
    }
    if (web_range_contains_cstr_ci(value, start, end, "bottom")) {
        y = viewport_height;
    } else if (web_range_contains_cstr_ci(value, start, end, "top")) {
        y = 0;
    }
    state->style.background_position_x = x;
    state->style.background_position_y = y;
}

static uint32_t web_css_skip_comment(const uint8_t *css, uint32_t pos, uint32_t end) {
    if (pos + 1u < end && css[pos] == '/' && css[pos + 1u] == '*') {
        pos += 2;
        while (pos + 1u < end) {
            if (css[pos] == '*' && css[pos + 1u] == '/') {
                return pos + 2u;
            }
            ++pos;
        }
        return end;
    }
    return pos;
}

static int web_value_has_important(const uint8_t *css, uint32_t start, uint32_t end) {
    return web_range_contains_cstr_ci(css, start, end, "!important");
}

static void web_apply_declarations(const uint8_t *css,
                                   uint32_t start,
                                   uint32_t end,
                                   web_style_state_t *state,
                                   uint32_t specificity,
                                   uint32_t viewport_width,
                                   uint32_t viewport_height) {
    uint32_t pos = start;
    ++state->order;
    while (pos < end) {
        pos = web_css_skip_comment(css, pos, end);
        while (pos < end && (web_is_space(css[pos]) || css[pos] == ';')) {
            ++pos;
        }
        if (pos >= end) {
            break;
        }
        uint32_t prop_start = pos;
        while (pos < end && css[pos] != ':' && css[pos] != ';' && css[pos] != '{' && css[pos] != '}') {
            ++pos;
        }
        if (pos >= end || css[pos] != ':') {
            while (pos < end && css[pos] != ';') {
                ++pos;
            }
            continue;
        }
        uint32_t prop_end = web_trim_end(css, prop_start, pos);
        prop_start = web_trim_start(css, prop_start, prop_end);
        ++pos;
        uint32_t value_start = pos;
        uint32_t paren_depth = 0;
        uint8_t quote = 0;
        while (pos < end) {
            uint8_t ch = css[pos];
            if (quote != 0) {
                if (ch == quote) {
                    quote = 0;
                }
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '(') {
                ++paren_depth;
            } else if (ch == ')' && paren_depth > 0) {
                --paren_depth;
            } else if (ch == ';' && paren_depth == 0) {
                break;
            }
            ++pos;
        }
        uint32_t value_end = web_trim_end(css, value_start, pos);
        value_start = web_trim_start(css, value_start, value_end);
        int important = web_value_has_important(css, value_start, value_end);

        if (web_range_equal_cstr_ci(css, prop_start, prop_end, "display")) {
            web_set_display(state, css, value_start, value_end, specificity, important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "float")) {
            web_set_float(state, css, value_start, value_end, specificity, important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "visibility")) {
            web_set_visibility(state, css, value_start, value_end, specificity, important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "text-align")) {
            if (web_range_contains_cstr_ci(css, value_start, value_end, "center")) {
                web_set_text_align(state, WEB_STYLE_ALIGN_CENTER, specificity, important);
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "right")) {
                web_set_text_align(state, WEB_STYLE_ALIGN_RIGHT, specificity, important);
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "left") ||
                       web_range_contains_cstr_ci(css, value_start, value_end, "start")) {
                web_set_text_align(state, WEB_STYLE_ALIGN_LEFT, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "color")) {
            uint32_t color = 0;
            if (web_parse_color_value(css, value_start, value_end, &color)) {
                web_set_color(state, color, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "background-color") ||
                   web_range_equal_cstr_ci(css, prop_start, prop_end, "background")) {
            uint32_t color = 0;
            if (web_parse_color_value(css, value_start, value_end, &color)) {
                web_set_background_color(state, color, specificity, important);
            }
            if (web_range_equal_cstr_ci(css, prop_start, prop_end, "background")) {
                web_set_background_repeat(state, css, value_start, value_end, specificity, important);
                web_set_background_position(state,
                                            css,
                                            value_start,
                                            value_end,
                                            viewport_width,
                                            viewport_height,
                                            specificity,
                                            important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "background-repeat")) {
            web_set_background_repeat(state, css, value_start, value_end, specificity, important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "background-position")) {
            web_set_background_position(state,
                                        css,
                                        value_start,
                                        value_end,
                                        viewport_width,
                                        viewport_height,
                                        specificity,
                                        important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "position")) {
            web_set_position(state, css, value_start, value_end, specificity, important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "left")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_LEFT, WEB_STYLE_FLAG_HAS_LEFT, &state->style.left, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_LEFT, WEB_STYLE_FLAG_HAS_LEFT, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "right")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_RIGHT, WEB_STYLE_FLAG_HAS_RIGHT, &state->style.right, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_RIGHT, WEB_STYLE_FLAG_HAS_RIGHT, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "top")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_height, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_TOP, WEB_STYLE_FLAG_HAS_TOP, &state->style.top, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_TOP, WEB_STYLE_FLAG_HAS_TOP, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "bottom")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_height, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_BOTTOM, WEB_STYLE_FLAG_HAS_BOTTOM, &state->style.bottom, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_BOTTOM, WEB_STYLE_FLAG_HAS_BOTTOM, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "width")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_WIDTH, WEB_STYLE_FLAG_HAS_WIDTH, &state->style.width, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_WIDTH, WEB_STYLE_FLAG_HAS_WIDTH, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "height")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_height, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_HEIGHT, WEB_STYLE_FLAG_HAS_HEIGHT, &state->style.height, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_HEIGHT, WEB_STYLE_FLAG_HAS_HEIGHT, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "min-width")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_MIN_WIDTH, WEB_STYLE_FLAG_HAS_MIN_WIDTH, &state->style.min_width, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_MIN_WIDTH, WEB_STYLE_FLAG_HAS_MIN_WIDTH, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "max-width")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_MAX_WIDTH, WEB_STYLE_FLAG_HAS_MAX_WIDTH, &state->style.max_width, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_MAX_WIDTH, WEB_STYLE_FLAG_HAS_MAX_WIDTH, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "min-height")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_height, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_MIN_HEIGHT, WEB_STYLE_FLAG_HAS_MIN_HEIGHT, &state->style.min_height, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_MIN_HEIGHT, WEB_STYLE_FLAG_HAS_MIN_HEIGHT, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "max-height")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_height, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_MAX_HEIGHT, WEB_STYLE_FLAG_HAS_MAX_HEIGHT, &state->style.max_height, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_MAX_HEIGHT, WEB_STYLE_FLAG_HAS_MAX_HEIGHT, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "inset")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_LEFT, WEB_STYLE_FLAG_HAS_LEFT, &state->style.left, px, specificity, important);
                web_set_length_property(state, WEB_PROP_RIGHT, WEB_STYLE_FLAG_HAS_RIGHT, &state->style.right, px, specificity, important);
                web_set_length_property(state, WEB_PROP_TOP, WEB_STYLE_FLAG_HAS_TOP, &state->style.top, px, specificity, important);
                web_set_length_property(state, WEB_PROP_BOTTOM, WEB_STYLE_FLAG_HAS_BOTTOM, &state->style.bottom, px, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "margin")) {
            uint32_t top = 0;
            uint32_t right = 0;
            uint32_t bottom = 0;
            uint32_t left = 0;
            web_set_flag_property(state,
                                  WEB_PROP_MARGIN_AUTO_X,
                                  WEB_STYLE_FLAG_MARGIN_AUTO_X,
                                  web_range_contains_cstr_ci(css, value_start, value_end, "auto"),
                                  specificity,
                                  important);
            if (web_parse_box_lengths(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &top, &right, &bottom, &left)) {
                web_set_length_property(state, WEB_PROP_MARGIN_TOP, WEB_STYLE_FLAG_HAS_MARGIN, &state->style.margin_top, top, specificity, important);
                web_set_length_property(state, WEB_PROP_MARGIN_RIGHT, WEB_STYLE_FLAG_HAS_MARGIN, &state->style.margin_right, right, specificity, important);
                web_set_length_property(state, WEB_PROP_MARGIN_BOTTOM, WEB_STYLE_FLAG_HAS_MARGIN, &state->style.margin_bottom, bottom, specificity, important);
                web_set_length_property(state, WEB_PROP_MARGIN_LEFT, WEB_STYLE_FLAG_HAS_MARGIN, &state->style.margin_left, left, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "margin") ||
                   web_range_equal_cstr_ci(css, prop_start, prop_end, "margin-left") ||
                   web_range_equal_cstr_ci(css, prop_start, prop_end, "margin-right")) {
            web_set_flag_property(state,
                                  WEB_PROP_MARGIN_AUTO_X,
                                  WEB_STYLE_FLAG_MARGIN_AUTO_X,
                                  web_range_contains_cstr_ci(css, value_start, value_end, "auto"),
                                  specificity,
                                  important);
            if (web_range_equal_cstr_ci(css, prop_start, prop_end, "margin-left")) {
                uint32_t px = 0;
                if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                    web_set_length_property(state, WEB_PROP_MARGIN_LEFT, WEB_STYLE_FLAG_HAS_MARGIN, &state->style.margin_left, px, specificity, important);
                } else {
                    web_clear_length_property(state, WEB_PROP_MARGIN_LEFT, WEB_STYLE_FLAG_HAS_MARGIN, specificity, important);
                }
            } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "margin-right")) {
                uint32_t px = 0;
                if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                    web_set_length_property(state, WEB_PROP_MARGIN_RIGHT, WEB_STYLE_FLAG_HAS_MARGIN, &state->style.margin_right, px, specificity, important);
                } else {
                    web_clear_length_property(state, WEB_PROP_MARGIN_RIGHT, WEB_STYLE_FLAG_HAS_MARGIN, specificity, important);
                }
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "margin-top")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_MARGIN_TOP, WEB_STYLE_FLAG_HAS_MARGIN, &state->style.margin_top, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_MARGIN_TOP, WEB_STYLE_FLAG_HAS_MARGIN, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "margin-bottom")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_MARGIN_BOTTOM, WEB_STYLE_FLAG_HAS_MARGIN, &state->style.margin_bottom, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_MARGIN_BOTTOM, WEB_STYLE_FLAG_HAS_MARGIN, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "padding")) {
            uint32_t top = 0;
            uint32_t right = 0;
            uint32_t bottom = 0;
            uint32_t left = 0;
            if (web_parse_box_lengths(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &top, &right, &bottom, &left)) {
                web_set_length_property(state, WEB_PROP_PADDING_TOP, WEB_STYLE_FLAG_HAS_PADDING, &state->style.padding_top, top, specificity, important);
                web_set_length_property(state, WEB_PROP_PADDING_RIGHT, WEB_STYLE_FLAG_HAS_PADDING, &state->style.padding_right, right, specificity, important);
                web_set_length_property(state, WEB_PROP_PADDING_BOTTOM, WEB_STYLE_FLAG_HAS_PADDING, &state->style.padding_bottom, bottom, specificity, important);
                web_set_length_property(state, WEB_PROP_PADDING_LEFT, WEB_STYLE_FLAG_HAS_PADDING, &state->style.padding_left, left, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "padding-left")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_PADDING_LEFT, WEB_STYLE_FLAG_HAS_PADDING, &state->style.padding_left, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_PADDING_LEFT, WEB_STYLE_FLAG_HAS_PADDING, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "padding-right")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_PADDING_RIGHT, WEB_STYLE_FLAG_HAS_PADDING, &state->style.padding_right, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_PADDING_RIGHT, WEB_STYLE_FLAG_HAS_PADDING, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "padding-top")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_PADDING_TOP, WEB_STYLE_FLAG_HAS_PADDING, &state->style.padding_top, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_PADDING_TOP, WEB_STYLE_FLAG_HAS_PADDING, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "padding-bottom")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_PADDING_BOTTOM, WEB_STYLE_FLAG_HAS_PADDING, &state->style.padding_bottom, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_PADDING_BOTTOM, WEB_STYLE_FLAG_HAS_PADDING, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "border-color")) {
            uint32_t color = 0;
            if (web_parse_color_value(css, value_start, value_end, &color)) {
                web_set_border_color(state, color, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "border") ||
                   web_range_equal_cstr_ci(css, prop_start, prop_end, "border-width")) {
            uint32_t px = 0;
            if (web_range_contains_cstr_ci(css, value_start, value_end, "none") ||
                web_range_contains_cstr_ci(css, value_start, value_end, "hidden")) {
                web_clear_length_property(state, WEB_PROP_BORDER_TOP, WEB_STYLE_FLAG_HAS_BORDER, specificity, important);
                web_clear_length_property(state, WEB_PROP_BORDER_RIGHT, WEB_STYLE_FLAG_HAS_BORDER, specificity, important);
                web_clear_length_property(state, WEB_PROP_BORDER_BOTTOM, WEB_STYLE_FLAG_HAS_BORDER, specificity, important);
                web_clear_length_property(state, WEB_PROP_BORDER_LEFT, WEB_STYLE_FLAG_HAS_BORDER, specificity, important);
            } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "border-width")) {
                uint32_t top = 0;
                uint32_t right = 0;
                uint32_t bottom = 0;
                uint32_t left = 0;
                if (web_parse_box_lengths(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &top, &right, &bottom, &left)) {
                    web_set_length_property(state, WEB_PROP_BORDER_TOP, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_top, top, specificity, important);
                    web_set_length_property(state, WEB_PROP_BORDER_RIGHT, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_right, right, specificity, important);
                    web_set_length_property(state, WEB_PROP_BORDER_BOTTOM, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_bottom, bottom, specificity, important);
                    web_set_length_property(state, WEB_PROP_BORDER_LEFT, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_left, left, specificity, important);
                }
            } else if (web_parse_border_width_value(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_BORDER_TOP, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_top, px, specificity, important);
                web_set_length_property(state, WEB_PROP_BORDER_RIGHT, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_right, px, specificity, important);
                web_set_length_property(state, WEB_PROP_BORDER_BOTTOM, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_bottom, px, specificity, important);
                web_set_length_property(state, WEB_PROP_BORDER_LEFT, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_left, px, specificity, important);
            }
            uint32_t color = 0;
            if (web_parse_color_value(css, value_start, value_end, &color)) {
                web_set_border_color(state, color, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "border-left-width")) {
            uint32_t px = 0;
            if (web_parse_border_width_value(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_BORDER_LEFT, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_left, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_BORDER_LEFT, WEB_STYLE_FLAG_HAS_BORDER, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "border-right-width")) {
            uint32_t px = 0;
            if (web_parse_border_width_value(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_BORDER_RIGHT, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_right, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_BORDER_RIGHT, WEB_STYLE_FLAG_HAS_BORDER, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "border-top-width")) {
            uint32_t px = 0;
            if (web_parse_border_width_value(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_BORDER_TOP, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_top, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_BORDER_TOP, WEB_STYLE_FLAG_HAS_BORDER, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "border-bottom-width")) {
            uint32_t px = 0;
            if (web_parse_border_width_value(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_BORDER_BOTTOM, WEB_STYLE_FLAG_HAS_BORDER, &state->style.border_bottom, px, specificity, important);
            } else {
                web_clear_length_property(state, WEB_PROP_BORDER_BOTTOM, WEB_STYLE_FLAG_HAS_BORDER, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "justify-content")) {
            if (web_range_contains_cstr_ci(css, value_start, value_end, "center")) {
                web_set_flag_property(state, WEB_PROP_CENTER_X, WEB_STYLE_FLAG_CENTER_X, 1, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "transform")) {
            if (web_range_contains_cstr_ci(css, value_start, value_end, "translate") &&
                web_range_contains_cstr_ci(css, value_start, value_end, "-50")) {
                web_set_flag_property(state, WEB_PROP_CENTER_X, WEB_STYLE_FLAG_CENTER_X, 1, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "font-weight")) {
            if (web_range_contains_cstr_ci(css, value_start, value_end, "bold") ||
                web_range_contains_cstr_ci(css, value_start, value_end, "600") ||
                web_range_contains_cstr_ci(css, value_start, value_end, "700") ||
                web_range_contains_cstr_ci(css, value_start, value_end, "800") ||
                web_range_contains_cstr_ci(css, value_start, value_end, "900")) {
                state->style.font_weight = WEB_STYLE_FONT_BOLD;
            } else {
                state->style.font_weight = WEB_STYLE_FONT_NORMAL;
            }
            web_set_flag_property(state,
                                  WEB_PROP_FONT_WEIGHT,
                                  WEB_STYLE_FLAG_FONT_BOLD,
                                  web_range_contains_cstr_ci(css, value_start, value_end, "bold") ||
                                      web_range_contains_cstr_ci(css, value_start, value_end, "600") ||
                                      web_range_contains_cstr_ci(css, value_start, value_end, "700") ||
                                      web_range_contains_cstr_ci(css, value_start, value_end, "800") ||
                                      web_range_contains_cstr_ci(css, value_start, value_end, "900"),
                                  specificity,
                                  important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "font-style")) {
            if (web_range_contains_cstr_ci(css, value_start, value_end, "italic") ||
                web_range_contains_cstr_ci(css, value_start, value_end, "oblique")) {
                state->style.font_style = WEB_STYLE_FONT_STYLE_ITALIC;
            } else {
                state->style.font_style = WEB_STYLE_FONT_STYLE_NORMAL;
            }
            web_set_flag_property(state,
                                  WEB_PROP_FONT_STYLE,
                                  WEB_STYLE_FLAG_FONT_ITALIC,
                                  web_range_contains_cstr_ci(css, value_start, value_end, "italic") ||
                                      web_range_contains_cstr_ci(css, value_start, value_end, "oblique"),
                                  specificity,
                                  important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "text-decoration") ||
                   web_range_equal_cstr_ci(css, prop_start, prop_end, "text-decoration-line")) {
            web_set_flag_property(state,
                                  WEB_PROP_TEXT_DECORATION,
                                  WEB_STYLE_FLAG_TEXT_UNDERLINE,
                                  web_range_contains_cstr_ci(css, value_start, value_end, "underline"),
                                  specificity,
                                  important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "font-size")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, 16, viewport_width, viewport_height, &px) &&
                web_cascade_allows(state, WEB_PROP_FONT_SIZE, specificity, important)) {
                if (px < 6) {
                    px = 6;
                }
                if (px > 72) {
                    px = 72;
                }
                state->style.font_size = px;
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "line-height")) {
            uint32_t px = 0;
            (void)web_parse_line_height_value(css,
                                              value_start,
                                              value_end,
                                              state->style.font_size,
                                              viewport_width,
                                              viewport_height,
                                              &px);
            if (px != 0 && web_cascade_allows(state, WEB_PROP_LINE_HEIGHT, specificity, important)) {
                if (px < 10) {
                    px = 10;
                }
                if (px > 72) {
                    px = 72;
                }
                state->style.line_height = px;
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "text-transform")) {
            if (web_cascade_allows(state, WEB_PROP_TEXT_TRANSFORM, specificity, important)) {
                if (web_range_contains_cstr_ci(css, value_start, value_end, "uppercase")) {
                    state->style.text_transform = WEB_STYLE_TEXT_TRANSFORM_UPPERCASE;
                } else if (web_range_contains_cstr_ci(css, value_start, value_end, "lowercase")) {
                    state->style.text_transform = WEB_STYLE_TEXT_TRANSFORM_LOWERCASE;
                } else if (web_range_contains_cstr_ci(css, value_start, value_end, "capitalize")) {
                    state->style.text_transform = WEB_STYLE_TEXT_TRANSFORM_CAPITALIZE;
                } else {
                    state->style.text_transform = WEB_STYLE_TEXT_TRANSFORM_NONE;
                }
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "white-space")) {
            if (web_cascade_allows(state, WEB_PROP_WHITE_SPACE, specificity, important)) {
                if (web_range_contains_cstr_ci(css, value_start, value_end, "pre-wrap")) {
                    state->style.white_space = WEB_STYLE_WHITE_SPACE_PRE_WRAP;
                } else if (web_range_contains_cstr_ci(css, value_start, value_end, "pre-line")) {
                    state->style.white_space = WEB_STYLE_WHITE_SPACE_PRE_LINE;
                } else if (web_range_contains_cstr_ci(css, value_start, value_end, "nowrap")) {
                    state->style.white_space = WEB_STYLE_WHITE_SPACE_NOWRAP;
                } else if (web_range_contains_cstr_ci(css, value_start, value_end, "pre")) {
                    state->style.white_space = WEB_STYLE_WHITE_SPACE_PRE;
                } else {
                    state->style.white_space = WEB_STYLE_WHITE_SPACE_NORMAL;
                }
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "list-style-type") ||
                   web_range_equal_cstr_ci(css, prop_start, prop_end, "list-style")) {
            if (web_cascade_allows(state, WEB_PROP_LIST_STYLE_TYPE, specificity, important)) {
                if (web_range_contains_cstr_ci(css, value_start, value_end, "none")) {
                    state->style.list_style_type = WEB_STYLE_LIST_NONE;
                } else if (web_range_contains_cstr_ci(css, value_start, value_end, "circle")) {
                    state->style.list_style_type = WEB_STYLE_LIST_CIRCLE;
                } else if (web_range_contains_cstr_ci(css, value_start, value_end, "square")) {
                    state->style.list_style_type = WEB_STYLE_LIST_SQUARE;
                } else if (web_range_contains_cstr_ci(css, value_start, value_end, "decimal") ||
                           web_range_contains_cstr_ci(css, value_start, value_end, "roman") ||
                           web_range_contains_cstr_ci(css, value_start, value_end, "alpha")) {
                    state->style.list_style_type = WEB_STYLE_LIST_DECIMAL;
                } else {
                    state->style.list_style_type = WEB_STYLE_LIST_DISC;
                }
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "overflow") ||
                   web_range_equal_cstr_ci(css, prop_start, prop_end, "overflow-x") ||
                   web_range_equal_cstr_ci(css, prop_start, prop_end, "overflow-y")) {
            uint32_t overflow = WEB_STYLE_OVERFLOW_VISIBLE;
            if (web_range_contains_cstr_ci(css, value_start, value_end, "hidden")) {
                overflow = WEB_STYLE_OVERFLOW_HIDDEN;
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "scroll")) {
                overflow = WEB_STYLE_OVERFLOW_SCROLL;
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "auto")) {
                overflow = WEB_STYLE_OVERFLOW_AUTO;
            }
            if ((web_range_equal_cstr_ci(css, prop_start, prop_end, "overflow") ||
                 web_range_equal_cstr_ci(css, prop_start, prop_end, "overflow-x")) &&
                web_cascade_allows(state, WEB_PROP_OVERFLOW_X, specificity, important)) {
                state->style.overflow_x = overflow;
            }
            if ((web_range_equal_cstr_ci(css, prop_start, prop_end, "overflow") ||
                 web_range_equal_cstr_ci(css, prop_start, prop_end, "overflow-y")) &&
                web_cascade_allows(state, WEB_PROP_OVERFLOW_Y, specificity, important)) {
                state->style.overflow_y = overflow;
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "text-indent")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px) &&
                web_cascade_allows(state, WEB_PROP_TEXT_INDENT, specificity, important)) {
                state->style.text_indent = px;
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "box-sizing")) {
            web_set_plain_property(state,
                                   WEB_PROP_BOX_SIZING,
                                   &state->style.box_sizing,
                                   web_range_contains_cstr_ci(css, value_start, value_end, "border-box") ?
                                       WEB_STYLE_BOX_BORDER_BOX : WEB_STYLE_BOX_CONTENT_BOX,
                                   specificity,
                                   important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "border-collapse")) {
            web_set_plain_property(state,
                                   WEB_PROP_BORDER_COLLAPSE,
                                   &state->style.border_collapse,
                                   web_range_contains_cstr_ci(css, value_start, value_end, "collapse") ?
                                       WEB_STYLE_BORDER_COLLAPSE : WEB_STYLE_BORDER_SEPARATE,
                                   specificity,
                                   important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "border-spacing")) {
            uint32_t top = 0;
            uint32_t right = 0;
            uint32_t bottom = 0;
            uint32_t left = 0;
            if (web_parse_box_lengths(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &top, &right, &bottom, &left) &&
                web_cascade_allows(state, WEB_PROP_BORDER_SPACING, specificity, important)) {
                state->style.border_spacing_h = top;
                state->style.border_spacing_v = right;
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "clear")) {
            uint32_t clear_side = WEB_STYLE_CLEAR_NONE;
            if (web_range_contains_cstr_ci(css, value_start, value_end, "both")) {
                clear_side = WEB_STYLE_CLEAR_BOTH;
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "left")) {
                clear_side = WEB_STYLE_CLEAR_LEFT;
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "right")) {
                clear_side = WEB_STYLE_CLEAR_RIGHT;
            }
            web_set_plain_property(state, WEB_PROP_CLEAR, &state->style.clear_side, clear_side, specificity, important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "vertical-align")) {
            uint32_t align = WEB_STYLE_VERTICAL_BASELINE;
            if (web_range_contains_cstr_ci(css, value_start, value_end, "sub")) {
                align = WEB_STYLE_VERTICAL_SUB;
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "super")) {
                align = WEB_STYLE_VERTICAL_SUPER;
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "middle")) {
                align = WEB_STYLE_VERTICAL_MIDDLE;
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "bottom")) {
                align = WEB_STYLE_VERTICAL_BOTTOM;
            } else if (web_range_contains_cstr_ci(css, value_start, value_end, "top")) {
                align = WEB_STYLE_VERTICAL_TOP;
            }
            web_set_plain_property(state, WEB_PROP_VERTICAL_ALIGN, &state->style.vertical_align, align, specificity, important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "list-style-position")) {
            web_set_plain_property(state,
                                   WEB_PROP_LIST_STYLE_POSITION,
                                   &state->style.list_style_position,
                                   web_range_contains_cstr_ci(css, value_start, value_end, "inside") ?
                                       WEB_STYLE_LIST_POSITION_INSIDE : WEB_STYLE_LIST_POSITION_OUTSIDE,
                                   specificity,
                                   important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "caption-side")) {
            web_set_plain_property(state,
                                   WEB_PROP_CAPTION_SIDE,
                                   &state->style.caption_side,
                                   web_range_contains_cstr_ci(css, value_start, value_end, "bottom") ?
                                       WEB_STYLE_CAPTION_BOTTOM : WEB_STYLE_CAPTION_TOP,
                                   specificity,
                                   important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "direction")) {
            web_set_plain_property(state,
                                   WEB_PROP_DIRECTION,
                                   &state->style.direction,
                                   web_range_contains_cstr_ci(css, value_start, value_end, "rtl") ?
                                       WEB_STYLE_DIRECTION_RTL : WEB_STYLE_DIRECTION_LTR,
                                   specificity,
                                   important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "table-layout")) {
            web_set_plain_property(state,
                                   WEB_PROP_TABLE_LAYOUT,
                                   &state->style.table_layout,
                                   web_range_contains_cstr_ci(css, value_start, value_end, "fixed") ?
                                       WEB_STYLE_TABLE_LAYOUT_FIXED : WEB_STYLE_TABLE_LAYOUT_AUTO,
                                   specificity,
                                   important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "empty-cells")) {
            web_set_plain_property(state,
                                   WEB_PROP_EMPTY_CELLS,
                                   &state->style.empty_cells,
                                   web_range_contains_cstr_ci(css, value_start, value_end, "hide") ?
                                       WEB_STYLE_EMPTY_CELLS_HIDE : WEB_STYLE_EMPTY_CELLS_SHOW,
                                   specificity,
                                   important);
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "opacity")) {
            uint32_t opacity = 255;
            if (web_parse_opacity_value(css, value_start, value_end, &opacity)) {
                web_set_plain_property(state, WEB_PROP_OPACITY, &state->style.opacity, opacity, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "z-index")) {
            uint32_t z_index = 0;
            if (web_parse_positive_integer_value(css, value_start, value_end, &z_index)) {
                web_set_plain_property(state, WEB_PROP_Z_INDEX, &state->style.z_index, z_index, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "letter-spacing")) {
            uint32_t px = 0;
            if (web_range_contains_cstr_ci(css, value_start, value_end, "normal") ||
                web_parse_length_px(css, value_start, value_end, state->style.font_size, viewport_width, viewport_height, &px)) {
                web_set_plain_property(state, WEB_PROP_LETTER_SPACING, &state->style.letter_spacing, px, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "word-spacing")) {
            uint32_t px = 0;
            if (web_range_contains_cstr_ci(css, value_start, value_end, "normal") ||
                web_parse_length_px(css, value_start, value_end, state->style.font_size, viewport_width, viewport_height, &px)) {
                web_set_plain_property(state, WEB_PROP_WORD_SPACING, &state->style.word_spacing, px, specificity, important);
            }
        }

        if (pos < end && css[pos] == ';') {
            ++pos;
        }
    }
}

static int web_attr_selector_matches(const uint8_t *html,
                                     uint32_t tag_pos,
                                     const uint8_t *selector,
                                     uint32_t *io_pos,
                                     uint32_t end) {
    uint32_t pos = *io_pos + 1u;
    while (pos < end && web_is_space(selector[pos])) {
        ++pos;
    }
    uint32_t name_start = pos;
    while (pos < end && web_is_name_char(selector[pos])) {
        ++pos;
    }
    uint32_t name_end = pos;
    while (pos < end && web_is_space(selector[pos])) {
        ++pos;
    }
    uint8_t op = 0;
    uint8_t op_prefix = 0;
    if (pos < end && selector[pos] != ']') {
        if (pos + 1u < end && selector[pos + 1u] == '=') {
            op_prefix = selector[pos];
            op = '=';
            pos += 2;
        } else if (selector[pos] == '=') {
            op = '=';
            ++pos;
        }
    }
    while (pos < end && web_is_space(selector[pos])) {
        ++pos;
    }
    uint32_t value_start = pos;
    uint32_t value_end = pos;
    if (op != 0) {
        uint8_t quote = 0;
        if (pos < end && (selector[pos] == '"' || selector[pos] == '\'')) {
            quote = selector[pos];
            ++pos;
            value_start = pos;
            while (pos < end && selector[pos] != quote) {
                ++pos;
            }
            value_end = pos;
            if (pos < end && selector[pos] == quote) {
                ++pos;
            }
        } else {
            value_start = pos;
            while (pos < end && selector[pos] != ']' && !web_is_space(selector[pos])) {
                ++pos;
            }
            value_end = pos;
        }
    }
    while (pos < end && selector[pos] != ']') {
        ++pos;
    }
    if (pos < end && selector[pos] == ']') {
        *io_pos = pos + 1u;
    } else {
        *io_pos = end;
    }

    uint32_t attr_start = 0;
    uint32_t attr_end = 0;
    if (name_start >= name_end ||
        !web_attr_value_range_name(html, tag_pos, selector, name_start, name_end, &attr_start, &attr_end)) {
        return 0;
    }
    if (op == 0) {
        return 1;
    }
    attr_start = web_trim_start(html, attr_start, attr_end);
    attr_end = web_trim_end(html, attr_start, attr_end);
    if (op_prefix == '~') {
        uint32_t scan = attr_start;
        while (scan < attr_end) {
            while (scan < attr_end && web_is_space(html[scan])) {
                ++scan;
            }
            uint32_t token_start = scan;
            while (scan < attr_end && !web_is_space(html[scan])) {
                ++scan;
            }
            if (token_start < scan && web_range_equal_ci(html, token_start, scan, selector, value_start, value_end)) {
                return 1;
            }
        }
        return 0;
    }
    if (op_prefix == '*') {
        return web_range_contains_range_ci(html, attr_start, attr_end, selector, value_start, value_end);
    }
    if (op_prefix == '^') {
        return attr_end - attr_start >= value_end - value_start &&
               web_range_equal_ci(html, attr_start, attr_start + (value_end - value_start), selector, value_start, value_end);
    }
    if (op_prefix == '$') {
        return web_range_ends_range_ci(html, attr_start, attr_end, selector, value_start, value_end);
    }
    if (op_prefix == '|') {
        uint32_t len = value_end - value_start;
        if (attr_end - attr_start < len ||
            !web_range_equal_ci(html, attr_start, attr_start + len, selector, value_start, value_end)) {
            return 0;
        }
        return attr_start + len == attr_end || html[attr_start + len] == '-';
    }
    return web_range_equal_ci(html, attr_start, attr_end, selector, value_start, value_end);
}

static int web_compound_selector_matches(const uint8_t *html,
                                         uint32_t tag_pos,
                                         const uint8_t *selector,
                                         uint32_t start,
                                         uint32_t end) {
    uint32_t pos = web_trim_start(selector, start, end);
    end = web_trim_end(selector, pos, end);
    int saw_matchable = 0;
    if (pos >= end) {
        return 0;
    }
    if (selector[pos] == '*') {
        saw_matchable = 1;
        ++pos;
    } else if (web_is_name_char(selector[pos])) {
        uint32_t name_start = pos;
        while (pos < end && (web_is_name_char(selector[pos]) || selector[pos] == '|')) {
            if (selector[pos] == '|') {
                name_start = pos + 1u;
            }
            ++pos;
        }
        if (name_start < pos &&
            (selector[name_start] == '*' ||
             web_tag_name_matches_range(html, tag_pos, selector, name_start, pos))) {
            saw_matchable = 1;
        } else {
            return 0;
        }
    }
    while (pos < end) {
        uint8_t ch = selector[pos];
        if (web_is_space(ch)) {
            ++pos;
            continue;
        }
        if (ch == '.') {
            ++pos;
            uint32_t class_start = pos;
            while (pos < end && web_is_name_char(selector[pos])) {
                ++pos;
            }
            if (class_start == pos ||
                !web_attr_token_contains_range(html, tag_pos, "class", selector, class_start, pos)) {
                return 0;
            }
            saw_matchable = 1;
        } else if (ch == '#') {
            ++pos;
            uint32_t id_start = pos;
            while (pos < end && web_is_name_char(selector[pos])) {
                ++pos;
            }
            if (id_start == pos ||
                !web_attr_value_exact_range_ci(html, tag_pos, "id", selector, id_start, pos)) {
                return 0;
            }
            saw_matchable = 1;
        } else if (ch == '[') {
            if (!web_attr_selector_matches(html, tag_pos, selector, &pos, end)) {
                return 0;
            }
            saw_matchable = 1;
        } else if (ch == ':') {
            if (pos + 1u < end && selector[pos + 1u] == ':') {
                return 0;
            }
            ++pos;
            uint32_t pseudo_start = pos;
            while (pos < end && web_is_name_char(selector[pos])) {
                ++pos;
            }
            if (web_range_equal_cstr_ci(selector, pseudo_start, pos, "root") &&
                !web_tag_name_is(html, tag_pos, "html")) {
                return 0;
            }
            if (pos < end && selector[pos] == '(') {
                uint32_t depth = 1;
                ++pos;
                while (pos < end && depth > 0) {
                    if (selector[pos] == '(') {
                        ++depth;
                    } else if (selector[pos] == ')') {
                        --depth;
                    }
                    ++pos;
                }
            }
        } else {
            ++pos;
        }
    }
    return saw_matchable;
}

static uint32_t web_selector_specificity(const uint8_t *selector, uint32_t start, uint32_t end) {
    uint32_t specificity = 0;
    int at_component_start = 1;
    uint8_t quote = 0;
    uint32_t paren_depth = 0;
    uint32_t bracket_depth = 0;
    for (uint32_t pos = start; pos < end; ++pos) {
        uint8_t ch = selector[pos];
        if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (ch == '[') {
            ++bracket_depth;
            specificity += 10;
            at_component_start = 0;
            continue;
        }
        if (ch == ']' && bracket_depth > 0) {
            --bracket_depth;
            continue;
        }
        if (bracket_depth != 0) {
            continue;
        }
        if (ch == '(') {
            ++paren_depth;
            continue;
        }
        if (ch == ')' && paren_depth > 0) {
            --paren_depth;
            continue;
        }
        if (ch == '#') {
            specificity += 100;
            at_component_start = 0;
        } else if (ch == '.') {
            specificity += 10;
            at_component_start = 0;
        } else if (ch == ':' && (pos + 1u >= end || selector[pos + 1u] != ':')) {
            specificity += 10;
            at_component_start = 0;
        } else if (web_is_space(ch) || ch == '>' || ch == '+' || ch == '~' || ch == ',') {
            at_component_start = 1;
        } else if (at_component_start && web_is_name_char(ch)) {
            specificity += 1;
            at_component_start = 0;
        }
    }
    return specificity;
}

static int web_selector_single_matches(const uint8_t *html,
                                       uint32_t tag_pos,
                                       const uint8_t *selector,
                                       uint32_t start,
                                       uint32_t end) {
    uint32_t part_start = start;
    uint8_t quote = 0;
    uint32_t bracket_depth = 0;
    uint32_t paren_depth = 0;
    for (uint32_t pos = start; pos < end; ++pos) {
        uint8_t ch = selector[pos];
        if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            }
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            continue;
        }
        if (ch == '[') {
            ++bracket_depth;
            continue;
        }
        if (ch == ']' && bracket_depth > 0) {
            --bracket_depth;
            continue;
        }
        if (ch == '(') {
            ++paren_depth;
            continue;
        }
        if (ch == ')' && paren_depth > 0) {
            --paren_depth;
            continue;
        }
        if (bracket_depth == 0 && paren_depth == 0 &&
            (web_is_space(ch) || ch == '>' || ch == '+' || ch == '~')) {
            part_start = pos + 1u;
        }
    }
    part_start = web_trim_start(selector, part_start, end);
    return web_compound_selector_matches(html, tag_pos, selector, part_start, end);
}

static int web_selector_list_matches(const uint8_t *html,
                                     uint32_t tag_pos,
                                     const uint8_t *selector,
                                     uint32_t start,
                                     uint32_t end,
                                     uint32_t *out_specificity) {
    uint32_t part_start = start;
    uint8_t quote = 0;
    uint32_t bracket_depth = 0;
    uint32_t paren_depth = 0;
    int matched = 0;
    uint32_t best_specificity = 0;
    for (uint32_t pos = start; pos <= end; ++pos) {
        uint8_t ch = (pos < end) ? selector[pos] : ',';
        if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            }
            continue;
        }
        if (pos < end && (ch == '"' || ch == '\'')) {
            quote = ch;
            continue;
        }
        if (pos < end && ch == '[') {
            ++bracket_depth;
            continue;
        }
        if (pos < end && ch == ']' && bracket_depth > 0) {
            --bracket_depth;
            continue;
        }
        if (pos < end && ch == '(') {
            ++paren_depth;
            continue;
        }
        if (pos < end && ch == ')' && paren_depth > 0) {
            --paren_depth;
            continue;
        }
        if (ch == ',' && bracket_depth == 0 && paren_depth == 0) {
            uint32_t part_end = web_trim_end(selector, part_start, pos);
            uint32_t clean_start = web_trim_start(selector, part_start, part_end);
            if (clean_start < part_end &&
                !web_range_starts_cstr_ci(selector, clean_start, part_end, "@") &&
                web_selector_single_matches(html, tag_pos, selector, clean_start, part_end)) {
                uint32_t specificity = web_selector_specificity(selector, clean_start, part_end);
                if (!matched || specificity > best_specificity) {
                    best_specificity = specificity;
                }
                matched = 1;
            }
            part_start = pos + 1u;
        }
    }
    if (matched) {
        *out_specificity = best_specificity;
    }
    return matched;
}

static uint32_t web_css_find_block_end(const uint8_t *css, uint32_t block_start, uint32_t end) {
    uint32_t depth = 1;
    uint32_t pos = block_start;
    uint8_t quote = 0;
    while (pos < end) {
        pos = web_css_skip_comment(css, pos, end);
        if (pos >= end) {
            break;
        }
        uint8_t ch = css[pos];
        if (quote != 0) {
            if (ch == quote) {
                quote = 0;
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0) {
                return pos;
            }
        }
        ++pos;
    }
    return end;
}

static int web_media_query_matches(const uint8_t *css,
                                   uint32_t start,
                                   uint32_t end,
                                   uint32_t viewport_width,
                                   uint32_t viewport_height) {
    int saw_constraint = 0;
    int matches = 1;
    uint32_t pos = start;
    while (pos < end) {
        if (web_range_starts_cstr_ci(css, pos, end, "max-width")) {
            saw_constraint = 1;
            while (pos < end && css[pos] != ':') {
                ++pos;
            }
            if (pos < end) {
                ++pos;
            }
            uint32_t value_start = pos;
            while (pos < end && css[pos] != ')' && css[pos] != ',') {
                ++pos;
            }
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, pos, viewport_width, viewport_width, viewport_height, &px) &&
                viewport_width > px) {
                matches = 0;
            }
        } else if (web_range_starts_cstr_ci(css, pos, end, "min-width")) {
            saw_constraint = 1;
            while (pos < end && css[pos] != ':') {
                ++pos;
            }
            if (pos < end) {
                ++pos;
            }
            uint32_t value_start = pos;
            while (pos < end && css[pos] != ')' && css[pos] != ',') {
                ++pos;
            }
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, pos, viewport_width, viewport_width, viewport_height, &px) &&
                viewport_width < px) {
                matches = 0;
            }
        } else {
            ++pos;
        }
    }
    return !saw_constraint || matches;
}

static void web_scan_css_rules(const uint8_t *css,
                               uint32_t start,
                               uint32_t end,
                               const uint8_t *html,
                               uint32_t tag_pos,
                               web_style_state_t *state,
                               uint32_t viewport_width,
                               uint32_t viewport_height,
                               uint32_t depth) {
    if (depth > 4u) {
        return;
    }
    uint32_t pos = start;
    while (pos < end) {
        pos = web_css_skip_comment(css, pos, end);
        while (pos < end && web_is_space(css[pos])) {
            ++pos;
        }
        if (pos >= end) {
            break;
        }
        uint32_t selector_start = pos;
        uint8_t quote = 0;
        uint32_t paren_depth = 0;
        uint32_t bracket_depth = 0;
        while (pos < end) {
            uint8_t ch = css[pos];
            if (quote != 0) {
                if (ch == quote) {
                    quote = 0;
                }
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '(') {
                ++paren_depth;
            } else if (ch == ')' && paren_depth > 0) {
                --paren_depth;
            } else if (ch == '[') {
                ++bracket_depth;
            } else if (ch == ']' && bracket_depth > 0) {
                --bracket_depth;
            } else if (ch == '{' && paren_depth == 0 && bracket_depth == 0) {
                break;
            } else if (ch == ';' && paren_depth == 0 && bracket_depth == 0 &&
                       web_range_starts_cstr_ci(css, web_trim_start(css, selector_start, pos), pos, "@")) {
                break;
            }
            ++pos;
        }
        if (pos >= end || css[pos] != '{') {
            ++pos;
            continue;
        }
        uint32_t selector_end = web_trim_end(css, selector_start, pos);
        uint32_t selector_clean_start = web_trim_start(css, selector_start, selector_end);
        uint32_t block_start = pos + 1u;
        uint32_t block_end = web_css_find_block_end(css, block_start, end);
        if (selector_clean_start < selector_end &&
            web_range_starts_cstr_ci(css, selector_clean_start, selector_end, "@")) {
            if (web_range_starts_cstr_ci(css, selector_clean_start, selector_end, "@media")) {
                if (web_media_query_matches(css,
                                            selector_clean_start + 6u,
                                            selector_end,
                                            viewport_width,
                                            viewport_height)) {
                    web_scan_css_rules(css, block_start, block_end, html, tag_pos, state, viewport_width, viewport_height, depth + 1u);
                }
            } else if (web_range_starts_cstr_ci(css, selector_clean_start, selector_end, "@supports") ||
                       web_range_starts_cstr_ci(css, selector_clean_start, selector_end, "@layer")) {
                web_scan_css_rules(css, block_start, block_end, html, tag_pos, state, viewport_width, viewport_height, depth + 1u);
            }
        } else {
            uint32_t specificity = 0;
            if (web_selector_list_matches(html, tag_pos, css, selector_clean_start, selector_end, &specificity)) {
                web_apply_declarations(css, block_start, block_end, state, specificity, viewport_width, viewport_height);
            }
        }
        pos = block_end;
        if (pos < end && css[pos] == '}') {
            ++pos;
        }
    }
}

static uint32_t web_find_style_close(const uint8_t *html, uint32_t pos) {
    while (html[pos] != 0) {
        if (html[pos] == '<' && web_is_closing_tag(html, pos) && web_tag_name_is(html, pos, "style")) {
            return pos;
        }
        ++pos;
    }
    return pos;
}

static uint32_t web_find_named_close(const uint8_t *html, uint32_t pos, const char *name) {
    while (html[pos] != 0) {
        if (html[pos] == '<' && web_is_closing_tag(html, pos) && web_tag_name_is(html, pos, name)) {
            return pos;
        }
        ++pos;
    }
    return pos;
}

static int web_tag_self_closes(const uint8_t *html, uint32_t pos) {
    uint32_t last = 0;
    while (html[pos] != 0 && html[pos] != '>') {
        if (!web_is_space(html[pos])) {
            last = html[pos];
        }
        ++pos;
    }
    return last == '/';
}

static int web_css_void_tag(const uint8_t *html, uint32_t pos) {
    return web_tag_self_closes(html, pos) ||
           web_tag_name_is(html, pos, "area") ||
           web_tag_name_is(html, pos, "base") ||
           web_tag_name_is(html, pos, "basefont") ||
           web_tag_name_is(html, pos, "bgsound") ||
           web_tag_name_is(html, pos, "br") ||
           web_tag_name_is(html, pos, "col") ||
           web_tag_name_is(html, pos, "command") ||
           web_tag_name_is(html, pos, "embed") ||
           web_tag_name_is(html, pos, "frame") ||
           web_tag_name_is(html, pos, "hr") ||
           web_tag_name_is(html, pos, "img") ||
           web_tag_name_is(html, pos, "input") ||
           web_tag_name_is(html, pos, "keygen") ||
           web_tag_name_is(html, pos, "link") ||
           web_tag_name_is(html, pos, "menuitem") ||
           web_tag_name_is(html, pos, "meta") ||
           web_tag_name_is(html, pos, "param") ||
           web_tag_name_is(html, pos, "source") ||
           web_tag_name_is(html, pos, "track") ||
           web_tag_name_is(html, pos, "wbr");
}

static int web_css_stack_top_is(const uint8_t *html, const int32_t *stack, uint32_t depth, const char *name) {
    if (depth == 0u || stack[depth - 1u] < 0) {
        return 0;
    }
    return web_tag_name_is(html, web_css_nodes[(uint32_t)stack[depth - 1u]].tag_pos, name);
}

static int web_css_stack_top_is_cell(const uint8_t *html, const int32_t *stack, uint32_t depth) {
    return web_css_stack_top_is(html, stack, depth, "td") ||
           web_css_stack_top_is(html, stack, depth, "th");
}

static int web_css_stack_top_is_dtdd(const uint8_t *html, const int32_t *stack, uint32_t depth) {
    return web_css_stack_top_is(html, stack, depth, "dt") ||
           web_css_stack_top_is(html, stack, depth, "dd");
}

static int web_css_new_tag_closes_p(const uint8_t *html, uint32_t pos) {
    return web_tag_name_is(html, pos, "address") ||
           web_tag_name_is(html, pos, "article") ||
           web_tag_name_is(html, pos, "aside") ||
           web_tag_name_is(html, pos, "blockquote") ||
           web_tag_name_is(html, pos, "details") ||
           web_tag_name_is(html, pos, "div") ||
           web_tag_name_is(html, pos, "dl") ||
           web_tag_name_is(html, pos, "fieldset") ||
           web_tag_name_is(html, pos, "figcaption") ||
           web_tag_name_is(html, pos, "figure") ||
           web_tag_name_is(html, pos, "footer") ||
           web_tag_name_is(html, pos, "form") ||
           web_tag_name_is(html, pos, "h1") ||
           web_tag_name_is(html, pos, "h2") ||
           web_tag_name_is(html, pos, "h3") ||
           web_tag_name_is(html, pos, "h4") ||
           web_tag_name_is(html, pos, "h5") ||
           web_tag_name_is(html, pos, "h6") ||
           web_tag_name_is(html, pos, "header") ||
           web_tag_name_is(html, pos, "hr") ||
           web_tag_name_is(html, pos, "main") ||
           web_tag_name_is(html, pos, "menu") ||
           web_tag_name_is(html, pos, "nav") ||
           web_tag_name_is(html, pos, "ol") ||
           web_tag_name_is(html, pos, "p") ||
           web_tag_name_is(html, pos, "pre") ||
           web_tag_name_is(html, pos, "search") ||
           web_tag_name_is(html, pos, "section") ||
           web_tag_name_is(html, pos, "table") ||
           web_tag_name_is(html, pos, "ul");
}

static void web_css_apply_implicit_closes(const uint8_t *html, uint32_t pos, const int32_t *stack, uint32_t *depth) {
    int changed;

    if (depth == NULL) {
        return;
    }
    do {
        changed = 0;
        if (*depth == 0u) {
            return;
        }
        if (web_css_stack_top_is(html, stack, *depth, "p") && web_css_new_tag_closes_p(html, pos)) {
            --*depth;
            changed = 1;
        } else if (web_css_stack_top_is(html, stack, *depth, "li") && web_tag_name_is(html, pos, "li")) {
            --*depth;
            changed = 1;
        } else if (web_css_stack_top_is_dtdd(html, stack, *depth) &&
                   (web_tag_name_is(html, pos, "dt") || web_tag_name_is(html, pos, "dd"))) {
            --*depth;
            changed = 1;
        } else if (web_css_stack_top_is(html, stack, *depth, "option") &&
                   (web_tag_name_is(html, pos, "option") || web_tag_name_is(html, pos, "optgroup"))) {
            --*depth;
            changed = 1;
        } else if (web_css_stack_top_is_cell(html, stack, *depth) &&
                   (web_tag_name_is(html, pos, "td") ||
                    web_tag_name_is(html, pos, "th") ||
                    web_tag_name_is(html, pos, "tr") ||
                    web_tag_name_is(html, pos, "tbody") ||
                    web_tag_name_is(html, pos, "thead") ||
                    web_tag_name_is(html, pos, "tfoot"))) {
            --*depth;
            changed = 1;
        } else if (web_css_stack_top_is(html, stack, *depth, "tr") &&
                   (web_tag_name_is(html, pos, "tr") ||
                    web_tag_name_is(html, pos, "tbody") ||
                    web_tag_name_is(html, pos, "thead") ||
                    web_tag_name_is(html, pos, "tfoot"))) {
            --*depth;
            changed = 1;
        }
    } while (changed != 0);
}

static void web_css_pop_to_matching_close(const uint8_t *html, uint32_t close_pos, const int32_t *stack, uint32_t *depth) {
    uint32_t close_start = 0;
    uint32_t close_end = 0;

    if (depth == NULL || *depth == 0u || !web_tag_name_range(html, close_pos, &close_start, &close_end)) {
        return;
    }
    for (uint32_t i = *depth; i > 0u; --i) {
        uint32_t open_pos = web_css_nodes[(uint32_t)stack[i - 1u]].tag_pos;
        if (web_tag_name_matches_range(html, open_pos, html, close_start, close_end)) {
            *depth = i - 1u;
            return;
        }
    }
}

static void web_css_release_nodes(void) {
    for (uint32_t i = 0; i < web_css_node_count; ++i) {
        if (web_css_nodes[i].libcss_node_data != NULL) {
            css_libcss_node_data_handler(&web_css_select_handler,
                                         CSS_NODE_DELETED,
                                         NULL,
                                         &web_css_nodes[i],
                                         NULL,
                                         web_css_nodes[i].libcss_node_data);
            web_css_nodes[i].libcss_node_data = NULL;
        }
        if (web_css_nodes[i].inline_style != NULL) {
            css_stylesheet_destroy(web_css_nodes[i].inline_style);
            web_css_nodes[i].inline_style = NULL;
        }
        if (web_css_nodes[i].name != NULL) {
            lwc_string_unref(web_css_nodes[i].name);
            web_css_nodes[i].name = NULL;
        }
        if (web_css_nodes[i].id != NULL) {
            lwc_string_unref(web_css_nodes[i].id);
            web_css_nodes[i].id = NULL;
        }
        for (uint32_t c = 0; c < web_css_nodes[i].class_count; ++c) {
            if (web_css_nodes[i].classes[c] != NULL) {
                lwc_string_unref(web_css_nodes[i].classes[c]);
                web_css_nodes[i].classes[c] = NULL;
            }
            web_css_nodes[i].class_refs[c] = NULL;
        }
        web_css_nodes[i].class_count = 0;
    }
    web_css_node_count = 0;
    web_css_node_saturated = 0;
}

static void web_css_reset(void) {
    web_css_release_nodes();
    if (web_css_select_ctx != NULL) {
        css_select_ctx_destroy(web_css_select_ctx);
        web_css_select_ctx = NULL;
    }
    if (web_css_sheet != NULL) {
        css_stylesheet_destroy(web_css_sheet);
        web_css_sheet = NULL;
    }
    if (web_css_ua_sheet != NULL) {
        css_stylesheet_destroy(web_css_ua_sheet);
        web_css_ua_sheet = NULL;
    }
    web_css_ready = 0;
    web_css_rule_blocks = 0;
}

static int web_css_create_sheet_ex(css_stylesheet **sheet, const char *url, bool inline_style) {
    css_stylesheet_params params;

    if (sheet == NULL) {
        return -1;
    }
    *sheet = NULL;
    memset(&params, 0, sizeof(params));
    params.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    params.level = CSS_LEVEL_21;
    params.charset = "UTF-8";
    params.url = url;
    params.title = "zbrowser";
    params.allow_quirks = true;
    params.inline_style = inline_style;
    params.resolve = web_css_resolve_url;

    return css_stylesheet_create(&params, sheet) == CSS_OK && *sheet != NULL ? 0 : -1;
}

static int web_css_create_sheet(css_stylesheet **sheet, const char *url) {
    return web_css_create_sheet_ex(sheet, url, false);
}

static int web_css_create_inline_sheet(css_stylesheet **sheet) {
    return web_css_create_sheet_ex(sheet, "zbrowser:inline", true);
}

static int web_css_append_sheet_data(css_stylesheet *sheet, const uint8_t *data, uint32_t len) {
    css_error error;

    if (sheet == NULL || data == NULL) {
        return -1;
    }
    if (len != 0) {
        error = css_stylesheet_append_data(sheet, data, len);
        if (error != CSS_OK && error != CSS_NEEDDATA) {
            return -1;
        }
    }
    return css_stylesheet_data_done(sheet) == CSS_OK ? 0 : -1;
}

static void web_css_intern_id(const uint8_t *html, web_css_node_t *node) {
    uint32_t start = 0;
    uint32_t end = 0;
    if (web_attr_value_range_cstr(html, node->tag_pos, "id", &start, &end) && end > start) {
        lwc_intern_string((const char *)(html + start), end - start, &node->id);
    }
}

static void web_css_intern_classes(const uint8_t *html, web_css_node_t *node) {
    uint32_t start = 0;
    uint32_t end = 0;
    if (!web_attr_value_range_cstr(html, node->tag_pos, "class", &start, &end)) {
        return;
    }
    uint32_t pos = start;
    while (pos < end && node->class_count < WEB_CSS_MAX_CLASSES) {
        while (pos < end && web_is_space(html[pos])) {
            ++pos;
        }
        uint32_t part_start = pos;
        while (pos < end && !web_is_space(html[pos])) {
            ++pos;
        }
        if (part_start < pos) {
            lwc_string *klass = NULL;
            if (lwc_intern_string((const char *)(html + part_start), pos - part_start, &klass) == lwc_error_ok &&
                klass != NULL) {
                node->classes[node->class_count] = klass;
                ++node->class_count;
            }
        }
    }
}

static void web_css_parse_inline_style(const uint8_t *html, web_css_node_t *node) {
    uint32_t start = 0;
    uint32_t end = 0;
    if (node == NULL || !web_attr_value_range_cstr(html, node->tag_pos, "style", &start, &end) || end <= start) {
        return;
    }
    web_css_trace_node_step(node, "inline-style-start");
    if (web_css_create_inline_sheet(&node->inline_style) != 0) {
        node->inline_style = NULL;
        return;
    }
    if (web_css_append_sheet_data(node->inline_style, html + start, end - start) != 0) {
        css_stylesheet_destroy(node->inline_style);
        node->inline_style = NULL;
        web_css_trace_node_step(node, "inline-style-fail");
        return;
    }
    web_css_trace_node_step(node, "inline-style-done");
}

static int32_t web_css_add_node(const uint8_t *html,
                                uint32_t tag_pos,
                                int32_t parent,
                                int32_t prev_sibling) {
    uint32_t name_start = 0;
    uint32_t name_end = 0;
    uint32_t node_index;
    if (web_css_node_count >= WEB_CSS_MAX_NODES) {
        web_css_node_saturated = 1;
        return -1;
    }
    if (!web_tag_name_range(html, tag_pos, &name_start, &name_end)) {
        return -1;
    }
    node_index = web_css_node_count;
    web_css_node_t *node = &web_css_nodes[web_css_node_count];
    memset(node, 0, sizeof(*node));
    node->tag_pos = tag_pos;
    node->parent = parent;
    node->prev_sibling = prev_sibling;
    node->next_sibling = -1;
    if (lwc_intern_string((const char *)(html + name_start), name_end - name_start, &node->name) != lwc_error_ok ||
        node->name == NULL) {
        return -1;
    }
    web_css_intern_id(html, node);
    web_css_intern_classes(html, node);
    web_css_parse_inline_style(html, node);
    if (parent >= 0) {
        ++web_css_nodes[(uint32_t)parent].child_count;
    }
    if (prev_sibling >= 0) {
        web_css_nodes[(uint32_t)prev_sibling].next_sibling = (int32_t)node_index;
    }
    ++web_css_node_count;
    return (int32_t)(web_css_node_count - 1u);
}

static void web_css_build_nodes(const uint8_t *html) {
    int32_t stack[WEB_CSS_MAX_STACK];
    int32_t last_child[WEB_CSS_MAX_STACK];
    uint32_t depth = 0;
    uint32_t pos = 0;

    for (uint32_t i = 0; i < WEB_CSS_MAX_STACK; ++i) {
        stack[i] = -1;
        last_child[i] = -1;
    }

    while (html[pos] != 0) {
        if (html[pos] != '<') {
            int saw_text = 0;
            while (html[pos] != 0 && html[pos] != '<') {
                if (!web_is_space(html[pos])) {
                    saw_text = 1;
                }
                ++pos;
            }
            if (saw_text && depth > 0u && stack[depth - 1u] >= 0) {
                ++web_css_nodes[(uint32_t)stack[depth - 1u]].text_child_count;
            }
            continue;
        }
        if (html[pos + 1u] != 0 &&
            html[pos + 2u] != 0 &&
            html[pos + 3u] != 0 &&
            html[pos + 1u] == '!' &&
            html[pos + 2u] == '-' &&
            html[pos + 3u] == '-') {
            pos += 4u;
            while (html[pos] != 0) {
                if (html[pos] == '-' && html[pos + 1u] == '-' && html[pos + 2u] == '>') {
                    pos += 3u;
                    break;
                }
                ++pos;
            }
            continue;
        }
        if (web_is_closing_tag(html, pos)) {
            web_css_pop_to_matching_close(html, pos, stack, &depth);
            pos = web_skip_tag(html, pos);
            continue;
        }
        uint32_t name_start = 0;
        uint32_t name_end = 0;
        if (web_tag_name_range(html, pos, &name_start, &name_end)) {
            web_css_apply_implicit_closes(html, pos, stack, &depth);
            int32_t parent = depth > 0 ? stack[depth - 1u] : -1;
            int32_t index = web_css_add_node(html, pos, parent, last_child[depth]);
            int raw_text = web_tag_name_is(html, pos, "style") || web_tag_name_is(html, pos, "script");
            if (index >= 0) {
                last_child[depth] = index;
                if (depth + 1u < WEB_CSS_MAX_STACK) {
                    last_child[depth + 1u] = -1;
                }
                if (!raw_text && !web_css_void_tag(html, pos) && depth + 1u < WEB_CSS_MAX_STACK) {
                    stack[depth] = index;
                    ++depth;
                }
            }
            if (raw_text) {
                uint32_t after_tag = web_skip_tag(html, pos);
                pos = web_find_named_close(html,
                                           after_tag,
                                           web_tag_name_is(html, pos, "style") ? "style" : "script");
                continue;
            }
        }
        pos = web_skip_tag(html, pos);
    }
}

static web_css_node_t *web_css_find_node(uint32_t tag_pos) {
    uint32_t lo = 0;
    uint32_t hi = web_css_node_count;
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) / 2u);
        if (web_css_nodes[mid].tag_pos == tag_pos) {
            return &web_css_nodes[mid];
        }
        if (web_css_nodes[mid].tag_pos < tag_pos) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static int web_css_prepare_document(const uint8_t *html) {
    css_error error;
    uint32_t pos = 0;
    unsigned long long start_ticks = timer_ticks();
    static const uint8_t ua_css[] =
        "html,body{display:block;color:#202122;background:#fff}"
        "body{margin:8px;font-size:16px;line-height:1.2}"
        "article,aside,details,div,footer,form,header,main,menu,nav,search,section,p,blockquote,ul,ol,li,dl,dt,dd,figure,figcaption,caption,pre,table,tr,h1,h2,h3,h4,h5,h6{display:block}"
        "p{margin:0.5em 0}"
        "h1{font-size:2em;margin:0.67em 0;border-bottom:1px solid #a2a9b1}"
        "h2{font-size:1.5em;margin:0.83em 0;border-bottom:1px solid #c8ccd1}"
        "h3{font-size:1.17em;margin:1em 0}"
        "h4,h5,h6{font-weight:bold;margin:1em 0}"
        "ul,ol,menu{margin:0.5em 0;padding-left:2em}"
        "li{display:list-item}"
        "dl{margin:0.5em 0}"
        "dd{margin-left:2em}"
        "blockquote{margin:0.5em 2em}"
        "figure{margin:0.5em 2em}"
        "fieldset{display:block;border:1px solid #c8ccd1;margin:0.5em 0;padding:0.5em}"
        "legend{display:block;padding:0 0.2em}"
        "pre{white-space:pre;background:#f8f9fa;border:1px solid #c8ccd1;padding:0.5em}"
        "table{display:table;border-collapse:separate;border-spacing:2px;margin:0.5em 0}"
        "thead,tbody,tfoot{display:table-row-group}"
        "tr{display:table-row}"
        "td,th{display:table-cell;padding:0.2em 0.4em}"
        "th{font-weight:bold;text-align:center;background:#eaecf0}"
        "caption{display:table-caption;text-align:center;color:#54595d}"
        "summary{display:list-item}"
        "a:link,a:visited{color:#3366cc;text-decoration:underline}"
        "b,strong{font-weight:bold}"
        "i,em,cite{font-style:italic}"
        "code,kbd,samp{background:#f8f9fa;color:#202122;font-size:0.875em}"
        "mark{background:#fff3cd;color:#202122}"
        "sup{font-size:0.75em;vertical-align:super}"
        "sub{font-size:0.75em;vertical-align:sub}"
        "button,input,select,textarea{font-size:16px}"
        "[hidden],template,input[type=\"hidden\"]{display:none}";

    web_css_trace_reset_stats();
    console_puts("css-trace prepare-reset\n");
    web_css_reset();
    console_puts("css-trace prepare-build-nodes\n");
    web_css_build_nodes(html);
    console_puts("css-trace prepare-built-nodes\n");
    if (web_css_node_saturated != 0) {
        web_css_reset();
        return -2;
    }

    console_puts("css-trace prepare-ua-sheet\n");
    if (web_css_create_sheet(&web_css_ua_sheet, "zbrowser:ua") != 0 ||
        web_css_append_sheet_data(web_css_ua_sheet, ua_css, sizeof(ua_css) - 1u) != 0) {
        web_css_reset();
        return -1;
    }
    console_puts("css-trace prepare-ua-sheet-done\n");
    console_puts("css-trace prepare-select-ctx\n");
    if (css_select_ctx_create(&web_css_select_ctx) != CSS_OK || web_css_select_ctx == NULL) {
        web_css_reset();
        return -1;
    }
    if (css_select_ctx_append_sheet(web_css_select_ctx, web_css_ua_sheet, CSS_ORIGIN_UA, "screen") != CSS_OK) {
        web_css_reset();
        return -1;
    }
    console_puts("css-trace prepare-select-ctx-done\n");

    console_puts("css-trace prepare-doc-sheet\n");
    if (web_css_create_sheet(&web_css_sheet, "zbrowser:document") != 0) {
        web_css_reset();
        return -1;
    }
    console_puts("css-trace prepare-doc-sheet-done\n");

    while (html[pos] != 0) {
        if (html[pos] == '<' && !web_is_closing_tag(html, pos) && web_tag_name_is(html, pos, "style")) {
            uint32_t style_start = web_skip_tag(html, pos);
            uint32_t style_end = web_find_style_close(html, style_start);
            if (style_end > style_start) {
                console_puts("css-trace prepare-style-block\n");
                error = css_stylesheet_append_data(web_css_sheet, html + style_start, style_end - style_start);
                if (error != CSS_OK && error != CSS_NEEDDATA) {
                    web_css_reset();
                    return -1;
                }
                ++web_css_rule_blocks;
            }
            pos = style_end;
        } else {
            ++pos;
        }
    }

    if (web_css_rule_blocks == 0) {
        css_stylesheet_destroy(web_css_sheet);
        web_css_sheet = NULL;
        web_css_ready = 1;
        console_puts("css-trace prepare-summary blocks=0 nodes=");
        console_put_dec64(web_css_node_count);
        console_puts(" ticks=");
        console_put_dec64(timer_ticks() - start_ticks);
        console_puts("\n");
        return 0;
    }
    console_puts("css-trace prepare-style-done\n");
    if (css_stylesheet_data_done(web_css_sheet) != CSS_OK) {
        web_css_reset();
        return -1;
    }
    console_puts("css-trace prepare-style-finalized\n");
    if (css_select_ctx_append_sheet(web_css_select_ctx, web_css_sheet, CSS_ORIGIN_AUTHOR, "screen") != CSS_OK) {
        web_css_reset();
        return -1;
    }
    console_puts("css-trace prepare-ready\n");
    web_css_ready = 1;
    console_puts("css-trace prepare-summary blocks=");
    console_put_dec64(web_css_rule_blocks);
    console_puts(" nodes=");
    console_put_dec64(web_css_node_count);
    console_puts(" ticks=");
    console_put_dec64(timer_ticks() - start_ticks);
    console_puts("\n");
    return (int)web_css_rule_blocks;
}

static void web_prepared_cache_reset(const uint8_t *html, uint32_t viewport_width, uint32_t viewport_height) {
    web_cached_html = html;
    web_cached_viewport_width = viewport_width;
    web_cached_viewport_height = viewport_height;
    web_cached_rule_count = 0;
    web_cached_rule_saturated = 0;
}

static void web_prepared_cache_add(uint32_t selector_start,
                                   uint32_t selector_end,
                                   uint32_t block_start,
                                   uint32_t block_end) {
    if (web_cached_rule_count >= WEB_STYLE_MAX_CACHED_RULES) {
        web_cached_rule_saturated = 1;
        return;
    }
    web_cached_rules[web_cached_rule_count].selector_start = selector_start;
    web_cached_rules[web_cached_rule_count].selector_end = selector_end;
    web_cached_rules[web_cached_rule_count].block_start = block_start;
    web_cached_rules[web_cached_rule_count].block_end = block_end;
    ++web_cached_rule_count;
}

static void web_prepare_css_rules(const uint8_t *css,
                                  uint32_t start,
                                  uint32_t end,
                                  uint32_t viewport_width,
                                  uint32_t viewport_height,
                                  uint32_t depth) {
    if (depth > 4u) {
        return;
    }
    uint32_t pos = start;
    while (pos < end) {
        pos = web_css_skip_comment(css, pos, end);
        while (pos < end && web_is_space(css[pos])) {
            ++pos;
        }
        if (pos >= end) {
            break;
        }
        uint32_t selector_start = pos;
        uint8_t quote = 0;
        uint32_t paren_depth = 0;
        uint32_t bracket_depth = 0;
        while (pos < end) {
            uint8_t ch = css[pos];
            if (quote != 0) {
                if (ch == quote) {
                    quote = 0;
                }
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
            } else if (ch == '(') {
                ++paren_depth;
            } else if (ch == ')' && paren_depth > 0) {
                --paren_depth;
            } else if (ch == '[') {
                ++bracket_depth;
            } else if (ch == ']' && bracket_depth > 0) {
                --bracket_depth;
            } else if (ch == '{' && paren_depth == 0 && bracket_depth == 0) {
                break;
            } else if (ch == ';' && paren_depth == 0 && bracket_depth == 0 &&
                       web_range_starts_cstr_ci(css, web_trim_start(css, selector_start, pos), pos, "@")) {
                break;
            }
            ++pos;
        }
        if (pos >= end || css[pos] != '{') {
            ++pos;
            continue;
        }
        uint32_t selector_end = web_trim_end(css, selector_start, pos);
        uint32_t selector_clean_start = web_trim_start(css, selector_start, selector_end);
        uint32_t block_start = pos + 1u;
        uint32_t block_end = web_css_find_block_end(css, block_start, end);
        if (selector_clean_start < selector_end &&
            web_range_starts_cstr_ci(css, selector_clean_start, selector_end, "@")) {
            if (web_range_starts_cstr_ci(css, selector_clean_start, selector_end, "@media")) {
                if (web_media_query_matches(css,
                                            selector_clean_start + 6u,
                                            selector_end,
                                            viewport_width,
                                            viewport_height)) {
                    web_prepare_css_rules(css, block_start, block_end, viewport_width, viewport_height, depth + 1u);
                }
            } else if (web_range_starts_cstr_ci(css, selector_clean_start, selector_end, "@supports") ||
                       web_range_starts_cstr_ci(css, selector_clean_start, selector_end, "@layer")) {
                web_prepare_css_rules(css, block_start, block_end, viewport_width, viewport_height, depth + 1u);
            }
        } else if (selector_clean_start < selector_end) {
            web_prepared_cache_add(selector_clean_start, selector_end, block_start, block_end);
        }
        pos = block_end;
        if (pos < end && css[pos] == '}') {
            ++pos;
        }
    }
}

static void web_apply_prepared_rules(const uint8_t *html,
                                     uint32_t tag_pos,
                                     web_style_state_t *state,
                                     uint32_t viewport_width,
                                     uint32_t viewport_height) {
    for (uint32_t rule = 0; rule < web_cached_rule_count; ++rule) {
        uint32_t specificity = 0;
        if (web_selector_list_matches(html,
                                      tag_pos,
                                      html,
                                      web_cached_rules[rule].selector_start,
                                      web_cached_rules[rule].selector_end,
                                      &specificity)) {
            web_apply_declarations(html,
                                   web_cached_rules[rule].block_start,
                                   web_cached_rules[rule].block_end,
                                   state,
                                   specificity,
                                   viewport_width,
                                   viewport_height);
        }
    }
}

int web_style_prepare_document(const uint8_t *html, uint32_t viewport_width, uint32_t viewport_height) {
    int css_status;
    unsigned long long start_ticks = timer_ticks();
    unsigned long long prepare_done_ticks;
    unsigned long long precompute_done_ticks;
    unsigned long long rulecache_done_ticks;

    if (html == NULL) {
        web_prepared_cache_reset(NULL, 0, 0);
        web_css_reset();
        return -1;
    }
    web_prepared_cache_reset(html, viewport_width, viewport_height);
    console_puts("css-trace style-prepare-enter\n");
    css_status = web_css_prepare_document(html);
    if (css_status < 0) {
        web_prepared_cache_reset(NULL, 0, 0);
        return css_status;
    }
    prepare_done_ticks = timer_ticks();
    console_puts("css-trace style-prepare-doc-ready\n");
    web_css_precompute_styles(html, viewport_width, viewport_height);
    precompute_done_ticks = timer_ticks();
    console_puts("css-trace style-prepare-precompute-done\n");
    uint32_t pos = 0;
    while (html[pos] != 0) {
        if (html[pos] == '<' && !web_is_closing_tag(html, pos) && web_tag_name_is(html, pos, "style")) {
            uint32_t style_start = web_skip_tag(html, pos);
            uint32_t style_end = web_find_style_close(html, style_start);
            web_prepare_css_rules(html, style_start, style_end, viewport_width, viewport_height, 0);
            pos = style_end;
        } else {
            ++pos;
        }
    }
    rulecache_done_ticks = timer_ticks();
    console_puts("css-trace style-prepare-summary rules=");
    console_put_dec64(web_cached_rule_count);
    console_puts(" prepare_ticks=");
    console_put_dec64(prepare_done_ticks - start_ticks);
    console_puts(" precompute_ticks=");
    console_put_dec64(precompute_done_ticks - prepare_done_ticks);
    console_puts(" rulecache_ticks=");
    console_put_dec64(rulecache_done_ticks - precompute_done_ticks);
    console_puts(" total_ticks=");
    console_put_dec64(rulecache_done_ticks - start_ticks);
    console_puts(" trace_lines=");
    console_put_dec64(web_css_trace_lines_emitted);
    console_puts(" trace_suppressed=");
    console_put_dec64(web_css_trace_lines_suppressed);
    console_puts(" trace_ticks=");
    console_put_dec64(web_css_trace_total_ticks);
    console_puts("\n");
    return web_cached_rule_saturated ? -3 : (int)web_cached_rule_count;
}

static void web_scan_style_blocks(const uint8_t *html,
                                  uint32_t tag_pos,
                                  web_style_state_t *state,
                                  uint32_t viewport_width,
                                  uint32_t viewport_height) {
    uint32_t pos = 0;
    while (html[pos] != 0) {
        if (html[pos] == '<' && !web_is_closing_tag(html, pos) && web_tag_name_is(html, pos, "style")) {
            uint32_t style_start = web_skip_tag(html, pos);
            uint32_t style_end = web_find_style_close(html, style_start);
            web_scan_css_rules(html, style_start, style_end, html, tag_pos, state, viewport_width, viewport_height, 0);
            pos = style_end;
        } else {
            ++pos;
        }
    }
}

static void web_state_init(web_style_state_t *state) {
    state->style.flags = 0;
    state->style.text_align = WEB_STYLE_ALIGN_LEFT;
    state->style.position = WEB_STYLE_POS_STATIC;
    state->style.display = WEB_STYLE_DISPLAY_INLINE;
    state->style.float_side = WEB_STYLE_FLOAT_NONE;
    state->style.font_weight = WEB_STYLE_FONT_NORMAL;
    state->style.font_style = WEB_STYLE_FONT_STYLE_NORMAL;
    state->style.left = 0;
    state->style.right = 0;
    state->style.top = 0;
    state->style.bottom = 0;
    state->style.width = 0;
    state->style.height = 0;
    state->style.min_width = 0;
    state->style.max_width = 0;
    state->style.min_height = 0;
    state->style.max_height = 0;
    state->style.color = 0;
    state->style.background_color = 0;
    state->style.margin_left = 0;
    state->style.margin_right = 0;
    state->style.margin_top = 0;
    state->style.margin_bottom = 0;
    state->style.padding_left = 0;
    state->style.padding_right = 0;
    state->style.padding_top = 0;
    state->style.padding_bottom = 0;
    state->style.border_left = 0;
    state->style.border_right = 0;
    state->style.border_top = 0;
    state->style.border_bottom = 0;
    state->style.border_color = 0;
    state->style.font_size = 16;
    state->style.line_height = 19;
    state->style.text_transform = WEB_STYLE_TEXT_TRANSFORM_NONE;
    state->style.white_space = WEB_STYLE_WHITE_SPACE_NORMAL;
    state->style.list_style_type = WEB_STYLE_LIST_DISC;
    state->style.overflow_x = WEB_STYLE_OVERFLOW_VISIBLE;
    state->style.overflow_y = WEB_STYLE_OVERFLOW_VISIBLE;
    state->style.box_sizing = WEB_STYLE_BOX_CONTENT_BOX;
    state->style.border_collapse = WEB_STYLE_BORDER_SEPARATE;
    state->style.border_spacing_h = 2;
    state->style.border_spacing_v = 2;
    state->style.text_indent = 0;
    state->style.clear_side = WEB_STYLE_CLEAR_NONE;
    state->style.vertical_align = WEB_STYLE_VERTICAL_BASELINE;
    state->style.list_style_position = WEB_STYLE_LIST_POSITION_OUTSIDE;
    state->style.caption_side = WEB_STYLE_CAPTION_TOP;
    state->style.direction = WEB_STYLE_DIRECTION_LTR;
    state->style.table_layout = WEB_STYLE_TABLE_LAYOUT_AUTO;
    state->style.empty_cells = WEB_STYLE_EMPTY_CELLS_SHOW;
    state->style.opacity = 255;
    state->style.z_index = 0;
    state->style.letter_spacing = 0;
    state->style.word_spacing = 0;
    state->style.background_repeat = WEB_STYLE_BG_REPEAT;
    state->style.background_position_x = 0;
    state->style.background_position_y = 0;
    for (uint32_t i = 0; i < WEB_PROP_COUNT; ++i) {
        state->score[i] = 0;
    }
    state->order = 1;
}

static uint32_t web_css_color_to_rgb(css_color color) {
    return color & 0x00ffffffu;
}

static int web_css_color_is_visible(css_color color) {
    return (color & 0xff000000u) != 0;
}

static uint32_t web_css_length_to_px(const css_computed_style *computed,
                                     const css_unit_ctx *unit_ctx,
                                     css_fixed length,
                                     css_unit unit,
                                     uint32_t percent_base) {
    css_fixed px;
    if (unit == CSS_UNIT_PCT) {
        int64_t value = ((int64_t)percent_base * (int64_t)length) / (100ll << CSS_RADIX_POINT);
        if (value < 0) {
            return 0;
        }
        if (value > 1000000) {
            return 1000000;
        }
        return (uint32_t)value;
    }
    px = css_unit_len2device_px(computed, unit_ctx, length, unit);
    if (px < 0) {
        return 0;
    }
    return (uint32_t)FIXTOINT(px);
}

static void web_css_set_score(web_style_state_t *state, uint32_t property) {
    state->score[property] = web_cascade_score(100, 0, state->order);
}

static int web_css_border_style_visible(uint8_t type) {
    return type != CSS_BORDER_STYLE_INHERIT &&
           type != CSS_BORDER_STYLE_NONE &&
           type != CSS_BORDER_STYLE_HIDDEN;
}

static uint32_t web_css_border_width_to_px(const css_computed_style *computed,
                                           const css_unit_ctx *unit_ctx,
                                           uint8_t type,
                                           css_fixed length,
                                           css_unit unit,
                                           uint32_t percent_base) {
    if (type == CSS_BORDER_WIDTH_THIN) {
        return 1;
    }
    if (type == CSS_BORDER_WIDTH_MEDIUM) {
        return 2;
    }
    if (type == CSS_BORDER_WIDTH_THICK) {
        return 4;
    }
    if (type == CSS_BORDER_WIDTH_WIDTH) {
        return web_css_length_to_px(computed, unit_ctx, length, unit, percent_base);
    }
    return 0;
}

static uint32_t web_css_font_size_to_px(uint8_t type,
                                        const css_computed_style *computed,
                                        const css_unit_ctx *unit_ctx,
                                        css_fixed length,
                                        css_unit unit) {
    if (type == CSS_FONT_SIZE_XX_SMALL) {
        return 9;
    }
    if (type == CSS_FONT_SIZE_X_SMALL) {
        return 10;
    }
    if (type == CSS_FONT_SIZE_SMALL) {
        return 13;
    }
    if (type == CSS_FONT_SIZE_MEDIUM) {
        return 16;
    }
    if (type == CSS_FONT_SIZE_LARGE) {
        return 18;
    }
    if (type == CSS_FONT_SIZE_X_LARGE) {
        return 24;
    }
    if (type == CSS_FONT_SIZE_XX_LARGE) {
        return 32;
    }
    if (type == CSS_FONT_SIZE_DIMENSION) {
        uint32_t px = web_css_length_to_px(computed, unit_ctx, length, unit, 16);
        if (px < 6) {
            return 6;
        }
        if (px > 72) {
            return 72;
        }
        return px;
    }
    return 16;
}

static uint32_t web_css_overflow_value(uint8_t type) {
    if (type == CSS_OVERFLOW_HIDDEN) {
        return WEB_STYLE_OVERFLOW_HIDDEN;
    }
    if (type == CSS_OVERFLOW_SCROLL) {
        return WEB_STYLE_OVERFLOW_SCROLL;
    }
    if (type == CSS_OVERFLOW_AUTO) {
        return WEB_STYLE_OVERFLOW_AUTO;
    }
    return WEB_STYLE_OVERFLOW_VISIBLE;
}

static uint32_t web_css_list_style_value(uint8_t type) {
    if (type == CSS_LIST_STYLE_TYPE_NONE) {
        return WEB_STYLE_LIST_NONE;
    }
    if (type == CSS_LIST_STYLE_TYPE_CIRCLE) {
        return WEB_STYLE_LIST_CIRCLE;
    }
    if (type == CSS_LIST_STYLE_TYPE_SQUARE) {
        return WEB_STYLE_LIST_SQUARE;
    }
    if (type == CSS_LIST_STYLE_TYPE_DECIMAL ||
        type == CSS_LIST_STYLE_TYPE_DECIMAL_LEADING_ZERO ||
        type == CSS_LIST_STYLE_TYPE_LOWER_ROMAN ||
        type == CSS_LIST_STYLE_TYPE_UPPER_ROMAN ||
        type == CSS_LIST_STYLE_TYPE_LOWER_ALPHA ||
        type == CSS_LIST_STYLE_TYPE_UPPER_ALPHA ||
        type == CSS_LIST_STYLE_TYPE_LOWER_LATIN ||
        type == CSS_LIST_STYLE_TYPE_UPPER_LATIN) {
        return WEB_STYLE_LIST_DECIMAL;
    }
    return WEB_STYLE_LIST_DISC;
}

static uint32_t web_css_clear_value(uint8_t type) {
    if (type == CSS_CLEAR_LEFT) {
        return WEB_STYLE_CLEAR_LEFT;
    }
    if (type == CSS_CLEAR_RIGHT) {
        return WEB_STYLE_CLEAR_RIGHT;
    }
    if (type == CSS_CLEAR_BOTH) {
        return WEB_STYLE_CLEAR_BOTH;
    }
    return WEB_STYLE_CLEAR_NONE;
}

static uint32_t web_css_vertical_align_value(uint8_t type) {
    if (type == CSS_VERTICAL_ALIGN_SUB) {
        return WEB_STYLE_VERTICAL_SUB;
    }
    if (type == CSS_VERTICAL_ALIGN_SUPER) {
        return WEB_STYLE_VERTICAL_SUPER;
    }
    if (type == CSS_VERTICAL_ALIGN_TOP || type == CSS_VERTICAL_ALIGN_TEXT_TOP) {
        return WEB_STYLE_VERTICAL_TOP;
    }
    if (type == CSS_VERTICAL_ALIGN_MIDDLE) {
        return WEB_STYLE_VERTICAL_MIDDLE;
    }
    if (type == CSS_VERTICAL_ALIGN_BOTTOM || type == CSS_VERTICAL_ALIGN_TEXT_BOTTOM) {
        return WEB_STYLE_VERTICAL_BOTTOM;
    }
    return WEB_STYLE_VERTICAL_BASELINE;
}

static uint32_t web_css_background_repeat_value(uint8_t type) {
    if (type == CSS_BACKGROUND_REPEAT_REPEAT_X) {
        return WEB_STYLE_BG_REPEAT_X;
    }
    if (type == CSS_BACKGROUND_REPEAT_REPEAT_Y) {
        return WEB_STYLE_BG_REPEAT_Y;
    }
    if (type == CSS_BACKGROUND_REPEAT_NO_REPEAT) {
        return WEB_STYLE_BG_NO_REPEAT;
    }
    return WEB_STYLE_BG_REPEAT;
}

static uint32_t web_css_fixed_unit_to_byte(css_fixed value) {
    int64_t scaled = ((int64_t)value * 255ll) >> CSS_RADIX_POINT;
    if (scaled < 0) {
        return 0;
    }
    if (scaled > 255) {
        return 255;
    }
    return (uint32_t)scaled;
}

static void web_css_set_computed_length(web_style_state_t *state,
                                        uint32_t property,
                                        uint32_t flag,
                                        uint32_t *field,
                                        uint32_t value) {
    state->style.flags |= flag;
    *field = value;
    web_css_set_score(state, property);
}

static void web_css_apply_computed_border_side(web_style_state_t *state,
                                               uint32_t property,
                                               uint32_t flag,
                                               uint32_t *field,
                                               uint8_t border_style,
                                               uint8_t border_width_type,
                                               css_fixed length,
                                               css_unit unit,
                                               const css_computed_style *computed,
                                               const css_unit_ctx *unit_ctx,
                                               uint32_t percent_base) {
    if (web_css_border_style_visible(border_style)) {
        uint32_t px = web_css_border_width_to_px(computed, unit_ctx, border_width_type, length, unit, percent_base);
        if (px != 0) {
            web_css_set_computed_length(state, property, flag, field, px);
        }
    }
}

static void web_css_apply_computed_border_color(web_style_state_t *state,
                                                uint8_t border_color_type,
                                                css_color border_color,
                                                uint32_t current_color,
                                                int have_current_color) {
    if (border_color_type == CSS_BORDER_COLOR_COLOR && web_css_color_is_visible(border_color)) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_BORDER_COLOR;
        state->style.border_color = web_css_color_to_rgb(border_color);
        web_css_set_score(state, WEB_PROP_BORDER_COLOR);
    } else if (border_color_type == CSS_BORDER_COLOR_CURRENT_COLOR && have_current_color) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_BORDER_COLOR;
        state->style.border_color = current_color;
        web_css_set_score(state, WEB_PROP_BORDER_COLOR);
    }
}

static void web_css_apply_computed_style(const web_css_node_t *node,
                                         web_style_state_t *state,
                                         const css_computed_style *computed,
                                         const css_unit_ctx *unit_ctx,
                                         uint32_t viewport_width,
                                         uint32_t viewport_height) {
    css_color color = 0;
    css_color border_color = 0;
    css_fixed length = 0;
    css_fixed hlength = 0;
    css_fixed vlength = 0;
    css_fixed fixed_value = 0;
    css_unit unit = CSS_UNIT_PX;
    css_unit hunit = CSS_UNIT_PX;
    css_unit vunit = CSS_UNIT_PX;
    int px = 0;
    int32_t z_index = 0;
    uint32_t current_color = 0x000000u;
    int have_current_color = 1;
    int margin_left_auto = 0;
    int margin_right_auto = 0;
    uint8_t type;

    web_css_trace_node_step(node, "apply-start");

    type = css_computed_display(computed, false);
    state->style.display = WEB_STYLE_DISPLAY_INLINE;
    if (type == CSS_DISPLAY_NONE) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_NONE;
        web_css_set_score(state, WEB_PROP_DISPLAY);
    } else if (type == CSS_DISPLAY_LIST_ITEM) {
        state->style.display = WEB_STYLE_DISPLAY_LIST_ITEM;
        web_css_set_score(state, WEB_PROP_DISPLAY);
    } else if (type == CSS_DISPLAY_BLOCK) {
        state->style.display = WEB_STYLE_DISPLAY_BLOCK;
        web_css_set_score(state, WEB_PROP_DISPLAY);
    } else if (type == CSS_DISPLAY_FLEX || type == CSS_DISPLAY_INLINE_FLEX) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_FLEX;
        state->style.display = WEB_STYLE_DISPLAY_FLEX;
        web_css_set_score(state, WEB_PROP_DISPLAY);
    } else if (type == CSS_DISPLAY_TABLE) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_TABLE;
        state->style.display = WEB_STYLE_DISPLAY_TABLE;
        web_css_set_score(state, WEB_PROP_DISPLAY);
    } else if (type == CSS_DISPLAY_TABLE_ROW ||
               type == CSS_DISPLAY_TABLE_ROW_GROUP ||
               type == CSS_DISPLAY_TABLE_HEADER_GROUP ||
               type == CSS_DISPLAY_TABLE_FOOTER_GROUP) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_TABLE_ROW;
        state->style.display = WEB_STYLE_DISPLAY_TABLE_ROW;
        web_css_set_score(state, WEB_PROP_DISPLAY);
    } else if (type == CSS_DISPLAY_TABLE_CELL ||
               type == CSS_DISPLAY_TABLE_CAPTION) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_TABLE_CELL;
        state->style.display = WEB_STYLE_DISPLAY_TABLE_CELL;
        web_css_set_score(state, WEB_PROP_DISPLAY);
    }

    type = css_computed_float(computed);
    state->style.float_side = WEB_STYLE_FLOAT_NONE;
    if (type == CSS_FLOAT_LEFT) {
        state->style.flags |= WEB_STYLE_FLAG_FLOAT_LEFT;
        state->style.float_side = WEB_STYLE_FLOAT_LEFT;
        web_css_set_score(state, WEB_PROP_FLOAT);
    } else if (type == CSS_FLOAT_RIGHT) {
        state->style.flags |= WEB_STYLE_FLAG_FLOAT_RIGHT;
        state->style.float_side = WEB_STYLE_FLOAT_RIGHT;
        web_css_set_score(state, WEB_PROP_FLOAT);
    }

    type = css_computed_visibility(computed);
    if (type == CSS_VISIBILITY_HIDDEN || type == CSS_VISIBILITY_COLLAPSE) {
        state->style.flags |= WEB_STYLE_FLAG_VISIBILITY_HIDDEN;
        web_css_set_score(state, WEB_PROP_VISIBILITY);
    }

    type = css_computed_text_align(computed);
    if (type == CSS_TEXT_ALIGN_CENTER || type == CSS_TEXT_ALIGN_LIBCSS_CENTER) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_TEXT_ALIGN;
        state->style.text_align = WEB_STYLE_ALIGN_CENTER;
        web_css_set_score(state, WEB_PROP_TEXT_ALIGN);
    } else if (type == CSS_TEXT_ALIGN_RIGHT || type == CSS_TEXT_ALIGN_LIBCSS_RIGHT) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_TEXT_ALIGN;
        state->style.text_align = WEB_STYLE_ALIGN_RIGHT;
        web_css_set_score(state, WEB_PROP_TEXT_ALIGN);
    } else if (type == CSS_TEXT_ALIGN_LEFT || type == CSS_TEXT_ALIGN_LIBCSS_LEFT) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_TEXT_ALIGN;
        state->style.text_align = WEB_STYLE_ALIGN_LEFT;
        web_css_set_score(state, WEB_PROP_TEXT_ALIGN);
    }

    if (css_computed_color(computed, &color) == CSS_COLOR_COLOR && web_css_color_is_visible(color)) {
        current_color = web_css_color_to_rgb(color);
        state->style.flags |= WEB_STYLE_FLAG_HAS_COLOR;
        state->style.color = current_color;
        web_css_set_score(state, WEB_PROP_COLOR);
    }

    type = css_computed_background_color(computed, &color);
    if (type == CSS_BACKGROUND_COLOR_COLOR && web_css_color_is_visible(color)) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_BG_COLOR;
        state->style.background_color = web_css_color_to_rgb(color);
        web_css_set_score(state, WEB_PROP_BACKGROUND_COLOR);
    } else if (type == CSS_BACKGROUND_COLOR_CURRENT_COLOR && have_current_color) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_BG_COLOR;
        state->style.background_color = current_color;
        web_css_set_score(state, WEB_PROP_BACKGROUND_COLOR);
    }

    type = css_computed_position(computed);
    if (type == CSS_POSITION_ABSOLUTE || type == CSS_POSITION_FIXED) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_POSITION;
        state->style.position = type == CSS_POSITION_FIXED ? WEB_STYLE_POS_FIXED : WEB_STYLE_POS_ABSOLUTE;
        web_css_set_score(state, WEB_PROP_POSITION);
    }

    if (css_computed_left(computed, &length, &unit) == CSS_LEFT_SET) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_LEFT;
        state->style.left = web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width);
        web_css_set_score(state, WEB_PROP_LEFT);
    }
    if (css_computed_right(computed, &length, &unit) == CSS_RIGHT_SET) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_RIGHT;
        state->style.right = web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width);
        web_css_set_score(state, WEB_PROP_RIGHT);
    }
    if (css_computed_top(computed, &length, &unit) == CSS_TOP_SET) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_TOP;
        state->style.top = web_css_length_to_px(computed, unit_ctx, length, unit, viewport_height);
        web_css_set_score(state, WEB_PROP_TOP);
    }
    if (css_computed_bottom(computed, &length, &unit) == CSS_BOTTOM_SET) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_BOTTOM;
        state->style.bottom = web_css_length_to_px(computed, unit_ctx, length, unit, viewport_height);
        web_css_set_score(state, WEB_PROP_BOTTOM);
    }
    if (css_computed_width_px(computed, unit_ctx, (int)viewport_width, &px) == CSS_WIDTH_SET && px >= 0) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_WIDTH;
        state->style.width = (uint32_t)px;
        web_css_set_score(state, WEB_PROP_WIDTH);
    }
    if (css_computed_height(computed, &length, &unit) == CSS_HEIGHT_SET) {
        state->style.flags |= WEB_STYLE_FLAG_HAS_HEIGHT;
        state->style.height = web_css_length_to_px(computed, unit_ctx, length, unit, viewport_height);
        web_css_set_score(state, WEB_PROP_HEIGHT);
    }
    if (css_computed_min_width(computed, &length, &unit) == CSS_MIN_WIDTH_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_MIN_WIDTH,
                                    WEB_STYLE_FLAG_HAS_MIN_WIDTH,
                                    &state->style.min_width,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    }
    if (css_computed_max_width(computed, &length, &unit) == CSS_MAX_WIDTH_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_MAX_WIDTH,
                                    WEB_STYLE_FLAG_HAS_MAX_WIDTH,
                                    &state->style.max_width,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    }
    if (css_computed_min_height(computed, &length, &unit) == CSS_MIN_HEIGHT_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_MIN_HEIGHT,
                                    WEB_STYLE_FLAG_HAS_MIN_HEIGHT,
                                    &state->style.min_height,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_height));
    }
    if (css_computed_max_height(computed, &length, &unit) == CSS_MAX_HEIGHT_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_MAX_HEIGHT,
                                    WEB_STYLE_FLAG_HAS_MAX_HEIGHT,
                                    &state->style.max_height,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_height));
    }

    web_css_trace_node_step(node, "apply-box");

    type = css_computed_margin_left(computed, &length, &unit);
    if (type == CSS_MARGIN_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_MARGIN_LEFT,
                                    WEB_STYLE_FLAG_HAS_MARGIN,
                                    &state->style.margin_left,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    } else if (type == CSS_MARGIN_AUTO) {
        margin_left_auto = 1;
    }
    type = css_computed_margin_right(computed, &length, &unit);
    if (type == CSS_MARGIN_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_MARGIN_RIGHT,
                                    WEB_STYLE_FLAG_HAS_MARGIN,
                                    &state->style.margin_right,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    } else if (type == CSS_MARGIN_AUTO) {
        margin_right_auto = 1;
    }
    if (css_computed_margin_top(computed, &length, &unit) == CSS_MARGIN_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_MARGIN_TOP,
                                    WEB_STYLE_FLAG_HAS_MARGIN,
                                    &state->style.margin_top,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    }
    if (css_computed_margin_bottom(computed, &length, &unit) == CSS_MARGIN_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_MARGIN_BOTTOM,
                                    WEB_STYLE_FLAG_HAS_MARGIN,
                                    &state->style.margin_bottom,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    }
    if (margin_left_auto != 0 || margin_right_auto != 0) {
        state->style.flags |= WEB_STYLE_FLAG_MARGIN_AUTO_X;
        web_css_set_score(state, WEB_PROP_MARGIN_AUTO_X);
        if (margin_left_auto != 0 && margin_right_auto != 0) {
            state->style.flags |= WEB_STYLE_FLAG_CENTER_X;
            web_css_set_score(state, WEB_PROP_CENTER_X);
        }
    }

    if (css_computed_padding_left(computed, &length, &unit) == CSS_PADDING_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_PADDING_LEFT,
                                    WEB_STYLE_FLAG_HAS_PADDING,
                                    &state->style.padding_left,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    }
    if (css_computed_padding_right(computed, &length, &unit) == CSS_PADDING_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_PADDING_RIGHT,
                                    WEB_STYLE_FLAG_HAS_PADDING,
                                    &state->style.padding_right,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    }
    if (css_computed_padding_top(computed, &length, &unit) == CSS_PADDING_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_PADDING_TOP,
                                    WEB_STYLE_FLAG_HAS_PADDING,
                                    &state->style.padding_top,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    }
    if (css_computed_padding_bottom(computed, &length, &unit) == CSS_PADDING_SET) {
        web_css_set_computed_length(state,
                                    WEB_PROP_PADDING_BOTTOM,
                                    WEB_STYLE_FLAG_HAS_PADDING,
                                    &state->style.padding_bottom,
                                    web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width));
    }

    type = css_computed_border_left_width(computed, &length, &unit);
    web_css_apply_computed_border_side(state,
                                       WEB_PROP_BORDER_LEFT,
                                       WEB_STYLE_FLAG_HAS_BORDER,
                                       &state->style.border_left,
                                       css_computed_border_left_style(computed),
                                       type,
                                       length,
                                       unit,
                                       computed,
                                       unit_ctx,
                                       viewport_width);
    type = css_computed_border_right_width(computed, &length, &unit);
    web_css_apply_computed_border_side(state,
                                       WEB_PROP_BORDER_RIGHT,
                                       WEB_STYLE_FLAG_HAS_BORDER,
                                       &state->style.border_right,
                                       css_computed_border_right_style(computed),
                                       type,
                                       length,
                                       unit,
                                       computed,
                                       unit_ctx,
                                       viewport_width);
    type = css_computed_border_top_width(computed, &length, &unit);
    web_css_apply_computed_border_side(state,
                                       WEB_PROP_BORDER_TOP,
                                       WEB_STYLE_FLAG_HAS_BORDER,
                                       &state->style.border_top,
                                       css_computed_border_top_style(computed),
                                       type,
                                       length,
                                       unit,
                                       computed,
                                       unit_ctx,
                                       viewport_width);
    type = css_computed_border_bottom_width(computed, &length, &unit);
    web_css_apply_computed_border_side(state,
                                       WEB_PROP_BORDER_BOTTOM,
                                       WEB_STYLE_FLAG_HAS_BORDER,
                                       &state->style.border_bottom,
                                       css_computed_border_bottom_style(computed),
                                       type,
                                       length,
                                       unit,
                                       computed,
                                       unit_ctx,
                                       viewport_width);
    if (state->style.border_top != 0) {
        type = css_computed_border_top_color(computed, &border_color);
        web_css_apply_computed_border_color(state, type, border_color, current_color, have_current_color);
    } else if (state->style.border_right != 0) {
        type = css_computed_border_right_color(computed, &border_color);
        web_css_apply_computed_border_color(state, type, border_color, current_color, have_current_color);
    } else if (state->style.border_bottom != 0) {
        type = css_computed_border_bottom_color(computed, &border_color);
        web_css_apply_computed_border_color(state, type, border_color, current_color, have_current_color);
    } else if (state->style.border_left != 0) {
        type = css_computed_border_left_color(computed, &border_color);
        web_css_apply_computed_border_color(state, type, border_color, current_color, have_current_color);
    }

    web_css_trace_node_step(node, "apply-border");

    type = css_computed_font_weight(computed);
    if (type == CSS_FONT_WEIGHT_BOLD ||
        type == CSS_FONT_WEIGHT_BOLDER ||
        type == CSS_FONT_WEIGHT_600 ||
        type == CSS_FONT_WEIGHT_700 ||
        type == CSS_FONT_WEIGHT_800 ||
        type == CSS_FONT_WEIGHT_900) {
        state->style.flags |= WEB_STYLE_FLAG_FONT_BOLD;
        state->style.font_weight = WEB_STYLE_FONT_BOLD;
        web_css_set_score(state, WEB_PROP_FONT_WEIGHT);
    }

    type = css_computed_font_style(computed);
    if (type == CSS_FONT_STYLE_ITALIC || type == CSS_FONT_STYLE_OBLIQUE) {
        state->style.flags |= WEB_STYLE_FLAG_FONT_ITALIC;
        state->style.font_style = WEB_STYLE_FONT_STYLE_ITALIC;
        web_css_set_score(state, WEB_PROP_FONT_STYLE);
    }

    type = css_computed_text_decoration(computed);
    if ((type & CSS_TEXT_DECORATION_UNDERLINE) != 0) {
        state->style.flags |= WEB_STYLE_FLAG_TEXT_UNDERLINE;
        web_css_set_score(state, WEB_PROP_TEXT_DECORATION);
    }

    type = css_computed_font_size(computed, &length, &unit);
    state->style.font_size = web_css_font_size_to_px(type, computed, unit_ctx, length, unit);
    web_css_set_score(state, WEB_PROP_FONT_SIZE);

    type = css_computed_line_height(computed, &length, &unit);
    if (type == CSS_LINE_HEIGHT_NORMAL) {
        state->style.line_height = (state->style.font_size * 6u) / 5u;
    } else if (type == CSS_LINE_HEIGHT_NUMBER) {
        uint64_t scaled = (uint64_t)state->style.font_size * (uint64_t)length;
        state->style.line_height = (uint32_t)(scaled >> CSS_RADIX_POINT);
    } else if (type == CSS_LINE_HEIGHT_DIMENSION) {
        state->style.line_height = web_css_length_to_px(computed, unit_ctx, length, unit, state->style.font_size);
    }
    if (state->style.line_height < 10) {
        state->style.line_height = 10;
    }
    if (state->style.line_height > 72) {
        state->style.line_height = 72;
    }
    web_css_set_score(state, WEB_PROP_LINE_HEIGHT);

    web_css_trace_node_step(node, "apply-font");

    type = css_computed_text_transform(computed);
    if (type == CSS_TEXT_TRANSFORM_UPPERCASE) {
        state->style.text_transform = WEB_STYLE_TEXT_TRANSFORM_UPPERCASE;
    } else if (type == CSS_TEXT_TRANSFORM_LOWERCASE) {
        state->style.text_transform = WEB_STYLE_TEXT_TRANSFORM_LOWERCASE;
    } else if (type == CSS_TEXT_TRANSFORM_CAPITALIZE) {
        state->style.text_transform = WEB_STYLE_TEXT_TRANSFORM_CAPITALIZE;
    } else {
        state->style.text_transform = WEB_STYLE_TEXT_TRANSFORM_NONE;
    }
    web_css_set_score(state, WEB_PROP_TEXT_TRANSFORM);

    type = css_computed_white_space(computed);
    if (type == CSS_WHITE_SPACE_PRE) {
        state->style.white_space = WEB_STYLE_WHITE_SPACE_PRE;
    } else if (type == CSS_WHITE_SPACE_NOWRAP) {
        state->style.white_space = WEB_STYLE_WHITE_SPACE_NOWRAP;
    } else if (type == CSS_WHITE_SPACE_PRE_WRAP) {
        state->style.white_space = WEB_STYLE_WHITE_SPACE_PRE_WRAP;
    } else if (type == CSS_WHITE_SPACE_PRE_LINE) {
        state->style.white_space = WEB_STYLE_WHITE_SPACE_PRE_LINE;
    } else {
        state->style.white_space = WEB_STYLE_WHITE_SPACE_NORMAL;
    }
    web_css_set_score(state, WEB_PROP_WHITE_SPACE);

    state->style.list_style_type = web_css_list_style_value(css_computed_list_style_type(computed));
    web_css_set_score(state, WEB_PROP_LIST_STYLE_TYPE);

    state->style.overflow_x = web_css_overflow_value(css_computed_overflow_x(computed));
    state->style.overflow_y = web_css_overflow_value(css_computed_overflow_y(computed));
    web_css_set_score(state, WEB_PROP_OVERFLOW_X);
    web_css_set_score(state, WEB_PROP_OVERFLOW_Y);

    type = css_computed_box_sizing(computed);
    state->style.box_sizing = type == CSS_BOX_SIZING_BORDER_BOX ? WEB_STYLE_BOX_BORDER_BOX : WEB_STYLE_BOX_CONTENT_BOX;
    web_css_set_score(state, WEB_PROP_BOX_SIZING);

    type = css_computed_border_collapse(computed);
    state->style.border_collapse = type == CSS_BORDER_COLLAPSE_COLLAPSE ? WEB_STYLE_BORDER_COLLAPSE : WEB_STYLE_BORDER_SEPARATE;
    web_css_set_score(state, WEB_PROP_BORDER_COLLAPSE);

    type = css_computed_border_spacing(computed, &hlength, &hunit, &vlength, &vunit);
    if (type == CSS_BORDER_SPACING_SET) {
        state->style.border_spacing_h = web_css_length_to_px(computed, unit_ctx, hlength, hunit, viewport_width);
        state->style.border_spacing_v = web_css_length_to_px(computed, unit_ctx, vlength, vunit, viewport_height);
        web_css_set_score(state, WEB_PROP_BORDER_SPACING);
    }

    if (css_computed_text_indent(computed, &length, &unit) == CSS_TEXT_INDENT_SET) {
        state->style.text_indent = web_css_length_to_px(computed, unit_ctx, length, unit, viewport_width);
        web_css_set_score(state, WEB_PROP_TEXT_INDENT);
    }

    state->style.clear_side = web_css_clear_value(css_computed_clear(computed));
    web_css_set_score(state, WEB_PROP_CLEAR);

    type = css_computed_vertical_align(computed, &length, &unit);
    state->style.vertical_align = web_css_vertical_align_value(type);
    web_css_set_score(state, WEB_PROP_VERTICAL_ALIGN);

    type = css_computed_list_style_position(computed);
    state->style.list_style_position = type == CSS_LIST_STYLE_POSITION_INSIDE ?
        WEB_STYLE_LIST_POSITION_INSIDE : WEB_STYLE_LIST_POSITION_OUTSIDE;
    web_css_set_score(state, WEB_PROP_LIST_STYLE_POSITION);

    type = css_computed_caption_side(computed);
    state->style.caption_side = type == CSS_CAPTION_SIDE_BOTTOM ? WEB_STYLE_CAPTION_BOTTOM : WEB_STYLE_CAPTION_TOP;
    web_css_set_score(state, WEB_PROP_CAPTION_SIDE);

    type = css_computed_direction(computed);
    state->style.direction = type == CSS_DIRECTION_RTL ? WEB_STYLE_DIRECTION_RTL : WEB_STYLE_DIRECTION_LTR;
    web_css_set_score(state, WEB_PROP_DIRECTION);

    type = css_computed_table_layout(computed);
    state->style.table_layout = type == CSS_TABLE_LAYOUT_FIXED ? WEB_STYLE_TABLE_LAYOUT_FIXED : WEB_STYLE_TABLE_LAYOUT_AUTO;
    web_css_set_score(state, WEB_PROP_TABLE_LAYOUT);

    type = css_computed_empty_cells(computed);
    state->style.empty_cells = type == CSS_EMPTY_CELLS_HIDE ? WEB_STYLE_EMPTY_CELLS_HIDE : WEB_STYLE_EMPTY_CELLS_SHOW;
    web_css_set_score(state, WEB_PROP_EMPTY_CELLS);

    web_css_trace_node_step(node, "apply-layout");

    if (css_computed_opacity(computed, &fixed_value) == CSS_OPACITY_SET) {
        state->style.opacity = web_css_fixed_unit_to_byte(fixed_value);
        web_css_set_score(state, WEB_PROP_OPACITY);
    }

    if (css_computed_z_index(computed, &z_index) == CSS_Z_INDEX_SET) {
        state->style.z_index = z_index > 0 ? (uint32_t)z_index : 0;
        web_css_set_score(state, WEB_PROP_Z_INDEX);
    }

    if (css_computed_letter_spacing(computed, &length, &unit) == CSS_LETTER_SPACING_SET) {
        state->style.letter_spacing = web_css_length_to_px(computed, unit_ctx, length, unit, state->style.font_size);
        web_css_set_score(state, WEB_PROP_LETTER_SPACING);
    }

    if (css_computed_word_spacing(computed, &length, &unit) == CSS_WORD_SPACING_SET) {
        state->style.word_spacing = web_css_length_to_px(computed, unit_ctx, length, unit, state->style.font_size);
        web_css_set_score(state, WEB_PROP_WORD_SPACING);
    }

    state->style.background_repeat = web_css_background_repeat_value(css_computed_background_repeat(computed));
    web_css_set_score(state, WEB_PROP_BACKGROUND_REPEAT);

    type = css_computed_background_position(computed, &hlength, &hunit, &vlength, &vunit);
    if (type != CSS_BACKGROUND_POSITION_INHERIT) {
        state->style.background_position_x = web_css_length_to_px(computed, unit_ctx, hlength, hunit, viewport_width);
        state->style.background_position_y = web_css_length_to_px(computed, unit_ctx, vlength, vunit, viewport_height);
        web_css_set_score(state, WEB_PROP_BACKGROUND_POSITION);
    }

    web_css_trace_node_step(node, "apply-done");
}

static int web_css_style_for_node(web_css_node_t *node,
                                  uint32_t viewport_width,
                                  uint32_t viewport_height,
                                  web_style_state_t *state) {
    css_select_results *results = NULL;
    css_media media;
    css_unit_ctx unit_ctx;
    css_error error;
    unsigned long long start_ticks;

    if (node == NULL) {
        return -1;
    }

    memset(&media, 0, sizeof(media));
    memset(&unit_ctx, 0, sizeof(unit_ctx));
    media.type = CSS_MEDIA_SCREEN;
    media.width = INTTOFIX((int)viewport_width);
    media.height = INTTOFIX((int)viewport_height);
    media.orientation = viewport_width >= viewport_height
        ? CSS_MEDIA_ORIENTATION_LANDSCAPE
        : CSS_MEDIA_ORIENTATION_PORTRAIT;
    media.scan = CSS_MEDIA_SCAN_PROGRESSIVE;
    media.update = CSS_MEDIA_UPDATE_FREQUENCY_NORMAL;
    media.pointer = CSS_MEDIA_POINTER_FINE;
    media.any_pointer = CSS_MEDIA_POINTER_FINE;
    media.hover = CSS_MEDIA_HOVER_HOVER;
    media.any_hover = CSS_MEDIA_HOVER_HOVER;
    media.light_level = CSS_MEDIA_LIGHT_LEVEL_NORMAL;
    media.scripting = CSS_MEDIA_SCRIPTING_ENABLED;
    media.color = INTTOFIX(24);
    unit_ctx.viewport_width = INTTOFIX((int)viewport_width);
    unit_ctx.viewport_height = INTTOFIX((int)viewport_height);
    unit_ctx.font_size_default = INTTOFIX(16);
    unit_ctx.font_size_minimum = INTTOFIX(6);
    unit_ctx.device_dpi = INTTOFIX(96);

    web_css_trace_node_step(node, "select-start");
    start_ticks = timer_ticks();
    error = css_select_style(web_css_select_ctx,
                             node,
                             &unit_ctx,
                             &media,
                             node->inline_style,
                             &web_css_select_handler,
                             NULL,
                             &results);
    if (error != CSS_OK || results == NULL || results->styles[CSS_PSEUDO_ELEMENT_NONE] == NULL) {
        web_css_trace_node_step(node, "select-fail");
        if (results != NULL) {
            css_select_results_destroy(results);
        }
        return -1;
    }
    web_css_select_total_ticks += timer_ticks() - start_ticks;
    web_css_trace_node_step(node, "select-done");
    start_ticks = timer_ticks();
    web_css_apply_computed_style(node,
                                 state,
                                 results->styles[CSS_PSEUDO_ELEMENT_NONE],
                                 &unit_ctx,
                                 viewport_width,
                                 viewport_height);
    web_css_apply_total_ticks += timer_ticks() - start_ticks;
    css_select_results_destroy(results);
    return 0;
}

static void web_apply_presentational_attrs(const uint8_t *html,
                                           uint32_t tag_pos,
                                           web_style_state_t *state,
                                           uint32_t viewport_width,
                                           uint32_t viewport_height) {
    uint32_t attr_start = 0;
    uint32_t attr_end = 0;
    if (web_attr_value_range_cstr(html, tag_pos, "align", &attr_start, &attr_end)) {
        if (web_range_contains_cstr_ci(html, attr_start, attr_end, "center")) {
            web_set_text_align(state, WEB_STYLE_ALIGN_CENTER, 1, 0);
        } else if (web_range_contains_cstr_ci(html, attr_start, attr_end, "right")) {
            web_set_text_align(state, WEB_STYLE_ALIGN_RIGHT, 1, 0);
        } else if (web_range_contains_cstr_ci(html, attr_start, attr_end, "left")) {
            web_set_text_align(state, WEB_STYLE_ALIGN_LEFT, 1, 0);
        }
    }
    if (web_attr_value_range_cstr(html, tag_pos, "width", &attr_start, &attr_end)) {
        uint32_t px = 0;
        if (web_parse_length_px(html, attr_start, attr_end, viewport_width, viewport_width, viewport_height, &px)) {
            web_set_length_property(state, WEB_PROP_WIDTH, WEB_STYLE_FLAG_HAS_WIDTH, &state->style.width, px, 1, 0);
        }
    }
    if (web_attr_value_range_cstr(html, tag_pos, "height", &attr_start, &attr_end)) {
        uint32_t px = 0;
        if (web_parse_length_px(html, attr_start, attr_end, viewport_height, viewport_width, viewport_height, &px)) {
            web_set_length_property(state, WEB_PROP_HEIGHT, WEB_STYLE_FLAG_HAS_HEIGHT, &state->style.height, px, 1, 0);
        }
    }
}

static void web_apply_netsurf_hints(const uint8_t *html, uint32_t tag_pos, web_style_state_t *state) {
    uint32_t display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    uint32_t role = NETSURF_PORT_HINT_ROLE_NONE;
    uint32_t flags = 0;

    if (state == NULL ||
        netsurf_port_style_hint_for_tag(html, tag_pos, &display, &role, &flags) == 0) {
        return;
    }
    if ((flags & (NETSURF_PORT_HINT_FLAG_SKIP | NETSURF_PORT_HINT_FLAG_HIDDEN)) != 0u) {
        web_set_display_hint(state, NETSURF_PORT_HINT_DISPLAY_INLINE, 65535u, 1);
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_NONE;
        return;
    }
    if (display != NETSURF_PORT_HINT_DISPLAY_UNKNOWN) {
        web_set_display_hint(state, display, 0, 0);
    }
    (void)role;
}

static void web_apply_inline_style(const uint8_t *html,
                                   uint32_t tag_pos,
                                   web_style_state_t *state,
                                   uint32_t viewport_width,
                                   uint32_t viewport_height) {
    uint32_t style_start = 0;
    uint32_t style_end = 0;
    if (web_attr_value_range_cstr(html, tag_pos, "style", &style_start, &style_end)) {
        web_apply_declarations(html, style_start, style_end, state, 10000, viewport_width, viewport_height);
    }
}

static void web_apply_hidden_attrs(const uint8_t *html, uint32_t tag_pos, web_style_state_t *state) {
    uint32_t attr_start = 0;
    uint32_t attr_end = 0;
    if (web_attr_value_range_cstr(html, tag_pos, "hidden", &attr_start, &attr_end)) {
        web_set_flag_property(state, WEB_PROP_DISPLAY, WEB_STYLE_FLAG_DISPLAY_NONE, 1, 65535, 1);
    }
    if (web_tag_name_is(html, tag_pos, "input") &&
        web_attr_value_range_cstr(html, tag_pos, "type", &attr_start, &attr_end) &&
        web_range_contains_cstr_ci(html, attr_start, attr_end, "hidden")) {
        web_set_flag_property(state, WEB_PROP_DISPLAY, WEB_STYLE_FLAG_DISPLAY_NONE, 1, 65535, 1);
    }
}

static void web_css_precompute_styles(const uint8_t *html,
                                      uint32_t viewport_width,
                                      uint32_t viewport_height) {
    unsigned long long start_ticks;
    uint32_t cached_nodes = 0;
    uint32_t selected_nodes = 0;
    uint32_t failed_nodes = 0;
    unsigned long long hint_ticks = 0;
    unsigned long long hint_started;

    if (html == NULL || web_css_ready == 0 || web_css_select_ctx == NULL) {
        return;
    }
    start_ticks = timer_ticks();
    web_css_select_total_ticks = 0;
    web_css_apply_total_ticks = 0;

    console_puts("css-trace precompute-count=");
    console_put_dec64(web_css_node_count);
    console_puts("\n");

    for (uint32_t i = 0; i < web_css_node_count; ++i) {
        web_css_node_t *node = &web_css_nodes[i];
        web_style_state_t state;

        web_css_trace_node_step(node, "precompute-node");
        if (node->style_cached != 0 &&
            node->style_viewport_width == viewport_width &&
            node->style_viewport_height == viewport_height) {
            ++cached_nodes;
            continue;
        }

        web_css_trace_node_step(node, "precompute-init");
        web_state_init(&state);
        web_css_trace_node_step(node, "precompute-select");
        if (web_css_style_for_node(node, viewport_width, viewport_height, &state) != 0) {
            ++failed_nodes;
            continue;
        }
        ++selected_nodes;
        hint_started = timer_ticks();
        web_apply_netsurf_hints(html, node->tag_pos, &state);
        web_apply_presentational_attrs(html, node->tag_pos, &state, viewport_width, viewport_height);
        web_apply_hidden_attrs(html, node->tag_pos, &state);
        hint_ticks += timer_ticks() - hint_started;
        node->cached_style = state.style;
        node->style_cached = 1;
        node->style_viewport_width = viewport_width;
        node->style_viewport_height = viewport_height;
    }
    console_puts("css-trace precompute-summary nodes=");
    console_put_dec64(web_css_node_count);
    console_puts(" cached=");
    console_put_dec64(cached_nodes);
    console_puts(" selected=");
    console_put_dec64(selected_nodes);
    console_puts(" failed=");
    console_put_dec64(failed_nodes);
    console_puts(" select_ticks=");
    console_put_dec64(web_css_select_total_ticks);
    console_puts(" apply_ticks=");
    console_put_dec64(web_css_apply_total_ticks);
    console_puts(" hint_ticks=");
    console_put_dec64(hint_ticks);
    console_puts(" trace_lines=");
    console_put_dec64(web_css_trace_lines_emitted);
    console_puts(" trace_suppressed=");
    console_put_dec64(web_css_trace_lines_suppressed);
    console_puts(" trace_ticks=");
    console_put_dec64(web_css_trace_total_ticks);
    console_puts(" ticks=");
    console_put_dec64(timer_ticks() - start_ticks);
    console_puts("\n");
}

int web_style_for_cached_rules(const uint8_t *html,
                               uint32_t tag_pos,
                               uint32_t viewport_width,
                               uint32_t viewport_height,
                               const web_style_rules_t *rules,
                               web_style_t *out_style) {
    if (html == NULL || out_style == NULL || html[tag_pos] != '<' || web_is_closing_tag(html, tag_pos)) {
        return -1;
    }
    web_style_state_t state;
    web_state_init(&state);
    web_apply_netsurf_hints(html, tag_pos, &state);
    web_apply_presentational_attrs(html, tag_pos, &state, viewport_width, viewport_height);
    if (rules != NULL && rules->selectors != 0 && rules->decls != 0 &&
        rules->selector_stride != 0 && rules->decl_stride != 0) {
        const uint8_t *selectors = (const uint8_t *)(uintptr_t)rules->selectors;
        const uint8_t *decls = (const uint8_t *)(uintptr_t)rules->decls;
        uint32_t selector_stride = (uint32_t)rules->selector_stride;
        uint32_t decl_stride = (uint32_t)rules->decl_stride;
        uint32_t rule_count = (uint32_t)rules->rule_count;
        for (uint32_t rule = 0; rule < rule_count; ++rule) {
            const uint8_t *selector = selectors + (rule * selector_stride);
            const uint8_t *decl = decls + (rule * decl_stride);
            uint32_t selector_len = web_cstrn_len_u8(selector, selector_stride);
            uint32_t decl_len = web_cstrn_len_u8(decl, decl_stride);
            uint32_t specificity = 0;
            if (selector_len != 0 && decl_len != 0 &&
                web_selector_list_matches(html, tag_pos, selector, 0, selector_len, &specificity)) {
                web_apply_declarations(decl, 0, decl_len, &state, specificity, viewport_width, viewport_height);
            }
        }
    }
    web_apply_inline_style(html, tag_pos, &state, viewport_width, viewport_height);
    web_apply_hidden_attrs(html, tag_pos, &state);
    *out_style = state.style;
    return 0;
}

int web_style_for_tag(const uint8_t *html,
                      uint32_t tag_pos,
                      uint32_t viewport_width,
                      uint32_t viewport_height,
                      web_style_t *out_style) {
    web_css_node_t *node = NULL;
    if (html == NULL || out_style == NULL || html[tag_pos] != '<' || web_is_closing_tag(html, tag_pos)) {
        return -1;
    }
    if (web_css_ready != 0 && web_css_select_ctx != NULL && web_cached_html == html) {
        node = web_css_find_node(tag_pos);
        if (node != NULL &&
            node->style_cached != 0 &&
            node->style_viewport_width == viewport_width &&
            node->style_viewport_height == viewport_height) {
            *out_style = node->cached_style;
            return 0;
        }
    }
    web_style_state_t state;
    web_state_init(&state);
    if (node != NULL && web_css_style_for_node(node, viewport_width, viewport_height, &state) == 0) {
        web_apply_netsurf_hints(html, tag_pos, &state);
        web_apply_presentational_attrs(html, tag_pos, &state, viewport_width, viewport_height);
        web_apply_hidden_attrs(html, tag_pos, &state);
        *out_style = state.style;
        node->cached_style = state.style;
        node->style_cached = 1;
        node->style_viewport_width = viewport_width;
        node->style_viewport_height = viewport_height;
        return 0;
    }
    web_apply_netsurf_hints(html, tag_pos, &state);
    web_apply_presentational_attrs(html, tag_pos, &state, viewport_width, viewport_height);
    if (web_cached_html == html &&
        web_cached_viewport_width == viewport_width &&
        web_cached_viewport_height == viewport_height) {
        web_apply_prepared_rules(html, tag_pos, &state, viewport_width, viewport_height);
    } else {
        web_scan_style_blocks(html, tag_pos, &state, viewport_width, viewport_height);
    }
    web_apply_inline_style(html, tag_pos, &state, viewport_width, viewport_height);
    web_apply_hidden_attrs(html, tag_pos, &state);
    *out_style = state.style;
    return 0;
}
