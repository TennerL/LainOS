#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "libc.h"
#include "netsurf_port.h"

#include <dom/dom.h>
#include "bindings/hubbub/parser.h"

#define NETSURF_PORT_DOM_CREATE 1u
#define NETSURF_PORT_DOM_PARSE 2u
#define NETSURF_PORT_DOM_COMPLETE 4u
#define NETSURF_PORT_DOM_ROOT 8u
#define NETSURF_PORT_DOM_TEXT 16u
#define NETSURF_PORT_DOM_CLEANUP 32u
#define NETSURF_PORT_DOM_SERIALIZE 64u
#define NETSURF_PORT_DOM_PRIMARY 128u
#define NETSURF_PORT_DOM_EXPECTED 63u

#define NETSURF_PORT_MODE_OMIT_SCRIPT 1u
#define NETSURF_PORT_MODE_MARK_PRIMARY 2u

typedef struct netsurf_port_writer {
    uint8_t *out;
    uint32_t capacity;
    uint32_t written;
    uint32_t mode;
    uint32_t primary_marked;
    dom_node *primary_node;
} netsurf_port_writer_t;

typedef struct netsurf_port_primary_match {
    dom_node *node;
    uint32_t score;
    uint32_t text_bytes;
    uint32_t order;
    uint32_t best_order;
} netsurf_port_primary_match_t;

#define NETSURF_PORT_MAX_HINTS 8192u

typedef struct netsurf_port_style_hint {
    const uint8_t *html;
    uint32_t tag_pos;
    uint8_t display;
    uint8_t role;
    uint8_t flags;
} netsurf_port_style_hint_t;

static uint32_t netsurf_port_last_dom_status;
static const uint8_t *netsurf_port_hint_html;
static uint32_t netsurf_port_hint_count;
static uint32_t netsurf_port_hint_saturated;
static netsurf_port_style_hint_t netsurf_port_hints[NETSURF_PORT_MAX_HINTS];

static uint8_t netsurf_port_lower(uint8_t ch);
static uint32_t netsurf_port_find_tag_pos_from_fragment(const uint8_t *html, const char *fragment);

static void netsurf_port_dom_msg(uint32_t severity, void *ctx, const char *msg, ...) {
    (void)severity;
    (void)ctx;
    (void)msg;
}

static void netsurf_port_clear_style_hints(void) {
    netsurf_port_hint_html = 0;
    netsurf_port_hint_count = 0;
    netsurf_port_hint_saturated = 0;
}

static int netsurf_port_dom_string_contains(dom_string *str, const char *needle) {
    const char *data;
    size_t len;
    size_t needle_len;

    if (str == 0 || needle == 0) {
        return 0;
    }

    data = dom_string_data(str);
    len = dom_string_byte_length(str);
    needle_len = strlen(needle);
    if (data == 0 || needle_len == 0 || len < needle_len) {
        return 0;
    }

    for (size_t i = 0; i <= len - needle_len; ++i) {
        if (memcmp(data + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int netsurf_port_dom_string_contains_ci(const dom_string *str, const char *needle) {
    const char *data;
    size_t len;
    size_t needle_len;

    if (str == 0 || needle == 0) {
        return 0;
    }

    data = dom_string_data(str);
    len = dom_string_byte_length(str);
    needle_len = strlen(needle);
    if (data == 0 || needle_len == 0 || len < needle_len) {
        return 0;
    }

    for (size_t i = 0; i <= len - needle_len; ++i) {
        size_t j = 0;

        while (j < needle_len &&
               netsurf_port_lower((uint8_t)data[i + j]) == netsurf_port_lower((uint8_t)needle[j])) {
            ++j;
        }
        if (j == needle_len) {
            return 1;
        }
    }
    return 0;
}

static int netsurf_port_dom_string_has_token_ci(const dom_string *str, const char *needle) {
    const char *data;
    size_t len;
    size_t needle_len;
    size_t pos = 0;

    if (str == 0 || needle == 0) {
        return 0;
    }

    data = dom_string_data(str);
    len = dom_string_byte_length(str);
    needle_len = strlen(needle);
    if (data == 0 || needle_len == 0 || len < needle_len) {
        return 0;
    }

    while (pos < len) {
        size_t token_start;
        size_t token_len;
        size_t i;

        while (pos < len && data[pos] <= ' ') {
            ++pos;
        }
        token_start = pos;
        while (pos < len && data[pos] > ' ') {
            ++pos;
        }
        token_len = pos - token_start;
        if (token_len != needle_len) {
            continue;
        }
        for (i = 0; i < needle_len; ++i) {
            if (netsurf_port_lower((uint8_t)data[token_start + i]) !=
                netsurf_port_lower((uint8_t)needle[i])) {
                break;
            }
        }
        if (i == needle_len) {
            return 1;
        }
    }
    return 0;
}

static int netsurf_port_is_html_root(dom_string *name) {
    const char *data;
    size_t len;

    if (name == 0) {
        return 0;
    }
    data = dom_string_data(name);
    len = dom_string_byte_length(name);
    return len == 4u && strncasecmp(data, "html", 4u) == 0;
}

uint32_t netsurf_port_dom_status(void) {
    return netsurf_port_last_dom_status;
}

const char *netsurf_port_status(void) {
    if ((netsurf_port_last_dom_status & NETSURF_PORT_DOM_EXPECTED) == NETSURF_PORT_DOM_EXPECTED) {
        return "dom-ready";
    }
    if ((netsurf_port_last_dom_status & NETSURF_PORT_DOM_CREATE) == 0u) {
        return "dom-create-failed";
    }
    if ((netsurf_port_last_dom_status & NETSURF_PORT_DOM_PARSE) == 0u) {
        return "dom-parse-failed";
    }
    if ((netsurf_port_last_dom_status & NETSURF_PORT_DOM_COMPLETE) == 0u) {
        return "dom-complete-failed";
    }
    return "dom-incomplete";
}

static uint8_t netsurf_port_lower(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return (uint8_t)(ch + ('a' - 'A'));
    }
    return ch;
}

static void netsurf_port_write_byte(netsurf_port_writer_t *writer, uint8_t ch) {
    if (writer->out != 0 && writer->capacity != 0 && writer->written + 1u < writer->capacity) {
        writer->out[writer->written] = ch;
    }
    ++writer->written;
}

static void netsurf_port_write_cstr(netsurf_port_writer_t *writer, const char *text) {
    if (text == 0) {
        return;
    }
    while (*text != '\0') {
        netsurf_port_write_byte(writer, (uint8_t)*text++);
    }
}

static void netsurf_port_write_dom_string(netsurf_port_writer_t *writer,
                                          const dom_string *str,
                                          int lower,
                                          int attr_escape,
                                          int text_escape) {
    const char *data;
    size_t len;

    if (str == 0) {
        return;
    }
    data = dom_string_data(str);
    len = dom_string_byte_length(str);
    if (data == 0) {
        return;
    }

    for (size_t i = 0; i < len; ++i) {
        uint8_t ch = (uint8_t)data[i];

        if (lower != 0) {
            ch = netsurf_port_lower(ch);
        }
        if (attr_escape != 0 && ch == '"') {
            netsurf_port_write_cstr(writer, "&quot;");
        } else if ((attr_escape != 0 || text_escape != 0) && ch == '&') {
            netsurf_port_write_cstr(writer, "&amp;");
        } else if (text_escape != 0 && ch == '<') {
            netsurf_port_write_cstr(writer, "&lt;");
        } else if (text_escape != 0 && ch == '>') {
            netsurf_port_write_cstr(writer, "&gt;");
        } else {
            netsurf_port_write_byte(writer, ch);
        }
    }
}

static int netsurf_port_dom_string_equals_ci(const dom_string *str, const char *name) {
    const char *data;
    size_t len;
    size_t name_len;

    if (str == 0 || name == 0) {
        return 0;
    }
    data = dom_string_data(str);
    len = dom_string_byte_length(str);
    name_len = strlen(name);
    if (data == 0 || len != name_len) {
        return 0;
    }
    for (size_t i = 0; i < len; ++i) {
        if (netsurf_port_lower((uint8_t)data[i]) != netsurf_port_lower((uint8_t)name[i])) {
            return 0;
        }
    }
    return 1;
}

static uint32_t netsurf_port_u32_max(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

static int netsurf_port_dom_string_is_void_tag(const dom_string *name) {
    return netsurf_port_dom_string_equals_ci(name, "area") ||
           netsurf_port_dom_string_equals_ci(name, "base") ||
           netsurf_port_dom_string_equals_ci(name, "br") ||
           netsurf_port_dom_string_equals_ci(name, "col") ||
           netsurf_port_dom_string_equals_ci(name, "embed") ||
           netsurf_port_dom_string_equals_ci(name, "hr") ||
           netsurf_port_dom_string_equals_ci(name, "img") ||
           netsurf_port_dom_string_equals_ci(name, "input") ||
           netsurf_port_dom_string_equals_ci(name, "link") ||
           netsurf_port_dom_string_equals_ci(name, "meta") ||
           netsurf_port_dom_string_equals_ci(name, "param") ||
           netsurf_port_dom_string_equals_ci(name, "source") ||
           netsurf_port_dom_string_equals_ci(name, "track") ||
           netsurf_port_dom_string_equals_ci(name, "wbr");
}

static int netsurf_port_dom_string_is_raw_text_tag(const dom_string *name) {
    return netsurf_port_dom_string_equals_ci(name, "script") ||
           netsurf_port_dom_string_equals_ci(name, "style") ||
           netsurf_port_dom_string_equals_ci(name, "title") ||
           netsurf_port_dom_string_equals_ci(name, "textarea");
}

static dom_string *netsurf_port_element_attr(dom_node *node, const char *attr_name) {
    dom_string *name = 0;
    dom_string *value = 0;

    if (node == 0 || attr_name == 0) {
        return 0;
    }
    if (dom_string_create((const uint8_t *)attr_name, strlen(attr_name), &name) != DOM_NO_ERR || name == 0) {
        return 0;
    }
    if (dom_element_get_attribute((dom_element *)node, name, &value) != DOM_NO_ERR) {
        value = 0;
    }
    dom_string_unref(name);
    return value;
}

static int netsurf_port_element_attr_equals_ci(dom_node *node,
                                               const char *attr_name,
                                               const char *value_name) {
    dom_string *value = 0;
    int match = 0;

    if (node == 0 || attr_name == 0 || value_name == 0) {
        return 0;
    }
    value = netsurf_port_element_attr(node, attr_name);
    if (value != 0) {
        match = netsurf_port_dom_string_equals_ci(value, value_name);
        dom_string_unref(value);
    }
    return match;
}

static int netsurf_port_element_attr_has_token_ci(dom_node *node,
                                                  const char *attr_name,
                                                  const char *value_name) {
    dom_string *value = 0;
    int match = 0;

    if (node == 0 || attr_name == 0 || value_name == 0) {
        return 0;
    }
    value = netsurf_port_element_attr(node, attr_name);
    if (value != 0) {
        match = netsurf_port_dom_string_has_token_ci(value, value_name);
        dom_string_unref(value);
    }
    return match;
}

static int netsurf_port_element_has_attr(dom_node *node, const char *attr_name) {
    dom_string *name = 0;
    bool match = false;

    if (node == 0 || attr_name == 0) {
        return 0;
    }
    if (dom_string_create((const uint8_t *)attr_name, strlen(attr_name), &name) != DOM_NO_ERR || name == 0) {
        return 0;
    }
    if (dom_element_has_attribute((dom_element *)node, name, &match) != DOM_NO_ERR) {
        match = false;
    }
    dom_string_unref(name);
    return match ? 1 : 0;
}

static int netsurf_port_primary_ignores_element(const dom_string *name) {
    return netsurf_port_dom_string_equals_ci(name, "head") ||
           netsurf_port_dom_string_equals_ci(name, "script") ||
           netsurf_port_dom_string_equals_ci(name, "style") ||
           netsurf_port_dom_string_equals_ci(name, "template") ||
           netsurf_port_dom_string_equals_ci(name, "noscript") ||
           netsurf_port_dom_string_equals_ci(name, "nav") ||
           netsurf_port_dom_string_equals_ci(name, "header") ||
           netsurf_port_dom_string_equals_ci(name, "footer") ||
           netsurf_port_dom_string_equals_ci(name, "aside") ||
           netsurf_port_dom_string_equals_ci(name, "form") ||
           netsurf_port_dom_string_equals_ci(name, "svg") ||
           netsurf_port_dom_string_equals_ci(name, "canvas");
}

static int netsurf_port_primary_attr_is_chrome(const dom_string *value) {
    return netsurf_port_dom_string_contains_ci(value, "sidebar") ||
           netsurf_port_dom_string_contains_ci(value, "navbox") ||
           netsurf_port_dom_string_contains_ci(value, "navigation") ||
           netsurf_port_dom_string_contains_ci(value, "breadcrumb") ||
           netsurf_port_dom_string_contains_ci(value, "breadcrumbs") ||
           netsurf_port_dom_string_contains_ci(value, "footer") ||
           netsurf_port_dom_string_contains_ci(value, "header") ||
           netsurf_port_dom_string_contains_ci(value, "toc") ||
           netsurf_port_dom_string_contains_ci(value, "toolbar") ||
           netsurf_port_dom_string_contains_ci(value, "menu") ||
           netsurf_port_dom_string_contains_ci(value, "metadata") ||
           netsurf_port_dom_string_contains_ci(value, "printfooter") ||
           netsurf_port_dom_string_contains_ci(value, "mw-editsection") ||
           netsurf_port_dom_string_contains_ci(value, "catlinks");
}

static int netsurf_port_element_role_is(dom_node *node, const char *role_name) {
    return netsurf_port_element_attr_has_token_ci(node, "role", role_name);
}

static int netsurf_port_element_role_is_chrome(dom_node *node) {
    return netsurf_port_element_role_is(node, "navigation") ||
           netsurf_port_element_role_is(node, "banner") ||
           netsurf_port_element_role_is(node, "contentinfo") ||
           netsurf_port_element_role_is(node, "complementary") ||
           netsurf_port_element_role_is(node, "search") ||
           netsurf_port_element_role_is(node, "tablist") ||
           netsurf_port_element_role_is(node, "toolbar");
}

static uint32_t netsurf_port_element_primary_score(dom_node *node, const dom_string *name) {
    dom_string *id = 0;
    dom_string *klass = 0;
    uint32_t score = 0;
    int eligible_tag;

    if (node == 0 || name == 0 || netsurf_port_primary_ignores_element(name)) {
        return 0;
    }

    eligible_tag = netsurf_port_dom_string_equals_ci(name, "div") ||
                   netsurf_port_dom_string_equals_ci(name, "section") ||
                   netsurf_port_dom_string_equals_ci(name, "main") ||
                   netsurf_port_dom_string_equals_ci(name, "article");
    if (eligible_tag == 0) {
        return 0;
    }

    if (netsurf_port_dom_string_equals_ci(name, "article")) {
        score = netsurf_port_u32_max(score, 90u);
    }
    if (netsurf_port_dom_string_equals_ci(name, "main")) {
        score = netsurf_port_u32_max(score, 85u);
    }
    if (netsurf_port_dom_string_equals_ci(name, "section")) {
        score = netsurf_port_u32_max(score, 35u);
    }

    id = netsurf_port_element_attr(node, "id");
    klass = netsurf_port_element_attr(node, "class");
    if (netsurf_port_primary_attr_is_chrome(id) ||
        netsurf_port_primary_attr_is_chrome(klass) ||
        netsurf_port_element_role_is_chrome(node)) {
        score = 0;
        goto out;
    }

    if (netsurf_port_element_role_is(node, "main")) {
        score = netsurf_port_u32_max(score, 145u);
    }
    if (netsurf_port_element_role_is(node, "article")) {
        score = netsurf_port_u32_max(score, 110u);
    }
    if (netsurf_port_dom_string_contains_ci(id, "mw-content-text")) {
        score = netsurf_port_u32_max(score, 140u);
    }
    if (netsurf_port_dom_string_contains_ci(klass, "mw-parser-output")) {
        score = netsurf_port_u32_max(score, 135u);
    }
    if (netsurf_port_dom_string_contains_ci(klass, "mw-body-content")) {
        score = netsurf_port_u32_max(score, 130u);
    }
    if (netsurf_port_dom_string_contains_ci(id, "article") ||
        netsurf_port_dom_string_contains_ci(klass, "article")) {
        score = netsurf_port_u32_max(score, 105u);
    }
    if (netsurf_port_dom_string_has_token_ci(klass, "post")) {
        score = netsurf_port_u32_max(score, 100u);
    }
    if (netsurf_port_dom_string_contains_ci(klass, "entry-content") ||
        netsurf_port_dom_string_contains_ci(klass, "post-content")) {
        score = netsurf_port_u32_max(score, 100u);
    }
    if (netsurf_port_dom_string_contains_ci(id, "main-content") ||
        netsurf_port_dom_string_contains_ci(klass, "main-content")) {
        score = netsurf_port_u32_max(score, 95u);
    }
    if (netsurf_port_dom_string_contains_ci(id, "content") ||
        netsurf_port_dom_string_contains_ci(klass, "content")) {
        score = netsurf_port_u32_max(score, 65u);
    }

out:
    if (klass != 0) {
        dom_string_unref(klass);
    }
    if (id != 0) {
        dom_string_unref(id);
    }
    return score;
}

static uint32_t netsurf_port_dom_string_text_weight(const dom_string *str, uint32_t capacity) {
    const char *data;
    size_t len;
    uint32_t count = 0;

    if (str == 0 || capacity == 0u) {
        return 0;
    }
    data = dom_string_data(str);
    len = dom_string_byte_length(str);
    if (data == 0) {
        return 0;
    }
    for (size_t i = 0; i < len && count < capacity; ++i) {
        uint8_t ch = (uint8_t)data[i];

        if (ch > ' ' && ch != 127u) {
            ++count;
        }
    }
    return count;
}

static void netsurf_port_primary_consider(netsurf_port_primary_match_t *match,
                                          dom_node *node,
                                          uint32_t base_score,
                                          uint32_t text_bytes,
                                          uint32_t order) {
    uint32_t text_bonus;
    uint32_t score;
    int better;

    if (match == 0 || node == 0 || base_score == 0u || text_bytes < 24u) {
        return;
    }
    text_bonus = text_bytes > 900u ? 900u : text_bytes;
    score = base_score * 1000u + text_bonus;
    better = score > match->score ||
             (score == match->score &&
              (text_bytes > match->text_bytes ||
               (text_bytes == match->text_bytes &&
                (match->best_order == 0u || order < match->best_order))));
    if (better == 0) {
        return;
    }
    if (match->node != 0) {
        dom_node_unref(match->node);
    }
    match->node = dom_node_ref(node);
    match->score = score;
    match->text_bytes = text_bytes;
    match->best_order = order;
}

static uint32_t netsurf_port_scan_primary_node(dom_node *node,
                                               netsurf_port_primary_match_t *match,
                                               uint32_t depth) {
    dom_node_type type;
    dom_string *name = 0;
    dom_string *value = 0;
    dom_node *child = 0;
    uint32_t text_bytes = 0;
    uint32_t base_score = 0;
    uint32_t order = 0;

    if (node == 0 || match == 0 || depth > 256u ||
        dom_node_get_node_type(node, &type) != DOM_NO_ERR) {
        return 0;
    }

    if (type == DOM_TEXT_NODE || type == DOM_CDATA_SECTION_NODE) {
        if (dom_node_get_node_value(node, &value) == DOM_NO_ERR && value != 0) {
            text_bytes = netsurf_port_dom_string_text_weight(value, 4096u);
            dom_string_unref(value);
        }
        return text_bytes;
    }
    if (type != DOM_DOCUMENT_NODE &&
        type != DOM_DOCUMENT_FRAGMENT_NODE &&
        type != DOM_ELEMENT_NODE) {
        return 0;
    }

    if (type == DOM_ELEMENT_NODE) {
        if (dom_element_get_tag_name((dom_element *)node, &name) != DOM_NO_ERR || name == 0) {
            return 0;
        }
        if (netsurf_port_primary_ignores_element(name)) {
            dom_string_unref(name);
            return 0;
        }
        order = ++match->order;
        base_score = netsurf_port_element_primary_score(node, name);
    }

    if (dom_node_get_first_child(node, &child) == DOM_NO_ERR) {
        while (child != 0) {
            dom_node *next = 0;
            uint32_t child_text = netsurf_port_scan_primary_node(child, match, depth + 1u);

            if (text_bytes < 4096u) {
                uint32_t remaining = 4096u - text_bytes;

                text_bytes += child_text > remaining ? remaining : child_text;
            }
            if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) {
                next = 0;
            }
            dom_node_unref(child);
            child = next;
        }
    }

    if (type == DOM_ELEMENT_NODE) {
        netsurf_port_primary_consider(match, node, base_score, text_bytes, order);
        dom_string_unref(name);
    }
    return text_bytes;
}

static dom_node *netsurf_port_select_primary_node(dom_node *root) {
    netsurf_port_primary_match_t match;

    memset(&match, 0, sizeof(match));
    (void)netsurf_port_scan_primary_node(root, &match, 0);
    return match.node;
}

static uint32_t netsurf_port_hint_display_for_tag(const dom_string *name) {
    if (name == 0) {
        return NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    }
    if (netsurf_port_dom_string_equals_ci(name, "li")) {
        return NETSURF_PORT_HINT_DISPLAY_LIST_ITEM;
    }
    if (netsurf_port_dom_string_equals_ci(name, "table")) {
        return NETSURF_PORT_HINT_DISPLAY_TABLE;
    }
    if (netsurf_port_dom_string_equals_ci(name, "tr") ||
        netsurf_port_dom_string_equals_ci(name, "thead") ||
        netsurf_port_dom_string_equals_ci(name, "tbody") ||
        netsurf_port_dom_string_equals_ci(name, "tfoot")) {
        return NETSURF_PORT_HINT_DISPLAY_TABLE_ROW;
    }
    if (netsurf_port_dom_string_equals_ci(name, "td") ||
        netsurf_port_dom_string_equals_ci(name, "th")) {
        return NETSURF_PORT_HINT_DISPLAY_TABLE_CELL;
    }
    if (netsurf_port_dom_string_equals_ci(name, "html") ||
        netsurf_port_dom_string_equals_ci(name, "body") ||
        netsurf_port_dom_string_equals_ci(name, "address") ||
        netsurf_port_dom_string_equals_ci(name, "article") ||
        netsurf_port_dom_string_equals_ci(name, "aside") ||
        netsurf_port_dom_string_equals_ci(name, "blockquote") ||
        netsurf_port_dom_string_equals_ci(name, "caption") ||
        netsurf_port_dom_string_equals_ci(name, "details") ||
        netsurf_port_dom_string_equals_ci(name, "div") ||
        netsurf_port_dom_string_equals_ci(name, "dl") ||
        netsurf_port_dom_string_equals_ci(name, "dt") ||
        netsurf_port_dom_string_equals_ci(name, "dd") ||
        netsurf_port_dom_string_equals_ci(name, "fieldset") ||
        netsurf_port_dom_string_equals_ci(name, "figcaption") ||
        netsurf_port_dom_string_equals_ci(name, "figure") ||
        netsurf_port_dom_string_equals_ci(name, "footer") ||
        netsurf_port_dom_string_equals_ci(name, "form") ||
        netsurf_port_dom_string_equals_ci(name, "h1") ||
        netsurf_port_dom_string_equals_ci(name, "h2") ||
        netsurf_port_dom_string_equals_ci(name, "h3") ||
        netsurf_port_dom_string_equals_ci(name, "h4") ||
        netsurf_port_dom_string_equals_ci(name, "h5") ||
        netsurf_port_dom_string_equals_ci(name, "h6") ||
        netsurf_port_dom_string_equals_ci(name, "header") ||
        netsurf_port_dom_string_equals_ci(name, "hr") ||
        netsurf_port_dom_string_equals_ci(name, "legend") ||
        netsurf_port_dom_string_equals_ci(name, "main") ||
        netsurf_port_dom_string_equals_ci(name, "nav") ||
        netsurf_port_dom_string_equals_ci(name, "ol") ||
        netsurf_port_dom_string_equals_ci(name, "p") ||
        netsurf_port_dom_string_equals_ci(name, "pre") ||
        netsurf_port_dom_string_equals_ci(name, "search") ||
        netsurf_port_dom_string_equals_ci(name, "section") ||
        netsurf_port_dom_string_equals_ci(name, "summary") ||
        netsurf_port_dom_string_equals_ci(name, "ul")) {
        return NETSURF_PORT_HINT_DISPLAY_BLOCK;
    }
    return NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
}

static int netsurf_port_hint_attr_scan_candidate(const dom_string *name, uint32_t display) {
    return display != NETSURF_PORT_HINT_DISPLAY_UNKNOWN ||
           netsurf_port_dom_string_equals_ci(name, "span");
}

static uint32_t netsurf_port_hint_role_for_tag(dom_node *node, const dom_string *name, int scan_attrs) {
    if (name == 0) {
        return NETSURF_PORT_HINT_ROLE_NONE;
    }
    if (netsurf_port_dom_string_equals_ci(name, "nav") ||
        netsurf_port_dom_string_equals_ci(name, "header") ||
        netsurf_port_dom_string_equals_ci(name, "footer") ||
        netsurf_port_dom_string_equals_ci(name, "search") ||
        netsurf_port_dom_string_equals_ci(name, "aside")) {
        return NETSURF_PORT_HINT_ROLE_CHROME;
    }
    if (scan_attrs != 0 && netsurf_port_element_role_is(node, "main")) {
        return NETSURF_PORT_HINT_ROLE_PRIMARY;
    }
    if (scan_attrs != 0 && netsurf_port_element_role_is(node, "heading")) {
        return NETSURF_PORT_HINT_ROLE_HEADING;
    }
    if (scan_attrs != 0 && netsurf_port_element_role_is_chrome(node)) {
        return NETSURF_PORT_HINT_ROLE_CHROME;
    }
    if (scan_attrs != 0 &&
        (netsurf_port_element_attr_has_token_ci(node, "class", "navbar") ||
         netsurf_port_element_attr_has_token_ci(node, "class", "navigation") ||
         netsurf_port_element_attr_has_token_ci(node, "class", "navbox") ||
         netsurf_port_element_attr_has_token_ci(node, "class", "breadcrumb") ||
         netsurf_port_element_attr_has_token_ci(node, "class", "breadcrumbs") ||
         netsurf_port_element_attr_has_token_ci(node, "class", "sidebar") ||
         netsurf_port_element_attr_has_token_ci(node, "class", "site-header") ||
         netsurf_port_element_attr_has_token_ci(node, "class", "site-footer") ||
         netsurf_port_element_attr_has_token_ci(node, "class", "toc") ||
         netsurf_port_element_attr_equals_ci(node, "id", "breadcrumb") ||
         netsurf_port_element_attr_equals_ci(node, "id", "breadcrumbs") ||
         netsurf_port_element_attr_equals_ci(node, "id", "footer"))) {
        return NETSURF_PORT_HINT_ROLE_CHROME;
    }
    if (netsurf_port_dom_string_equals_ci(name, "table") ||
        netsurf_port_dom_string_equals_ci(name, "thead") ||
        netsurf_port_dom_string_equals_ci(name, "tbody") ||
        netsurf_port_dom_string_equals_ci(name, "tfoot")) {
        return NETSURF_PORT_HINT_ROLE_TABLE;
    }
    if (netsurf_port_dom_string_equals_ci(name, "tr")) {
        return NETSURF_PORT_HINT_ROLE_ROW;
    }
    if (netsurf_port_dom_string_equals_ci(name, "td") ||
        netsurf_port_dom_string_equals_ci(name, "th")) {
        return NETSURF_PORT_HINT_ROLE_CELL;
    }
    if (netsurf_port_dom_string_equals_ci(name, "ul") ||
        netsurf_port_dom_string_equals_ci(name, "ol") ||
        netsurf_port_dom_string_equals_ci(name, "dl") ||
        netsurf_port_dom_string_equals_ci(name, "li") ||
        netsurf_port_dom_string_equals_ci(name, "dt") ||
        netsurf_port_dom_string_equals_ci(name, "dd")) {
        return NETSURF_PORT_HINT_ROLE_LIST;
    }
    if (netsurf_port_dom_string_equals_ci(name, "img") ||
        netsurf_port_dom_string_equals_ci(name, "figure") ||
        netsurf_port_dom_string_equals_ci(name, "figcaption") ||
        netsurf_port_dom_string_equals_ci(name, "canvas") ||
        netsurf_port_dom_string_equals_ci(name, "video") ||
        netsurf_port_dom_string_equals_ci(name, "audio")) {
        return NETSURF_PORT_HINT_ROLE_MEDIA;
    }
    if (netsurf_port_dom_string_equals_ci(name, "h1") ||
        netsurf_port_dom_string_equals_ci(name, "h2") ||
        netsurf_port_dom_string_equals_ci(name, "h3") ||
        netsurf_port_dom_string_equals_ci(name, "h4") ||
        netsurf_port_dom_string_equals_ci(name, "h5") ||
        netsurf_port_dom_string_equals_ci(name, "h6")) {
        return NETSURF_PORT_HINT_ROLE_HEADING;
    }
    if (netsurf_port_dom_string_equals_ci(name, "form") ||
        netsurf_port_dom_string_equals_ci(name, "button") ||
        netsurf_port_dom_string_equals_ci(name, "input") ||
        netsurf_port_dom_string_equals_ci(name, "select") ||
        netsurf_port_dom_string_equals_ci(name, "textarea")) {
        return NETSURF_PORT_HINT_ROLE_FORM;
    }
    return NETSURF_PORT_HINT_ROLE_NONE;
}

static int netsurf_port_hint_should_skip(dom_node *node) {
    return netsurf_port_element_attr_has_token_ci(node, "class", "printfooter") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "metadata") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "noprint") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "screen-reader-text") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "visually-hidden") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "visuallyhidden") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "sr-only") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "mw-editsection") ||
           netsurf_port_element_attr_equals_ci(node, "id", "catlinks");
}

static uint32_t netsurf_port_hint_flags_for_element(netsurf_port_writer_t *writer,
                                                    dom_node *node,
                                                    const dom_string *name,
                                                    uint32_t display,
                                                    uint32_t role,
                                                    int scan_attrs) {
    uint32_t flags = 0;

    (void)scan_attrs;
    if (writer != 0 && writer->primary_node != 0 && writer->primary_node == node) {
        flags |= NETSURF_PORT_HINT_FLAG_PRIMARY;
    }
    if (display != NETSURF_PORT_HINT_DISPLAY_UNKNOWN ||
        role != NETSURF_PORT_HINT_ROLE_NONE) {
        flags |= NETSURF_PORT_HINT_FLAG_STRUCTURAL;
    }
    if (netsurf_port_hint_should_skip(node)) {
        flags |= NETSURF_PORT_HINT_FLAG_SKIP;
    }
    /* Only HTML hidden state should affect visual layout hints here. */
    if (netsurf_port_element_has_attr(node, "hidden")) {
        flags |= NETSURF_PORT_HINT_FLAG_HIDDEN;
    }
    if (netsurf_port_dom_string_equals_ci(name, "input") &&
        netsurf_port_element_attr_equals_ci(node, "type", "hidden")) {
        flags |= NETSURF_PORT_HINT_FLAG_HIDDEN;
    }
    if (netsurf_port_dom_string_equals_ci(name, "script") ||
        netsurf_port_dom_string_equals_ci(name, "template")) {
        flags |= NETSURF_PORT_HINT_FLAG_SKIP;
    }
    return flags;
}

static void netsurf_port_add_style_hint(const uint8_t *html,
                                        uint32_t tag_pos,
                                        uint32_t display,
                                        uint32_t role,
                                        uint32_t flags) {
    netsurf_port_style_hint_t *hint;

    if (html == 0 ||
        (display == NETSURF_PORT_HINT_DISPLAY_UNKNOWN &&
         role == NETSURF_PORT_HINT_ROLE_NONE &&
         flags == 0u)) {
        return;
    }
    if (netsurf_port_hint_html == 0) {
        netsurf_port_hint_html = html;
    }
    if (netsurf_port_hint_html != html) {
        return;
    }
    if (netsurf_port_hint_count >= NETSURF_PORT_MAX_HINTS) {
        netsurf_port_hint_saturated = 1;
        return;
    }
    hint = &netsurf_port_hints[netsurf_port_hint_count];
    hint->html = html;
    hint->tag_pos = tag_pos;
    hint->display = (uint8_t)display;
    hint->role = (uint8_t)role;
    hint->flags = (uint8_t)flags;
    ++netsurf_port_hint_count;
}

static void netsurf_port_record_style_hint(netsurf_port_writer_t *writer,
                                           dom_node *node,
                                           const dom_string *name,
                                           uint32_t tag_pos) {
    uint32_t display;
    uint32_t role;
    uint32_t flags;
    int scan_attrs;

    if (writer == 0 || writer->out == 0 || node == 0 || name == 0) {
        return;
    }
    display = netsurf_port_hint_display_for_tag(name);
    scan_attrs = netsurf_port_hint_attr_scan_candidate(name, display);
    role = netsurf_port_hint_role_for_tag(node, name, scan_attrs);
    flags = netsurf_port_hint_flags_for_element(writer, node, name, display, role, scan_attrs);
    if ((flags & NETSURF_PORT_HINT_FLAG_PRIMARY) != 0u) {
        role = NETSURF_PORT_HINT_ROLE_PRIMARY;
    }
    netsurf_port_add_style_hint(writer->out, tag_pos, display, role, flags);
}

int netsurf_port_style_hint_for_tag(const uint8_t *html,
                                    uint32_t tag_pos,
                                    uint32_t *out_display,
                                    uint32_t *out_role,
                                    uint32_t *out_flags) {
    uint32_t lo = 0;
    uint32_t hi = netsurf_port_hint_count;

    if (out_display != 0) {
        *out_display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    }
    if (out_role != 0) {
        *out_role = NETSURF_PORT_HINT_ROLE_NONE;
    }
    if (out_flags != 0) {
        *out_flags = 0;
    }
    if (html == 0 || html != netsurf_port_hint_html || netsurf_port_hint_count == 0u) {
        return 0;
    }
    while (lo < hi) {
        uint32_t mid = lo + ((hi - lo) / 2u);

        if (netsurf_port_hints[mid].tag_pos == tag_pos) {
            if (out_display != 0) {
                *out_display = netsurf_port_hints[mid].display;
            }
            if (out_role != 0) {
                *out_role = netsurf_port_hints[mid].role;
            }
            if (out_flags != 0) {
                *out_flags = netsurf_port_hints[mid].flags;
                if (netsurf_port_hint_saturated != 0u) {
                    *out_flags |= NETSURF_PORT_HINT_FLAG_STRUCTURAL;
                }
            }
            return 1;
        }
        if (netsurf_port_hints[mid].tag_pos < tag_pos) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return 0;
}

static int netsurf_port_primary_candidate(dom_node *node, const dom_string *name) {
    if (netsurf_port_element_role_is(node, "main")) {
        return 1;
    }
    if (netsurf_port_dom_string_equals_ci(name, "main") ||
        netsurf_port_dom_string_equals_ci(name, "article")) {
        return 1;
    }
    if (!netsurf_port_dom_string_equals_ci(name, "div") &&
        !netsurf_port_dom_string_equals_ci(name, "section")) {
        return 0;
    }
    return netsurf_port_element_attr_equals_ci(node, "id", "mw-content-text") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "mw-body-content") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "mw-parser-output") ||
           netsurf_port_element_attr_equals_ci(node, "id", "article") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "article") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "post") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "entry-content") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "post-content") ||
           netsurf_port_element_attr_has_token_ci(node, "class", "main-content");
}

static int netsurf_port_omits_element(uint32_t mode, const dom_string *name) {
    if ((mode & NETSURF_PORT_MODE_OMIT_SCRIPT) == 0u) {
        return 0;
    }
    return netsurf_port_dom_string_equals_ci(name, "script") ||
           netsurf_port_dom_string_equals_ci(name, "noscript") ||
           netsurf_port_dom_string_equals_ci(name, "template");
}

static void netsurf_port_serialize_node(netsurf_port_writer_t *writer,
                                        dom_node *node,
                                        int raw_text,
                                        uint32_t depth);

static void netsurf_port_serialize_children(netsurf_port_writer_t *writer,
                                            dom_node *node,
                                            int raw_text,
                                            uint32_t depth) {
    dom_node *child = 0;

    if (depth > 256u || dom_node_get_first_child(node, &child) != DOM_NO_ERR) {
        return;
    }
    while (child != 0) {
        dom_node *next = 0;

        netsurf_port_serialize_node(writer, child, raw_text, depth + 1u);
        if (dom_node_get_next_sibling(child, &next) != DOM_NO_ERR) {
            next = 0;
        }
        dom_node_unref(child);
        child = next;
    }
}

static void netsurf_port_serialize_attributes(netsurf_port_writer_t *writer, dom_node *node) {
    dom_namednodemap *attrs = 0;
    dom_ulong length = 0;

    if (dom_node_get_attributes(node, &attrs) != DOM_NO_ERR || attrs == 0) {
        return;
    }
    if (dom_namednodemap_get_length(attrs, &length) == DOM_NO_ERR) {
        for (dom_ulong i = 0; i < length; ++i) {
            dom_node *attr = 0;
            dom_string *name = 0;
            dom_string *value = 0;

            if (dom_namednodemap_item(attrs, i, &attr) != DOM_NO_ERR || attr == 0) {
                continue;
            }
            if (dom_node_get_node_name(attr, &name) == DOM_NO_ERR && name != 0) {
                netsurf_port_write_byte(writer, ' ');
                netsurf_port_write_dom_string(writer, name, 1, 0, 0);
                if (dom_node_get_node_value(attr, &value) == DOM_NO_ERR && value != 0) {
                    netsurf_port_write_cstr(writer, "=\"");
                    netsurf_port_write_dom_string(writer, value, 0, 1, 1);
                    netsurf_port_write_byte(writer, '"');
                }
            }
            if (value != 0) {
                dom_string_unref(value);
            }
            if (name != 0) {
                dom_string_unref(name);
            }
            dom_node_unref(attr);
        }
    }
    dom_namednodemap_unref(attrs);
}

static void netsurf_port_serialize_node(netsurf_port_writer_t *writer,
                                        dom_node *node,
                                        int raw_text,
                                        uint32_t depth) {
    dom_node_type type;
    dom_string *name = 0;
    dom_string *value = 0;
    uint32_t tag_pos;
    int void_tag;
    int child_raw_text;

    if (node == 0 || depth > 256u || dom_node_get_node_type(node, &type) != DOM_NO_ERR) {
        return;
    }

    if (type == DOM_DOCUMENT_NODE || type == DOM_DOCUMENT_FRAGMENT_NODE) {
        netsurf_port_serialize_children(writer, node, raw_text, depth + 1u);
        return;
    }
    if (type == DOM_DOCUMENT_TYPE_NODE) {
        netsurf_port_write_cstr(writer, "<!doctype html>\n");
        return;
    }
    if (type == DOM_TEXT_NODE || type == DOM_CDATA_SECTION_NODE) {
        if (dom_node_get_node_value(node, &value) == DOM_NO_ERR && value != 0) {
            netsurf_port_write_dom_string(writer, value, 0, 0, raw_text == 0);
            dom_string_unref(value);
        }
        return;
    }
    if (type != DOM_ELEMENT_NODE) {
        return;
    }

    if (dom_element_get_tag_name((dom_element *)node, &name) != DOM_NO_ERR || name == 0) {
        return;
    }

    if (netsurf_port_omits_element(writer->mode, name)) {
        dom_string_unref(name);
        return;
    }

    tag_pos = writer->written;
    netsurf_port_record_style_hint(writer, node, name, tag_pos);
    netsurf_port_write_byte(writer, '<');
    netsurf_port_write_dom_string(writer, name, 1, 0, 0);
    netsurf_port_serialize_attributes(writer, node);
    if ((writer->mode & NETSURF_PORT_MODE_MARK_PRIMARY) != 0u &&
        writer->primary_marked == 0u &&
        ((writer->primary_node != 0 && writer->primary_node == node) ||
         (writer->primary_node == 0 && netsurf_port_primary_candidate(node, name)))) {
        netsurf_port_write_cstr(writer, " data-zbrowser-primary=\"1\"");
        writer->primary_marked = 1u;
    }
    netsurf_port_write_byte(writer, '>');

    void_tag = netsurf_port_dom_string_is_void_tag(name);
    child_raw_text = netsurf_port_dom_string_is_raw_text_tag(name);
    if (void_tag == 0) {
        netsurf_port_serialize_children(writer, node, child_raw_text, depth + 1u);
        netsurf_port_write_cstr(writer, "</");
        netsurf_port_write_dom_string(writer, name, 1, 0, 0);
        netsurf_port_write_byte(writer, '>');
    }
    dom_string_unref(name);
}

static dom_hubbub_error netsurf_port_parse_document(const uint8_t *html,
                                                    size_t len,
                                                    dom_hubbub_parser **out_parser,
                                                    dom_document **out_document,
                                                    uint32_t *status) {
    dom_hubbub_parser_params params;
    dom_hubbub_error error;

    memset(&params, 0, sizeof(params));
    params.enc = "UTF-8";
    params.fix_enc = true;
    params.enable_script = false;
    params.msg = netsurf_port_dom_msg;

    error = dom_hubbub_parser_create(&params, out_parser, out_document);
    if (error == DOM_HUBBUB_OK && *out_parser != 0 && *out_document != 0) {
        *status |= NETSURF_PORT_DOM_CREATE;
    } else {
        return error;
    }
    error = dom_hubbub_parser_parse_chunk(*out_parser, html, len);
    if (error == DOM_HUBBUB_OK) {
        *status |= NETSURF_PORT_DOM_PARSE;
    } else {
        return error;
    }
    error = dom_hubbub_parser_completed(*out_parser);
    if (error == DOM_HUBBUB_OK) {
        *status |= NETSURF_PORT_DOM_COMPLETE;
    }
    return error;
}

static int netsurf_port_rewrite_html_mode(const uint8_t *html,
                                          uint32_t len,
                                          uint8_t *out,
                                          uint32_t out_capacity,
                                          uint32_t mode) {
    dom_hubbub_parser *parser = 0;
    dom_document *document = 0;
    dom_element *root = 0;
    dom_node *primary_node = 0;
    netsurf_port_writer_t count_writer;
    netsurf_port_writer_t write_writer;
    uint32_t status = 0;
    int result = -1;

    if (html == 0 || out == 0 || out_capacity == 0) {
        netsurf_port_last_dom_status = 0;
        netsurf_port_clear_style_hints();
        return -1;
    }
    netsurf_port_clear_style_hints();
    if (netsurf_port_parse_document(html, len, &parser, &document, &status) != DOM_HUBBUB_OK) {
        goto out;
    }
    if (document == 0 ||
        dom_document_get_document_element(document, &root) != DOM_NO_ERR ||
        root == 0) {
        goto out;
    }
    status |= NETSURF_PORT_DOM_ROOT;
    if ((mode & NETSURF_PORT_MODE_MARK_PRIMARY) != 0u) {
        primary_node = netsurf_port_select_primary_node((dom_node *)root);
    }

    memset(&count_writer, 0, sizeof(count_writer));
    count_writer.mode = mode;
    count_writer.primary_node = primary_node;
    netsurf_port_serialize_node(&count_writer, (dom_node *)root, 0, 0);
    status |= NETSURF_PORT_DOM_TEXT;
    if (count_writer.written + 1u > out_capacity) {
        result = -2;
        goto out;
    }

    memset(&write_writer, 0, sizeof(write_writer));
    write_writer.out = out;
    write_writer.capacity = out_capacity;
    write_writer.mode = mode;
    write_writer.primary_node = primary_node;
    if ((mode & NETSURF_PORT_MODE_MARK_PRIMARY) != 0u) {
        netsurf_port_hint_html = out;
    }
    netsurf_port_serialize_node(&write_writer, (dom_node *)root, 0, 0);
    if (write_writer.written < out_capacity) {
        out[write_writer.written] = '\0';
    } else {
        out[out_capacity - 1u] = '\0';
    }
    status |= NETSURF_PORT_DOM_SERIALIZE;
    if (write_writer.primary_marked != 0u) {
        status |= NETSURF_PORT_DOM_PRIMARY;
    }
    result = (int)write_writer.written;

out:
    if (primary_node != 0) {
        dom_node_unref(primary_node);
    }
    if (root != 0) {
        dom_node_unref(root);
    }
    if (parser != 0) {
        dom_hubbub_parser_destroy(parser);
        status |= NETSURF_PORT_DOM_CLEANUP;
    }
    if (document != 0) {
        dom_node_unref(document);
    }
    netsurf_port_last_dom_status = status;
    return result;
}

int netsurf_port_rewrite_html(const uint8_t *html, uint32_t len, uint8_t *out, uint32_t out_capacity) {
    return netsurf_port_rewrite_html_mode(html, len, out, out_capacity, 0u);
}

int netsurf_port_rewrite_render_html(const uint8_t *html, uint32_t len, uint8_t *out, uint32_t out_capacity) {
    return netsurf_port_rewrite_html_mode(html,
                                          len,
                                          out,
                                          out_capacity,
                                          NETSURF_PORT_MODE_OMIT_SCRIPT |
                                          NETSURF_PORT_MODE_MARK_PRIMARY);
}

uint32_t netsurf_port_parse_html_smoke(const char *html, size_t len) {
    dom_hubbub_parser_params params;
    dom_hubbub_parser *parser = 0;
    dom_document *document = 0;
    dom_element *root = 0;
    dom_string *root_name = 0;
    dom_string *text = 0;
    uint32_t status = 0;

    if (html == 0 && len != 0u) {
        netsurf_port_last_dom_status = 0;
        return 0;
    }

    memset(&params, 0, sizeof(params));
    params.enc = "UTF-8";
    params.fix_enc = true;
    params.enable_script = false;
    params.msg = netsurf_port_dom_msg;

    if (dom_hubbub_parser_create(&params, &parser, &document) == DOM_HUBBUB_OK &&
        parser != 0 &&
        document != 0) {
        status |= NETSURF_PORT_DOM_CREATE;
    }

    if (parser != 0 &&
        dom_hubbub_parser_parse_chunk(parser, (const uint8_t *)html, len) == DOM_HUBBUB_OK) {
        status |= NETSURF_PORT_DOM_PARSE;
    }
    if (parser != 0 && dom_hubbub_parser_completed(parser) == DOM_HUBBUB_OK) {
        status |= NETSURF_PORT_DOM_COMPLETE;
    }

    if (document != 0 &&
        dom_document_get_document_element(document, &root) == DOM_NO_ERR &&
        root != 0) {
        if (dom_node_get_node_name(root, &root_name) == DOM_NO_ERR &&
            netsurf_port_is_html_root(root_name)) {
            status |= NETSURF_PORT_DOM_ROOT;
        }
        if (dom_node_get_text_content(root, &text) == DOM_NO_ERR &&
            netsurf_port_dom_string_contains(text, "NetSurf DOM")) {
            status |= NETSURF_PORT_DOM_TEXT;
        }
    }

    if (text != 0) {
        dom_string_unref(text);
    }
    if (root_name != 0) {
        dom_string_unref(root_name);
    }
    if (root != 0) {
        dom_node_unref(root);
    }
    if (parser != 0) {
        dom_hubbub_parser_destroy(parser);
        status |= NETSURF_PORT_DOM_CLEANUP;
    }
    if (document != 0) {
        dom_node_unref(document);
    }

    netsurf_port_last_dom_status = status;
    return status == NETSURF_PORT_DOM_EXPECTED ? 1u : 0u;
}

uint32_t netsurf_port_dom_smoke(void) {
    static const char html[] =
        "<!doctype html><html><head><title>LainOS</title></head>"
        "<body><main><h1>NetSurf DOM</h1><p>adapter smoke</p></main></body></html>";

    return netsurf_port_parse_html_smoke(html, sizeof(html) - 1u);
}

static uint32_t netsurf_port_find_tag_pos_from_fragment(const uint8_t *html, const char *fragment) {
    const char *found;

    if (html == 0 || fragment == 0) {
        return 0xffffffffu;
    }
    found = strstr((const char *)html, fragment);
    if (found == 0) {
        return 0xffffffffu;
    }
    while (found > (const char *)html && found[0] != '<') {
        --found;
    }
    if (found[0] != '<') {
        return 0xffffffffu;
    }
    return (uint32_t)(found - (const char *)html);
}

uint32_t netsurf_port_render_smoke(void) {
    static const char html[] =
        "<!doctype html><html><head><title>render smoke</title>"
        "<script>console.log('skip');</script></head>"
        "<body><search><form><input type=\"search\" name=\"site\" value=\"docs\"></form></search>"
        "<div class=\"breadcrumbs\">trail</div>"
        "<div role=\"navigation\" class=\"toc\">chrome</div>"
        "<div aria-hidden=\"true\" id=\"aria-hidden-only\">still visible</div>"
        "<span role=\"heading\" aria-level=\"2\">Adapter Heading</span>"
        "<span class=\"sr-only\">assistive only</span>"
        "<span class=\"screen-reader-text\">wordpress helper</span>"
        "<div class=\"navbarish\">content should stay content</div>"
        "<div class=\"noprinter\">also content</div>"
        "<div role=\"main\"><form><fieldset><legend>Search</legend>"
        "<input type=\"hidden\" name=\"source\" value=\"smoke\">"
        "<input type=\"search\" name=\"q\" value=\"LainOS\"></fieldset></form>"
        "<p>NetSurf render smoke</p></div>"
        "<div id=\"catlinks\">categories</div></body></html>";
    static const char fallback_html[] =
        "<!doctype html><html><body>"
        "<div class=\"site-header\">Site navigation</div>"
        "<section class=\"post\"><p>Standalone post body with enough readable text to win primary selection.</p></section>"
        "</body></html>";
    uint8_t out[2048];
    uint8_t fallback_out[768];
    uint32_t display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    uint32_t role = NETSURF_PORT_HINT_ROLE_NONE;
    uint32_t flags = 0;
    uint32_t legend_pos;
    uint32_t main_pos;
    uint32_t nav_pos;
    uint32_t search_pos;
    uint32_t breadcrumb_pos;
    uint32_t aria_hidden_pos;
    uint32_t heading_pos;
    uint32_t sr_only_pos;
    uint32_t screen_reader_text_pos;
    uint32_t hidden_input_pos;
    uint32_t catlinks_pos;
    uint32_t navbarish_pos;
    uint32_t noprinter_pos;
    uint32_t site_header_pos;
    uint32_t post_pos;
    uint32_t required_status;
    int written;
    int fallback_written;

    written = netsurf_port_rewrite_render_html((const uint8_t *)html, sizeof(html) - 1u, out, sizeof(out));
    if (written <= 0) {
        return 0u;
    }
    required_status = NETSURF_PORT_DOM_EXPECTED |
                      NETSURF_PORT_DOM_SERIALIZE |
                      NETSURF_PORT_DOM_PRIMARY;
    if ((netsurf_port_last_dom_status & required_status) != required_status) {
        return 0u;
    }
    if (strstr((const char *)out, "<script") != 0 ||
        strstr((const char *)out, "console.log") != 0) {
        return 0u;
    }

    legend_pos = netsurf_port_find_tag_pos_from_fragment(out, "<legend");
    if (legend_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, legend_pos, &display, &role, &flags) == 0 ||
        display != NETSURF_PORT_HINT_DISPLAY_BLOCK) {
        return 0u;
    }

    main_pos = netsurf_port_find_tag_pos_from_fragment(out, "data-zbrowser-primary=\"1\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (main_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, main_pos, &display, &role, &flags) == 0 ||
        role != NETSURF_PORT_HINT_ROLE_PRIMARY ||
        (flags & NETSURF_PORT_HINT_FLAG_PRIMARY) == 0u) {
        return 0u;
    }

    nav_pos = netsurf_port_find_tag_pos_from_fragment(out, "role=\"navigation\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (nav_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, nav_pos, &display, &role, &flags) == 0 ||
        role != NETSURF_PORT_HINT_ROLE_CHROME) {
        return 0u;
    }

    search_pos = netsurf_port_find_tag_pos_from_fragment(out, "<search");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (search_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, search_pos, &display, &role, &flags) == 0 ||
        display != NETSURF_PORT_HINT_DISPLAY_BLOCK ||
        role != NETSURF_PORT_HINT_ROLE_CHROME) {
        return 0u;
    }

    breadcrumb_pos = netsurf_port_find_tag_pos_from_fragment(out, "class=\"breadcrumbs\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (breadcrumb_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, breadcrumb_pos, &display, &role, &flags) == 0 ||
        role != NETSURF_PORT_HINT_ROLE_CHROME) {
        return 0u;
    }

    aria_hidden_pos = netsurf_port_find_tag_pos_from_fragment(out, "id=\"aria-hidden-only\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (aria_hidden_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, aria_hidden_pos, &display, &role, &flags) == 0 ||
        (flags & NETSURF_PORT_HINT_FLAG_HIDDEN) != 0u) {
        return 0u;
    }

    heading_pos = netsurf_port_find_tag_pos_from_fragment(out, "role=\"heading\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (heading_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, heading_pos, &display, &role, &flags) == 0 ||
        role != NETSURF_PORT_HINT_ROLE_HEADING) {
        return 0u;
    }

    sr_only_pos = netsurf_port_find_tag_pos_from_fragment(out, "class=\"sr-only\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (sr_only_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, sr_only_pos, &display, &role, &flags) == 0 ||
        (flags & NETSURF_PORT_HINT_FLAG_SKIP) == 0u) {
        return 0u;
    }

    screen_reader_text_pos = netsurf_port_find_tag_pos_from_fragment(out, "class=\"screen-reader-text\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (screen_reader_text_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, screen_reader_text_pos, &display, &role, &flags) == 0 ||
        (flags & NETSURF_PORT_HINT_FLAG_SKIP) == 0u) {
        return 0u;
    }

    hidden_input_pos = netsurf_port_find_tag_pos_from_fragment(out, "type=\"hidden\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (hidden_input_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, hidden_input_pos, &display, &role, &flags) == 0 ||
        (flags & NETSURF_PORT_HINT_FLAG_HIDDEN) == 0u) {
        return 0u;
    }

    catlinks_pos = netsurf_port_find_tag_pos_from_fragment(out, "id=\"catlinks\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (catlinks_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, catlinks_pos, &display, &role, &flags) == 0 ||
        (flags & NETSURF_PORT_HINT_FLAG_SKIP) == 0u) {
        return 0u;
    }

    navbarish_pos = netsurf_port_find_tag_pos_from_fragment(out, "class=\"navbarish\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (navbarish_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, navbarish_pos, &display, &role, &flags) == 0 ||
        role == NETSURF_PORT_HINT_ROLE_CHROME) {
        return 0u;
    }

    noprinter_pos = netsurf_port_find_tag_pos_from_fragment(out, "class=\"noprinter\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (noprinter_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(out, noprinter_pos, &display, &role, &flags) == 0 ||
        (flags & NETSURF_PORT_HINT_FLAG_SKIP) != 0u) {
        return 0u;
    }

    fallback_written = netsurf_port_rewrite_render_html((const uint8_t *)fallback_html,
                                                        sizeof(fallback_html) - 1u,
                                                        fallback_out,
                                                        sizeof(fallback_out));
    if (fallback_written <= 0) {
        return 0u;
    }
    if ((netsurf_port_last_dom_status & required_status) != required_status) {
        return 0u;
    }

    site_header_pos = netsurf_port_find_tag_pos_from_fragment(fallback_out, "class=\"site-header\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (site_header_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(fallback_out, site_header_pos, &display, &role, &flags) == 0 ||
        role != NETSURF_PORT_HINT_ROLE_CHROME) {
        return 0u;
    }

    post_pos = netsurf_port_find_tag_pos_from_fragment(fallback_out,
                                                       "class=\"post\" data-zbrowser-primary=\"1\"");
    display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    role = NETSURF_PORT_HINT_ROLE_NONE;
    flags = 0;
    if (post_pos == 0xffffffffu ||
        netsurf_port_style_hint_for_tag(fallback_out, post_pos, &display, &role, &flags) == 0 ||
        role != NETSURF_PORT_HINT_ROLE_PRIMARY ||
        (flags & NETSURF_PORT_HINT_FLAG_PRIMARY) == 0u) {
        return 0u;
    }

    return 1u;
}
