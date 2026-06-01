#include <stdint.h>

#define ZBROWSER_ENGINE_LINE_CAPACITY 180u
#define ZBROWSER_ENGINE_STATUS_CAPACITY 128u
#define ZBROWSER_ENGINE_STYLE_SCAN_LIMIT 1048576u
#define ZBROWSER_ENGINE_DRAW_SCAN_LIMIT 262144u
#define ZBROWSER_ENGINE_DRAW_LINE_MARGIN 160u
#define ZBROWSER_ENGINE_RULES_MAX 96u
#define ZBROWSER_ENGINE_SELECTOR_CAPACITY 64u
#define ZBROWSER_ENGINE_ATTR_CAPACITY 96u
#define ZBROWSER_ENGINE_STYLE_STACK_MAX 128u

extern void gfx_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t color);
extern void draw_text_at_pixel(uint32_t x, uint32_t y, const uint8_t *text, uint32_t fg, uint32_t bg);
extern void draw_text_scaled_at_pixel(uint32_t x,
                                      uint32_t y,
                                      const uint8_t *text,
                                      uint32_t fg,
                                      uint32_t bg,
                                      uint32_t scale);

typedef struct {
    uint32_t fg;
    uint32_t bg;
    uint32_t has_fg;
    uint32_t has_bg;
    uint32_t selector_len;
    uint8_t selector[ZBROWSER_ENGINE_SELECTOR_CAPACITY];
} zbrowser_css_rule_t;

typedef struct {
    uint32_t fg;
    uint32_t bg;
    uint32_t link;
    uint32_t heading;
    uint32_t muted;
} zbrowser_css_theme_t;

typedef struct {
    const uint8_t *html;
    uint32_t size;
    uint32_t text_nodes;
    uint32_t style_rules;
    uint32_t matched_rules;
    uint32_t rule_count;
    zbrowser_css_theme_t css;
    zbrowser_css_rule_t rules[ZBROWSER_ENGINE_RULES_MAX];
} zbrowser_engine_doc_t;

typedef struct {
    uint8_t text[ZBROWSER_ENGINE_LINE_CAPACITY];
    uint32_t len;
    uint32_t y;
    uint32_t logical_line;
    uint32_t scroll_line;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t fg;
    uint32_t bg;
    uint32_t scale;
    uint32_t align;
    uint32_t stack_fg[ZBROWSER_ENGINE_STYLE_STACK_MAX];
    uint32_t stack_bg[ZBROWSER_ENGINE_STYLE_STACK_MAX];
    uint32_t stack_scale[ZBROWSER_ENGINE_STYLE_STACK_MAX];
    uint32_t stack_align[ZBROWSER_ENGINE_STYLE_STACK_MAX];
    uint32_t stack_depth;
} zbrowser_render_t;

static uint8_t zbrowser_engine_status_text[ZBROWSER_ENGINE_STATUS_CAPACITY];
static zbrowser_engine_doc_t zbrowser_doc;

static void zbrowser_copy(uint8_t *dst, uint32_t capacity, const uint8_t *src) {
    uint32_t i = 0;

    if (capacity == 0u) {
        return;
    }
    while (src[i] != 0u && i + 1u < capacity) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0u;
}

static void zbrowser_append(uint8_t *dst, uint32_t capacity, const uint8_t *src) {
    uint32_t len = 0;
    uint32_t i = 0;

    if (capacity == 0u) {
        return;
    }
    while (len + 1u < capacity && dst[len] != 0u) {
        ++len;
    }
    while (src[i] != 0u && len + 1u < capacity) {
        dst[len++] = src[i++];
    }
    dst[len] = 0u;
}

static void zbrowser_append_uint(uint8_t *dst, uint32_t capacity, uint32_t value) {
    uint8_t digits[12];
    uint32_t len = 0;
    uint32_t count = 0;

    while (len + 1u < capacity && dst[len] != 0u) {
        ++len;
    }
    if (len + 1u >= capacity) {
        return;
    }
    if (value == 0u) {
        dst[len] = '0';
        dst[len + 1u] = 0u;
        return;
    }
    while (value != 0u && count < sizeof(digits)) {
        digits[count++] = (uint8_t)('0' + (value % 10u));
        value /= 10u;
    }
    while (count != 0u && len + 1u < capacity) {
        dst[len++] = digits[--count];
    }
    dst[len] = 0u;
}

static uint32_t zbrowser_strlen(const uint8_t *text) {
    uint32_t len = 0;

    if (text == 0) {
        return 0;
    }
    while (text[len] != 0u) {
        ++len;
    }
    return len;
}

static uint8_t zbrowser_lower(uint8_t ch) {
    return ch >= 'A' && ch <= 'Z' ? (uint8_t)(ch + 32u) : ch;
}

static int zbrowser_ascii_space(uint8_t ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f';
}

static int zbrowser_name_char(uint8_t ch) {
    ch = zbrowser_lower(ch);
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
}

static int zbrowser_selector_char(uint8_t ch) {
    return zbrowser_name_char(ch) || ch == '.' || ch == '#' || ch == '*';
}

static int zbrowser_ci_equal_n(const uint8_t *a, const char *b, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        if (zbrowser_lower(a[i]) != (uint8_t)b[i]) {
            return 0;
        }
    }
    return b[n] == '\0';
}

static int zbrowser_ci_equal_buf(const uint8_t *a, uint32_t a_len, const uint8_t *b, uint32_t b_len) {
    if (a_len != b_len) {
        return 0;
    }
    for (uint32_t i = 0; i < a_len; ++i) {
        if (zbrowser_lower(a[i]) != zbrowser_lower(b[i])) {
            return 0;
        }
    }
    return 1;
}

static int zbrowser_ci_starts(const uint8_t *text, uint32_t size, uint32_t pos, const char *needle) {
    uint32_t i = 0;

    while (needle[i] != '\0') {
        if (pos + i >= size || zbrowser_lower(text[pos + i]) != (uint8_t)needle[i]) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static uint32_t zbrowser_skip_spaces(const uint8_t *text, uint32_t size, uint32_t pos) {
    while (pos < size && zbrowser_ascii_space(text[pos])) {
        ++pos;
    }
    return pos;
}

static uint32_t zbrowser_skip_css_ws_comments(const uint8_t *text, uint32_t size, uint32_t pos) {
    int again = 1;

    while (again) {
        again = 0;
        pos = zbrowser_skip_spaces(text, size, pos);
        if (pos + 1u < size && text[pos] == '/' && text[pos + 1u] == '*') {
            pos += 2u;
            while (pos + 1u < size && !(text[pos] == '*' && text[pos + 1u] == '/')) {
                ++pos;
            }
            if (pos + 1u < size) {
                pos += 2u;
            }
            again = 1;
        }
    }
    return pos;
}

static uint32_t zbrowser_skip_css_at_rule(const uint8_t *css, uint32_t size, uint32_t pos) {
    uint32_t depth = 0;

    while (pos < size) {
        if (pos + 1u < size && css[pos] == '/' && css[pos + 1u] == '*') {
            pos = zbrowser_skip_css_ws_comments(css, size, pos);
            continue;
        }
        if (css[pos] == '{') {
            ++depth;
        } else if (css[pos] == '}') {
            if (depth == 0u) {
                return pos + 1u;
            }
            --depth;
            if (depth == 0u) {
                return pos + 1u;
            }
        } else if (css[pos] == ';' && depth == 0u) {
            return pos + 1u;
        }
        ++pos;
    }
    return pos;
}

static int zbrowser_is_named_color(const uint8_t *name, uint32_t len, const char *match) {
    uint32_t i = 0;

    while (match[i] != '\0') {
        if (i >= len || zbrowser_lower(name[i]) != (uint8_t)match[i]) {
            return 0;
        }
        ++i;
    }
    return i == len;
}

static int zbrowser_hex_value(uint8_t ch) {
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0');
    }
    ch = zbrowser_lower(ch);
    if (ch >= 'a' && ch <= 'f') {
        return (int)(ch - 'a' + 10u);
    }
    return -1;
}

static uint32_t zbrowser_parse_uint_attr(const uint8_t *text) {
    uint32_t value = 0;
    uint32_t pos = 0;

    if (text == 0) {
        return 0;
    }
    while (text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10u + (uint32_t)(text[pos] - '0');
        ++pos;
    }
    return value;
}

static int zbrowser_ci_starts_buf(const uint8_t *value, uint32_t len, const char *match) {
    uint32_t i = 0;

    while (match[i] != '\0') {
        if (i >= len || zbrowser_lower(value[i]) != (uint8_t)match[i]) {
            return 0;
        }
        ++i;
    }
    return 1;
}

static uint32_t zbrowser_skip_css_color_separator(const uint8_t *value, uint32_t pos, uint32_t end) {
    while (pos < end && (zbrowser_ascii_space(value[pos]) || value[pos] == ',' || value[pos] == '/')) {
        ++pos;
    }
    return pos;
}

static int zbrowser_parse_rgb_component(const uint8_t *value,
                                        uint32_t *pos_ptr,
                                        uint32_t end,
                                        uint32_t *component) {
    uint32_t pos = *pos_ptr;
    uint32_t number = 0;
    int saw_digit = 0;
    int percent = 0;

    pos = zbrowser_skip_css_color_separator(value, pos, end);
    while (pos < end && value[pos] >= '0' && value[pos] <= '9') {
        number = number * 10u + (uint32_t)(value[pos] - '0');
        saw_digit = 1;
        ++pos;
    }
    if (!saw_digit) {
        return -1;
    }
    if (pos < end && value[pos] == '%') {
        percent = 1;
        ++pos;
    }
    if (percent) {
        if (number > 100u) {
            number = 100u;
        }
        number = (number * 255u) / 100u;
    } else if (number > 255u) {
        number = 255u;
    }
    *component = number;
    *pos_ptr = pos;
    return 0;
}

static int zbrowser_parse_rgb_color(const uint8_t *value, uint32_t len, uint32_t *out) {
    uint32_t pos = 0;
    uint32_t end = len;
    uint32_t r;
    uint32_t g;
    uint32_t b;

    while (pos < end && zbrowser_ascii_space(value[pos])) {
        ++pos;
    }
    if (pos + 4u >= end || !zbrowser_ci_starts_buf(value + pos, end - pos, "rgb")) {
        return -1;
    }
    pos += 3u;
    if (pos < end && zbrowser_lower(value[pos]) == 'a') {
        ++pos;
    }
    pos = zbrowser_skip_spaces(value, end, pos);
    if (pos >= end || value[pos] != '(') {
        return -1;
    }
    ++pos;
    if (zbrowser_parse_rgb_component(value, &pos, end, &r) != 0 ||
        zbrowser_parse_rgb_component(value, &pos, end, &g) != 0 ||
        zbrowser_parse_rgb_component(value, &pos, end, &b) != 0) {
        return -1;
    }
    *out = (r << 16u) | (g << 8u) | b;
    return 0;
}

static int zbrowser_parse_color_value(const uint8_t *value, uint32_t len, uint32_t *out) {
    uint32_t start = 0;
    uint32_t end = len;

    while (start < end && zbrowser_ascii_space(value[start])) {
        ++start;
    }
    while (end > start && zbrowser_ascii_space(value[end - 1u])) {
        --end;
    }
    if (end <= start) {
        return -1;
    }

    if (value[start] == '#') {
        uint32_t count = end - start - 1u;
        uint32_t color = 0;
        if (count == 3u) {
            for (uint32_t i = 0; i < 3u; ++i) {
                int v = zbrowser_hex_value(value[start + 1u + i]);
                if (v < 0) {
                    return -1;
                }
                color = (color << 8u) | (uint32_t)(v * 17);
            }
            *out = color;
            return 0;
        }
        if (count == 6u) {
            for (uint32_t i = 0; i < 6u; ++i) {
                int v = zbrowser_hex_value(value[start + 1u + i]);
                if (v < 0) {
                    return -1;
                }
                color = (color << 4u) | (uint32_t)v;
            }
            *out = color;
            return 0;
        }
    }

    if (zbrowser_parse_rgb_color(value + start, end - start, out) == 0) {
        return 0;
    }

    if (zbrowser_is_named_color(value + start, end - start, "black")) {
        *out = 0x000000u;
    } else if (zbrowser_is_named_color(value + start, end - start, "white")) {
        *out = 0xffffffu;
    } else if (zbrowser_is_named_color(value + start, end - start, "red")) {
        *out = 0xd83b35u;
    } else if (zbrowser_is_named_color(value + start, end - start, "green")) {
        *out = 0x2f8a42u;
    } else if (zbrowser_is_named_color(value + start, end - start, "blue")) {
        *out = 0x2d6cdfu;
    } else if (zbrowser_is_named_color(value + start, end - start, "navy")) {
        *out = 0x1f3f7au;
    } else if (zbrowser_is_named_color(value + start, end - start, "gray") ||
               zbrowser_is_named_color(value + start, end - start, "grey")) {
        *out = 0x808080u;
    } else if (zbrowser_is_named_color(value + start, end - start, "yellow")) {
        *out = 0xd4b72cu;
    } else if (zbrowser_is_named_color(value + start, end - start, "orange")) {
        *out = 0xd9822bu;
    } else if (zbrowser_is_named_color(value + start, end - start, "purple")) {
        *out = 0x8b5bd6u;
    } else if (zbrowser_is_named_color(value + start, end - start, "silver")) {
        *out = 0xc0c0c0u;
    } else if (zbrowser_is_named_color(value + start, end - start, "maroon")) {
        *out = 0x800000u;
    } else if (zbrowser_is_named_color(value + start, end - start, "olive")) {
        *out = 0x808000u;
    } else if (zbrowser_is_named_color(value + start, end - start, "lime")) {
        *out = 0x00ff00u;
    } else if (zbrowser_is_named_color(value + start, end - start, "teal")) {
        *out = 0x008080u;
    } else if (zbrowser_is_named_color(value + start, end - start, "aqua") ||
               zbrowser_is_named_color(value + start, end - start, "cyan")) {
        *out = 0x00ffffu;
    } else if (zbrowser_is_named_color(value + start, end - start, "fuchsia") ||
               zbrowser_is_named_color(value + start, end - start, "magenta")) {
        *out = 0xff00ffu;
    } else {
        return -1;
    }
    return 0;
}

static int zbrowser_selector_combinator(uint8_t ch) {
    return zbrowser_ascii_space(ch) || ch == '>' || ch == '+' || ch == '~';
}

static uint32_t zbrowser_selector_simple_end(const uint8_t *selector, uint32_t selector_len) {
    uint32_t end = 0;

    while (end < selector_len && zbrowser_ascii_space(selector[end])) {
        ++end;
    }
    while (end < selector_len && zbrowser_selector_char(selector[end])) {
        ++end;
    }
    return end;
}

static void zbrowser_selector_rightmost_compound(const uint8_t *selector,
                                                uint32_t selector_len,
                                                const uint8_t **out_selector,
                                                uint32_t *out_len) {
    uint32_t end = selector_len;
    uint32_t start;

    while (end > 0u && zbrowser_ascii_space(selector[end - 1u])) {
        --end;
    }
    start = end;
    while (start > 0u && !zbrowser_selector_combinator(selector[start - 1u])) {
        --start;
    }
    while (start < end && zbrowser_ascii_space(selector[start])) {
        ++start;
    }
    *out_selector = selector + start;
    *out_len = end - start;
}

static zbrowser_css_rule_t *zbrowser_find_or_add_rule(const uint8_t *selector, uint32_t selector_len) {
    uint32_t start = 0;
    uint32_t end;
    uint32_t simple_len;

    while (start < selector_len && zbrowser_ascii_space(selector[start])) {
        ++start;
    }
    selector += start;
    selector_len -= start;
    end = zbrowser_selector_simple_end(selector, selector_len);
    while (end > 0u && zbrowser_ascii_space(selector[end - 1u])) {
        --end;
    }
    simple_len = end;
    if (simple_len == 0u || simple_len >= ZBROWSER_ENGINE_SELECTOR_CAPACITY) {
        return 0;
    }

    for (uint32_t i = 0; i < zbrowser_doc.rule_count; ++i) {
        if (zbrowser_ci_equal_buf(zbrowser_doc.rules[i].selector,
                                  zbrowser_doc.rules[i].selector_len,
                                  selector,
                                  simple_len)) {
            return &zbrowser_doc.rules[i];
        }
    }

    if (zbrowser_doc.rule_count >= ZBROWSER_ENGINE_RULES_MAX) {
        return 0;
    }

    zbrowser_css_rule_t *rule = &zbrowser_doc.rules[zbrowser_doc.rule_count++];
    for (uint32_t i = 0; i < simple_len; ++i) {
        rule->selector[i] = selector[i];
    }
    rule->selector[simple_len] = 0u;
    rule->selector_len = simple_len;
    rule->has_fg = 0u;
    rule->has_bg = 0u;
    rule->fg = 0u;
    rule->bg = 0u;
    return rule;
}

static void zbrowser_apply_property_to_simple_selector(const uint8_t *selector,
                                                       uint32_t selector_len,
                                                       int full_selector_is_body,
                                                       int full_selector_is_link,
                                                       int full_selector_is_heading,
                                                       int full_selector_is_paragraph,
                                                       int is_color,
                                                       int is_bg,
                                                       uint32_t color) {
    zbrowser_css_rule_t *rule = zbrowser_find_or_add_rule(selector, selector_len);

    if (rule != 0) {
        if (is_bg) {
            rule->bg = color;
            rule->has_bg = 1u;
        } else {
            rule->fg = color;
            rule->has_fg = 1u;
        }
    }

    if (full_selector_is_body) {
        if (is_bg) {
            zbrowser_doc.css.bg = color;
        } else {
            zbrowser_doc.css.fg = color;
        }
    } else if (full_selector_is_link && is_color) {
        zbrowser_doc.css.link = color;
    } else if (full_selector_is_heading && is_color) {
        zbrowser_doc.css.heading = color;
    } else if (full_selector_is_paragraph && is_color) {
        zbrowser_doc.css.fg = color;
    }
}

static void zbrowser_apply_property(const uint8_t *selector_list,
                                    uint32_t selector_list_len,
                                    const uint8_t *property,
                                    uint32_t property_len,
                                    const uint8_t *value,
                                    uint32_t value_len) {
    uint32_t color = 0;
    int is_color = property_len == 5u && zbrowser_ci_equal_n(property, "color", 5u);
    int is_bg = (property_len == 10u && zbrowser_ci_equal_n(property, "background", 10u)) ||
                (property_len == 16u && zbrowser_ci_equal_n(property, "background-color", 16u));
    uint32_t part_start = 0;

    if (!is_color && !is_bg) {
        return;
    }
    if (zbrowser_parse_color_value(value, value_len, &color) != 0) {
        return;
    }

    while (part_start < selector_list_len) {
        uint32_t part_end = part_start;
        uint32_t trimmed_start;
        uint32_t trimmed_end;
        const uint8_t *simple_selector;
        uint32_t simple_len;
        int is_body;
        int is_link;
        int is_heading;
        int is_paragraph;

        while (part_end < selector_list_len && selector_list[part_end] != ',') {
            ++part_end;
        }
        trimmed_start = part_start;
        trimmed_end = part_end;
        while (trimmed_start < trimmed_end && zbrowser_ascii_space(selector_list[trimmed_start])) {
            ++trimmed_start;
        }
        while (trimmed_end > trimmed_start && zbrowser_ascii_space(selector_list[trimmed_end - 1u])) {
            --trimmed_end;
        }
        zbrowser_selector_rightmost_compound(selector_list + trimmed_start,
                                            trimmed_end - trimmed_start,
                                            &simple_selector,
                                            &simple_len);
        is_body = simple_len == 4u && zbrowser_ci_equal_n(simple_selector, "body", 4u);
        is_link = simple_len == 1u && zbrowser_ci_equal_n(simple_selector, "a", 1u);
        is_heading = simple_len == 2u &&
                     (zbrowser_ci_equal_n(simple_selector, "h1", 2u) ||
                      zbrowser_ci_equal_n(simple_selector, "h2", 2u) ||
                      zbrowser_ci_equal_n(simple_selector, "h3", 2u));
        is_paragraph = simple_len == 1u && zbrowser_ci_equal_n(simple_selector, "p", 1u);
        zbrowser_apply_property_to_simple_selector(simple_selector,
                                                   simple_len,
                                                   is_body,
                                                   is_link,
                                                   is_heading,
                                                   is_paragraph,
                                                   is_color,
                                                   is_bg,
                                                   color);
        part_start = part_end < selector_list_len ? part_end + 1u : selector_list_len;
    }
}

static uint32_t zbrowser_parse_declarations(const uint8_t *selector,
                                            uint32_t selector_len,
                                            const uint8_t *css,
                                            uint32_t size,
                                            uint32_t pos,
                                            uint32_t end) {
    while (pos < end) {
        uint32_t prop_start;
        uint32_t prop_end;
        uint32_t value_start;
        uint32_t value_end;

        pos = zbrowser_skip_css_ws_comments(css, size, pos);
        if (pos >= end || css[pos] == '}') {
            return pos;
        }
        prop_start = pos;
        while (pos < end && css[pos] != ':' && css[pos] != ';' && css[pos] != '}') {
            ++pos;
        }
        prop_end = pos;
        while (prop_end > prop_start && zbrowser_ascii_space(css[prop_end - 1u])) {
            --prop_end;
        }
        if (pos >= end || css[pos] != ':') {
            while (pos < end && css[pos] != ';' && css[pos] != '}') {
                ++pos;
            }
            if (pos < end && css[pos] == ';') {
                ++pos;
            }
            continue;
        }
        ++pos;
        value_start = zbrowser_skip_css_ws_comments(css, size, pos);
        while (pos < end && css[pos] != ';' && css[pos] != '}') {
            ++pos;
        }
        value_end = pos;
        while (value_end > value_start && zbrowser_ascii_space(css[value_end - 1u])) {
            --value_end;
        }
        zbrowser_apply_property(selector,
                                selector_len,
                                css + prop_start,
                                prop_end - prop_start,
                                css + value_start,
                                value_end - value_start);
        if (pos < end && css[pos] == ';') {
            ++pos;
        }
    }
    return pos;
}

static void zbrowser_parse_css_block(const uint8_t *css, uint32_t size) {
    uint32_t pos = 0;

    while (pos < size) {
        uint32_t selector_start;
        uint32_t selector_end;
        uint32_t decl_start;
        uint32_t decl_end;

        pos = zbrowser_skip_css_ws_comments(css, size, pos);
        if (pos >= size) {
            break;
        }
        if (css[pos] == '@') {
            pos = zbrowser_skip_css_at_rule(css, size, pos);
            continue;
        }
        selector_start = pos;
        while (pos < size && css[pos] != '{') {
            if (pos + 1u < size && css[pos] == '/' && css[pos + 1u] == '*') {
                pos = zbrowser_skip_css_ws_comments(css, size, pos);
                continue;
            }
            ++pos;
        }
        if (pos >= size) {
            break;
        }
        selector_end = pos;
        while (selector_end > selector_start && zbrowser_ascii_space(css[selector_end - 1u])) {
            --selector_end;
        }
        ++pos;
        decl_start = pos;
        while (pos < size && css[pos] != '}') {
            ++pos;
        }
        decl_end = pos;
        if (selector_end > selector_start) {
            (void)zbrowser_parse_declarations(css + selector_start,
                                              selector_end - selector_start,
                                              css,
                                              size,
                                              decl_start,
                                              decl_end);
            ++zbrowser_doc.style_rules;
        }
        if (pos < size && css[pos] == '}') {
            ++pos;
        }
    }
}

static uint32_t zbrowser_find_tag_end(const uint8_t *html, uint32_t size, uint32_t pos) {
    uint8_t quote = 0;

    while (pos < size) {
        uint8_t ch = html[pos];
        if (quote != 0u) {
            if (ch == quote) {
                quote = 0;
            }
        } else if (ch == '"' || ch == '\'') {
            quote = ch;
        } else if (ch == '>') {
            return pos;
        }
        ++pos;
    }
    return size;
}

static void zbrowser_parse_style_blocks(const uint8_t *html, uint32_t size) {
    uint32_t pos = 0;

    while (pos < size) {
        uint32_t start;
        uint32_t content_start;
        uint32_t content_end;
        uint32_t close_end;

        while (pos < size && html[pos] != '<') {
            ++pos;
        }
        if (pos >= size) {
            break;
        }
        if (!zbrowser_ci_starts(html, size, pos + 1u, "style")) {
            ++pos;
            continue;
        }
        start = zbrowser_find_tag_end(html, size, pos + 1u);
        if (start >= size) {
            break;
        }
        content_start = start + 1u;
        content_end = content_start;
        while (content_end < size && !zbrowser_ci_starts(html, size, content_end, "</style")) {
            ++content_end;
        }
        if (content_end > content_start) {
            zbrowser_parse_css_block(html + content_start, content_end - content_start);
        }
        close_end = zbrowser_find_tag_end(html, size, content_end + 1u);
        pos = close_end < size ? close_end + 1u : size;
    }
}

static void zbrowser_render_flush(zbrowser_render_t *r) {
    uint32_t draw_y;
    uint32_t draw_x = 10u;
    uint32_t char_w;
    uint32_t text_w;

    if (r->len == 0u) {
        return;
    }
    r->text[r->len] = 0u;
    if (r->logical_line >= r->scroll_line && r->y + 18u < r->viewport_height) {
        draw_y = r->y;
        char_w = r->scale > 1u ? 16u : 8u;
        text_w = r->len * char_w;
        if (r->align == 1u && r->viewport_width > text_w + 20u) {
            draw_x = (r->viewport_width - text_w) / 2u;
        }
        if (r->scale > 1u) {
            draw_text_scaled_at_pixel(draw_x, draw_y, r->text, r->fg, r->bg, r->scale);
            r->y += 30u;
        } else {
            draw_text_at_pixel(draw_x, draw_y, r->text, r->fg, r->bg);
            r->y += 17u;
        }
    }
    ++r->logical_line;
    r->len = 0u;
}

static void zbrowser_render_break(zbrowser_render_t *r) {
    zbrowser_render_flush(r);
    if (r->logical_line >= r->scroll_line && r->y + 18u < r->viewport_height) {
        r->y += 5u;
    }
    ++r->logical_line;
}

static void zbrowser_render_char(zbrowser_render_t *r, uint8_t ch) {
    uint32_t char_w = r->scale > 1u ? 16u : 8u;
    uint32_t max_chars = r->viewport_width > 30u ? (r->viewport_width - 20u) / char_w : 1u;

    if (max_chars >= ZBROWSER_ENGINE_LINE_CAPACITY) {
        max_chars = ZBROWSER_ENGINE_LINE_CAPACITY - 1u;
    }
    if (ch == '\n' || ch == '\r') {
        zbrowser_render_flush(r);
        return;
    }
    if (ch == '\t') {
        ch = ' ';
    }
    if (ch < 32u) {
        return;
    }
    if (ch == ' ' && r->len == 0u) {
        return;
    }
    if (r->len + 1u >= max_chars) {
        zbrowser_render_flush(r);
    }
    if (r->len < ZBROWSER_ENGINE_LINE_CAPACITY - 1u) {
        r->text[r->len++] = ch;
    }
}

static void zbrowser_style_push(zbrowser_render_t *r) {
    if (r->stack_depth >= ZBROWSER_ENGINE_STYLE_STACK_MAX) {
        return;
    }
    r->stack_fg[r->stack_depth] = r->fg;
    r->stack_bg[r->stack_depth] = r->bg;
    r->stack_scale[r->stack_depth] = r->scale;
    r->stack_align[r->stack_depth] = r->align;
    ++r->stack_depth;
}

static void zbrowser_style_pop(zbrowser_render_t *r) {
    if (r->stack_depth == 0u) {
        r->fg = zbrowser_doc.css.fg;
        r->bg = zbrowser_doc.css.bg;
        r->scale = 1u;
        r->align = 0u;
        return;
    }
    --r->stack_depth;
    r->fg = r->stack_fg[r->stack_depth];
    r->bg = r->stack_bg[r->stack_depth];
    r->scale = r->stack_scale[r->stack_depth];
    r->align = r->stack_align[r->stack_depth];
}

static void zbrowser_render_text(zbrowser_render_t *r, const uint8_t *text) {
    uint32_t pos = 0;

    while (text[pos] != 0u) {
        zbrowser_render_char(r, text[pos]);
        ++pos;
    }
}

static uint32_t zbrowser_decode_entity(const uint8_t *html, uint32_t size, uint32_t pos, uint8_t *out) {
    uint32_t end = pos + 1u;

    while (end < size && end - pos < 12u && html[end] != ';') {
        ++end;
    }
    if (end >= size || html[end] != ';') {
        *out = '&';
        return pos + 1u;
    }
    if (end - pos == 4u && zbrowser_ci_starts(html, size, pos, "&amp")) {
        *out = '&';
    } else if (end - pos == 3u && zbrowser_ci_starts(html, size, pos, "&lt")) {
        *out = '<';
    } else if (end - pos == 3u && zbrowser_ci_starts(html, size, pos, "&gt")) {
        *out = '>';
    } else if (end - pos == 5u && zbrowser_ci_starts(html, size, pos, "&quot")) {
        *out = '"';
    } else if (end - pos == 5u && zbrowser_ci_starts(html, size, pos, "&nbsp")) {
        *out = ' ';
    } else {
        *out = ' ';
    }
    return end + 1u;
}

static void zbrowser_apply_inline_style(const uint8_t *tag, uint32_t size, zbrowser_render_t *r) {
    uint32_t pos = 0;

    while (pos + 5u < size) {
        if (zbrowser_ci_starts(tag, size, pos, "style")) {
            uint32_t value_start;
            uint32_t value_end;
            uint8_t quote;
            uint32_t css_pos;
            pos += 5u;
            pos = zbrowser_skip_spaces(tag, size, pos);
            if (pos >= size || tag[pos] != '=') {
                continue;
            }
            ++pos;
            pos = zbrowser_skip_spaces(tag, size, pos);
            if (pos >= size || (tag[pos] != '"' && tag[pos] != '\'')) {
                continue;
            }
            quote = tag[pos++];
            value_start = pos;
            while (pos < size && tag[pos] != quote) {
                ++pos;
            }
            value_end = pos;
            css_pos = value_start;
            while (css_pos < value_end) {
                uint32_t prop_start;
                uint32_t prop_end;
                uint32_t color_start;
                uint32_t color_end;
                uint32_t color = 0;
                int is_color;
                int is_bg;

                css_pos = zbrowser_skip_spaces(tag, size, css_pos);
                prop_start = css_pos;
                while (css_pos < value_end && tag[css_pos] != ':' && tag[css_pos] != ';') {
                    ++css_pos;
                }
                prop_end = css_pos;
                while (prop_end > prop_start && zbrowser_ascii_space(tag[prop_end - 1u])) {
                    --prop_end;
                }
                if (css_pos >= value_end || tag[css_pos] != ':') {
                    if (css_pos < value_end) {
                        ++css_pos;
                    }
                    continue;
                }
                ++css_pos;
                color_start = zbrowser_skip_spaces(tag, size, css_pos);
                while (css_pos < value_end && tag[css_pos] != ';') {
                    ++css_pos;
                }
                color_end = css_pos;
                while (color_end > color_start && zbrowser_ascii_space(tag[color_end - 1u])) {
                    --color_end;
                }
                is_color = prop_end - prop_start == 5u && zbrowser_ci_equal_n(tag + prop_start, "color", 5u);
                is_bg = (prop_end - prop_start == 10u && zbrowser_ci_equal_n(tag + prop_start, "background", 10u)) ||
                        (prop_end - prop_start == 16u && zbrowser_ci_equal_n(tag + prop_start, "background-color", 16u));
                if ((is_color || is_bg) &&
                    zbrowser_parse_color_value(tag + color_start, color_end - color_start, &color) == 0) {
                    if (is_bg) {
                        r->bg = color;
                    } else {
                        r->fg = color;
                    }
                }
                if (css_pos < value_end && tag[css_pos] == ';') {
                    ++css_pos;
                }
            }
        }
        ++pos;
    }
}

static int zbrowser_tag_attr_value(const uint8_t *tag,
                                   uint32_t size,
                                   const char *name,
                                   uint8_t *out,
                                   uint32_t capacity) {
    uint32_t pos = 0;
    uint32_t name_len = 0;

    if (out == 0 || capacity == 0u) {
        return 0;
    }
    out[0] = 0u;
    while (name[name_len] != '\0') {
        ++name_len;
    }

    while (pos < size) {
        uint32_t attr_start;
        uint32_t attr_end;
        uint8_t quote = 0;

        while (pos < size && !zbrowser_name_char(tag[pos])) {
            ++pos;
        }
        attr_start = pos;
        while (pos < size && zbrowser_name_char(tag[pos])) {
            ++pos;
        }
        attr_end = pos;
        pos = zbrowser_skip_spaces(tag, size, pos);
        if (pos >= size || tag[pos] != '=') {
            continue;
        }
        ++pos;
        pos = zbrowser_skip_spaces(tag, size, pos);
        if (pos < size && (tag[pos] == '"' || tag[pos] == '\'')) {
            quote = tag[pos++];
        }
        if (attr_end - attr_start == name_len && zbrowser_ci_equal_n(tag + attr_start, name, name_len)) {
            uint32_t out_pos = 0;
            while (pos < size &&
                   ((quote != 0u && tag[pos] != quote) ||
                    (quote == 0u && !zbrowser_ascii_space(tag[pos]) && tag[pos] != '>'))) {
                if (out_pos + 1u < capacity) {
                    out[out_pos++] = tag[pos];
                }
                ++pos;
            }
            out[out_pos] = 0u;
            return out_pos != 0u;
        }
        while (pos < size &&
               ((quote != 0u && tag[pos] != quote) ||
                (quote == 0u && !zbrowser_ascii_space(tag[pos]) && tag[pos] != '>'))) {
            ++pos;
        }
        if (pos < size && quote != 0u && tag[pos] == quote) {
            ++pos;
        }
    }
    return 0;
}

static int zbrowser_class_list_contains(const uint8_t *classes, const uint8_t *name, uint32_t name_len) {
    uint32_t pos = 0;

    while (classes[pos] != 0u) {
        uint32_t start;
        uint32_t end;

        while (classes[pos] != 0u && zbrowser_ascii_space(classes[pos])) {
            ++pos;
        }
        start = pos;
        while (classes[pos] != 0u && !zbrowser_ascii_space(classes[pos])) {
            ++pos;
        }
        end = pos;
        if (end > start && zbrowser_ci_equal_buf(classes + start, end - start, name, name_len)) {
            return 1;
        }
    }
    return 0;
}

static void zbrowser_parse_legacy_body_attrs(const uint8_t *html, uint32_t size) {
    uint32_t pos = 0;
    uint8_t value[ZBROWSER_ENGINE_ATTR_CAPACITY];
    uint32_t color;

    while (pos < size) {
        uint32_t end;
        uint32_t name_start;
        uint32_t name_end;

        while (pos < size && html[pos] != '<') {
            ++pos;
        }
        if (pos >= size) {
            return;
        }
        end = zbrowser_find_tag_end(html, size, pos + 1u);
        if (end >= size) {
            return;
        }
        name_start = zbrowser_skip_spaces(html, size, pos + 1u);
        name_end = name_start;
        while (name_end < end && zbrowser_name_char(html[name_end])) {
            ++name_end;
        }
        if (name_end - name_start == 4u && zbrowser_ci_equal_n(html + name_start, "body", 4u)) {
            if (zbrowser_tag_attr_value(html + pos + 1u, end - pos - 1u, "bgcolor", value, sizeof(value)) &&
                zbrowser_parse_color_value(value, zbrowser_strlen(value), &color) == 0) {
                zbrowser_doc.css.bg = color;
            }
            if (zbrowser_tag_attr_value(html + pos + 1u, end - pos - 1u, "text", value, sizeof(value)) &&
                zbrowser_parse_color_value(value, zbrowser_strlen(value), &color) == 0) {
                zbrowser_doc.css.fg = color;
            }
            if (zbrowser_tag_attr_value(html + pos + 1u, end - pos - 1u, "link", value, sizeof(value)) &&
                zbrowser_parse_color_value(value, zbrowser_strlen(value), &color) == 0) {
                zbrowser_doc.css.link = color;
            }
            return;
        }
        pos = end + 1u;
    }
}

static int zbrowser_rule_matches_tag(const zbrowser_css_rule_t *rule,
                                     const uint8_t *tag_name,
                                     uint32_t tag_len,
                                     const uint8_t *tag,
                                     uint32_t tag_size) {
    const uint8_t *selector = rule->selector;
    uint32_t selector_len = rule->selector_len;
    uint32_t pos = 0;
    uint32_t tag_part_end = 0;
    uint8_t class_attr[ZBROWSER_ENGINE_ATTR_CAPACITY];
    uint8_t id_attr[ZBROWSER_ENGINE_ATTR_CAPACITY];

    if (selector_len == 1u && selector[0] == '*') {
        return 1;
    }

    while (tag_part_end < selector_len && selector[tag_part_end] != '.' && selector[tag_part_end] != '#') {
        ++tag_part_end;
    }
    if (tag_part_end != 0u && !zbrowser_ci_equal_buf(selector, tag_part_end, tag_name, tag_len)) {
        return 0;
    }
    pos = tag_part_end;

    while (pos < selector_len) {
        uint8_t prefix = selector[pos++];
        uint32_t value_start = pos;
        uint32_t value_len;

        while (pos < selector_len && selector[pos] != '.' && selector[pos] != '#') {
            ++pos;
        }
        value_len = pos - value_start;
        if (value_len == 0u) {
            return 0;
        }
        if (prefix == '.') {
            if (!zbrowser_tag_attr_value(tag, tag_size, "class", class_attr, sizeof(class_attr)) ||
                !zbrowser_class_list_contains(class_attr, selector + value_start, value_len)) {
                return 0;
            }
        } else if (prefix == '#') {
            uint32_t id_len = 0;
            if (!zbrowser_tag_attr_value(tag, tag_size, "id", id_attr, sizeof(id_attr))) {
                return 0;
            }
            while (id_attr[id_len] != 0u) {
                ++id_len;
            }
            if (!zbrowser_ci_equal_buf(id_attr, id_len, selector + value_start, value_len)) {
                return 0;
            }
        } else {
            return 0;
        }
    }

    return tag_part_end != 0u || selector[0] == '.' || selector[0] == '#';
}

static void zbrowser_apply_css_rules(const uint8_t *tag_name,
                                     uint32_t tag_len,
                                     const uint8_t *tag,
                                     uint32_t tag_size,
                                     zbrowser_render_t *r) {
    for (uint32_t i = 0; i < zbrowser_doc.rule_count; ++i) {
        const zbrowser_css_rule_t *rule = &zbrowser_doc.rules[i];
        if (!zbrowser_rule_matches_tag(rule, tag_name, tag_len, tag, tag_size)) {
            continue;
        }
        if (rule->has_bg) {
            r->bg = rule->bg;
        }
        if (rule->has_fg) {
            r->fg = rule->fg;
        }
        ++zbrowser_doc.matched_rules;
    }
}

static void zbrowser_draw_image_placeholder(const uint8_t *tag,
                                            uint32_t tag_size,
                                            zbrowser_render_t *r) {
    uint8_t attr_value[ZBROWSER_ENGINE_ATTR_CAPACITY];
    uint32_t width = 88u;
    uint32_t height = 31u;
    uint32_t x = 10u;
    uint32_t y;

    zbrowser_render_flush(r);
    if (zbrowser_tag_attr_value(tag, tag_size, "width", attr_value, sizeof(attr_value))) {
        width = zbrowser_parse_uint_attr(attr_value);
    }
    if (zbrowser_tag_attr_value(tag, tag_size, "height", attr_value, sizeof(attr_value))) {
        height = zbrowser_parse_uint_attr(attr_value);
    }
    if (width == 0u) {
        width = 88u;
    }
    if (height == 0u) {
        height = 31u;
    }
    if (width > r->viewport_width - 20u) {
        width = r->viewport_width > 20u ? r->viewport_width - 20u : 1u;
    }
    if (height > 96u) {
        height = 96u;
    }
    if (r->align == 1u && r->viewport_width > width) {
        x = (r->viewport_width - width) / 2u;
    }
    if (r->logical_line >= r->scroll_line && r->y + height + 18u < r->viewport_height) {
        y = r->y;
        gfx_fill_rect(x, y, width, height, 0xd9ead3u);
        gfx_fill_rect(x + 1u, y + 1u, width > 2u ? width - 2u : 1u, height > 2u ? height - 2u : 1u, 0xf4fff0u);
        if (zbrowser_tag_attr_value(tag, tag_size, "alt", attr_value, sizeof(attr_value))) {
            draw_text_at_pixel(x + 4u, y + 6u, attr_value, 0x19790cu, 0xf4fff0u);
        } else {
            draw_text_at_pixel(x + 4u, y + 6u, (const uint8_t *)"[image]", 0x19790cu, 0xf4fff0u);
        }
        r->y += height + 8u;
    }
    ++r->logical_line;
}

static int zbrowser_draw_table_cell_box(const uint8_t *tag,
                                        uint32_t tag_size,
                                        zbrowser_render_t *r) {
    uint8_t attr_value[ZBROWSER_ENGINE_ATTR_CAPACITY];
    uint32_t width = 0;
    uint32_t height = 10u;
    uint32_t color = 0;
    uint32_t x = 10u;

    if (!zbrowser_tag_attr_value(tag, tag_size, "bgcolor", attr_value, sizeof(attr_value)) ||
        zbrowser_parse_color_value(attr_value, zbrowser_strlen(attr_value), &color) != 0) {
        return 0;
    }
    if (zbrowser_tag_attr_value(tag, tag_size, "width", attr_value, sizeof(attr_value))) {
        width = zbrowser_parse_uint_attr(attr_value);
    }
    if (zbrowser_tag_attr_value(tag, tag_size, "height", attr_value, sizeof(attr_value))) {
        height = zbrowser_parse_uint_attr(attr_value);
    }
    if (width == 0u) {
        width = 40u;
    }
    if (height == 0u) {
        height = 10u;
    }
    if (width > r->viewport_width - 20u) {
        width = r->viewport_width > 20u ? r->viewport_width - 20u : 1u;
    }
    if (height > 48u) {
        height = 48u;
    }
    zbrowser_render_flush(r);
    if (r->align == 1u && r->viewport_width > width) {
        x = (r->viewport_width - width) / 2u;
    }
    if (r->logical_line >= r->scroll_line && r->y + height + 18u < r->viewport_height) {
        gfx_fill_rect(x, r->y, width, height, color);
        r->y += height + 4u;
    }
    ++r->logical_line;
    return 1;
}

static uint32_t zbrowser_skip_element(const uint8_t *html, uint32_t size, uint32_t pos, const char *name) {
    while (pos < size) {
        if (pos + 1u < size &&
            html[pos] == '<' &&
            html[pos + 1u] == '/' &&
            zbrowser_ci_starts(html, size, pos + 2u, name)) {
            uint32_t end = zbrowser_find_tag_end(html, size, pos + 2u);
            return end < size ? end + 1u : size;
        }
        ++pos;
    }
    return pos;
}

static uint32_t zbrowser_handle_tag(const uint8_t *html, uint32_t size, uint32_t pos, zbrowser_render_t *r) {
    uint32_t end = zbrowser_find_tag_end(html, size, pos + 1u);
    uint32_t name_start;
    uint32_t name_end;
    int closing = 0;
    int self_closing = 0;

    if (end >= size) {
        return size;
    }
    if (pos + 1u < end && (html[pos + 1u] == '!' || html[pos + 1u] == '?')) {
        return end + 1u;
    }
    name_start = pos + 1u;
    if (name_start < end && html[name_start] == '/') {
        closing = 1;
        ++name_start;
    }
    name_start = zbrowser_skip_spaces(html, size, name_start);
    name_end = name_start;
    while (name_end < end && zbrowser_name_char(html[name_end])) {
        ++name_end;
    }
    if (name_end == name_start) {
        return end + 1u;
    }
    if (end > pos && html[end - 1u] == '/') {
        self_closing = 1;
    }

    if (!closing) {
        uint8_t attr_value[ZBROWSER_ENGINE_ATTR_CAPACITY];
        uint32_t parsed_color;
        zbrowser_style_push(r);
        if (name_end - name_start == 6u && zbrowser_ci_equal_n(html + name_start, "script", 6u)) {
            zbrowser_style_pop(r);
            return zbrowser_skip_element(html, size, end + 1u, "script");
        }
        if (name_end - name_start == 5u && zbrowser_ci_equal_n(html + name_start, "style", 5u)) {
            zbrowser_style_pop(r);
            return zbrowser_skip_element(html, size, end + 1u, "style");
        }
        if ((name_end - name_start == 2u && zbrowser_ci_equal_n(html + name_start, "br", 2u)) ||
            (name_end - name_start == 2u && zbrowser_ci_equal_n(html + name_start, "li", 2u))) {
            zbrowser_render_flush(r);
            if (name_end - name_start == 2u && zbrowser_ci_equal_n(html + name_start, "li", 2u)) {
                zbrowser_render_char(r, '*');
                zbrowser_render_char(r, ' ');
            }
        } else if ((name_end - name_start == 1u && zbrowser_ci_equal_n(html + name_start, "p", 1u)) ||
                   (name_end - name_start == 3u && zbrowser_ci_equal_n(html + name_start, "div", 3u))) {
            zbrowser_render_break(r);
            r->fg = zbrowser_doc.css.fg;
            r->scale = 1u;
        } else if (name_end - name_start == 6u && zbrowser_ci_equal_n(html + name_start, "center", 6u)) {
            zbrowser_render_break(r);
            r->align = 1u;
        } else if (name_end - name_start == 4u && zbrowser_ci_equal_n(html + name_start, "body", 4u)) {
            r->fg = zbrowser_doc.css.fg;
            r->bg = zbrowser_doc.css.bg;
        } else if (name_end - name_start == 1u && zbrowser_ci_equal_n(html + name_start, "a", 1u)) {
            r->fg = zbrowser_doc.css.link;
        } else if (name_end - name_start == 2u &&
                   (zbrowser_ci_equal_n(html + name_start, "h1", 2u) ||
                    zbrowser_ci_equal_n(html + name_start, "h2", 2u) ||
                    zbrowser_ci_equal_n(html + name_start, "h3", 2u))) {
            zbrowser_render_break(r);
            r->fg = zbrowser_doc.css.heading;
            r->scale = zbrowser_ci_equal_n(html + name_start, "h1", 2u) ? 2u : 1u;
        } else if (name_end - name_start == 4u && zbrowser_ci_equal_n(html + name_start, "font", 4u)) {
            if (zbrowser_tag_attr_value(html + pos + 1u, end - pos - 1u, "color", attr_value, sizeof(attr_value)) &&
                zbrowser_parse_color_value(attr_value, zbrowser_strlen(attr_value), &parsed_color) == 0) {
                r->fg = parsed_color;
            }
        } else if (name_end - name_start == 3u && zbrowser_ci_equal_n(html + name_start, "img", 3u)) {
            zbrowser_draw_image_placeholder(html + pos + 1u, end - pos - 1u, r);
        } else if (name_end - name_start == 5u && zbrowser_ci_equal_n(html + name_start, "table", 5u)) {
            if (zbrowser_tag_attr_value(html + pos + 1u, end - pos - 1u, "bgcolor", attr_value, sizeof(attr_value)) &&
                zbrowser_parse_color_value(attr_value, zbrowser_strlen(attr_value), &parsed_color) == 0) {
                r->bg = parsed_color;
            }
        } else if (name_end - name_start == 2u && zbrowser_ci_equal_n(html + name_start, "tr", 2u)) {
            zbrowser_render_flush(r);
        } else if (name_end - name_start == 2u && zbrowser_ci_equal_n(html + name_start, "td", 2u)) {
            (void)zbrowser_draw_table_cell_box(html + pos + 1u, end - pos - 1u, r);
        } else if (name_end - name_start == 5u && zbrowser_ci_equal_n(html + name_start, "input", 5u)) {
            if (zbrowser_tag_attr_value(html + pos + 1u, end - pos - 1u, "type", attr_value, sizeof(attr_value)) &&
                zbrowser_ci_equal_n(attr_value, "hidden", 6u)) {
                zbrowser_style_pop(r);
                return end + 1u;
            }
            zbrowser_render_flush(r);
            if (zbrowser_tag_attr_value(html + pos + 1u, end - pos - 1u, "value", attr_value, sizeof(attr_value))) {
                zbrowser_render_text(r, (const uint8_t *)"[ ");
                zbrowser_render_text(r, attr_value);
                zbrowser_render_text(r, (const uint8_t *)" ]");
            } else {
                zbrowser_render_text(r, (const uint8_t *)"[ ____ ]");
            }
            zbrowser_render_flush(r);
        }
        zbrowser_apply_css_rules(html + name_start,
                                 name_end - name_start,
                                 html + pos + 1u,
                                 end - pos - 1u,
                                 r);
        zbrowser_apply_inline_style(html + pos + 1u, end - pos - 1u, r);
        if (self_closing ||
            (name_end - name_start == 2u && zbrowser_ci_equal_n(html + name_start, "br", 2u)) ||
            (name_end - name_start == 3u && zbrowser_ci_equal_n(html + name_start, "img", 3u)) ||
            (name_end - name_start == 4u &&
             (zbrowser_ci_equal_n(html + name_start, "link", 4u) ||
              zbrowser_ci_equal_n(html + name_start, "meta", 4u))) ||
            (name_end - name_start == 5u && zbrowser_ci_equal_n(html + name_start, "input", 5u))) {
            zbrowser_style_pop(r);
        }
    } else {
        if ((name_end - name_start == 2u &&
             (zbrowser_ci_equal_n(html + name_start, "h1", 2u) ||
              zbrowser_ci_equal_n(html + name_start, "h2", 2u) ||
              zbrowser_ci_equal_n(html + name_start, "h3", 2u))) ||
            (name_end - name_start == 1u && zbrowser_ci_equal_n(html + name_start, "p", 1u)) ||
            (name_end - name_start == 2u && zbrowser_ci_equal_n(html + name_start, "tr", 2u)) ||
            (name_end - name_start == 3u && zbrowser_ci_equal_n(html + name_start, "div", 3u)) ||
            (name_end - name_start == 4u && zbrowser_ci_equal_n(html + name_start, "form", 4u)) ||
            (name_end - name_start == 5u && zbrowser_ci_equal_n(html + name_start, "table", 5u)) ||
            (name_end - name_start == 6u && zbrowser_ci_equal_n(html + name_start, "center", 6u))) {
            zbrowser_render_break(r);
        }
        zbrowser_style_pop(r);
    }

    return end + 1u;
}

int zbrowser_engine_init(void) {
    zbrowser_doc.css.fg = 0xd8e1e8u;
    zbrowser_doc.css.bg = 0x101820u;
    zbrowser_doc.css.link = 0x73b7ffu;
    zbrowser_doc.css.heading = 0xf7d154u;
    zbrowser_doc.css.muted = 0x9fb3c1u;
    zbrowser_copy(zbrowser_engine_status_text,
                  sizeof(zbrowser_engine_status_text),
                  (const uint8_t *)"C NetSurf engine object linked; CSS-aware renderer active");
    return 0;
}

uint8_t *zbrowser_engine_status(void) {
    return zbrowser_engine_status_text;
}

int zbrowser_engine_prepare(const uint8_t *url, const uint8_t *html, uint32_t size) {
    uint32_t style_scan_size;

    (void)url;

    zbrowser_engine_init();
    zbrowser_doc.html = html;
    zbrowser_doc.size = size;
    zbrowser_doc.text_nodes = 0;
    zbrowser_doc.style_rules = 0;
    zbrowser_doc.matched_rules = 0;
    zbrowser_doc.rule_count = 0;

    if (html == 0 || size == 0u) {
        zbrowser_copy(zbrowser_engine_status_text,
                      sizeof(zbrowser_engine_status_text),
                      (const uint8_t *)"C engine has no document");
        return -1;
    }

    style_scan_size = size > ZBROWSER_ENGINE_STYLE_SCAN_LIMIT ? ZBROWSER_ENGINE_STYLE_SCAN_LIMIT : size;
    zbrowser_parse_legacy_body_attrs(html, style_scan_size);
    zbrowser_parse_style_blocks(html, style_scan_size);
    zbrowser_copy(zbrowser_engine_status_text,
                  sizeof(zbrowser_engine_status_text),
                  (const uint8_t *)"C CSS-aware renderer active; blocks=");
    zbrowser_append_uint(zbrowser_engine_status_text, sizeof(zbrowser_engine_status_text), zbrowser_doc.style_rules);
    zbrowser_append(zbrowser_engine_status_text, sizeof(zbrowser_engine_status_text), (const uint8_t *)" rules=");
    zbrowser_append_uint(zbrowser_engine_status_text, sizeof(zbrowser_engine_status_text), zbrowser_doc.rule_count);
    return 0;
}

int zbrowser_engine_draw(const uint8_t *html,
                         uint32_t size,
                         uint32_t scroll_line,
                         uint32_t viewport_width,
                         uint32_t viewport_height) {
    zbrowser_render_t r;
    uint32_t pos = 0;
    uint32_t draw_scan_size = size;
    uint32_t max_logical_line = scroll_line + ZBROWSER_ENGINE_DRAW_LINE_MARGIN;
    int last_space = 1;

    if (viewport_width == 0u) {
        return -1;
    }
    gfx_fill_rect(0u, 58u, viewport_width, viewport_height > 82u ? viewport_height - 82u : 0u, zbrowser_doc.css.bg);

    if (html == 0 || size == 0u) {
        draw_text_at_pixel(10,
                           76,
                           (const uint8_t *)"C browser engine object is linked into zbrowser_netsurf.",
                           zbrowser_doc.css.fg,
                           zbrowser_doc.css.bg);
        draw_text_at_pixel(10,
                           98,
                           (const uint8_t *)"Load a page to exercise the CSS-aware renderer.",
                           zbrowser_doc.css.muted,
                           zbrowser_doc.css.bg);
        return 0;
    }

    r.len = 0;
    r.y = 70u;
    r.logical_line = 0;
    r.scroll_line = scroll_line;
    r.viewport_width = viewport_width;
    r.viewport_height = viewport_height;
    r.fg = zbrowser_doc.css.fg;
    r.bg = zbrowser_doc.css.bg;
    r.scale = 1u;
    r.align = 0u;
    r.stack_depth = 0u;

    if (draw_scan_size > ZBROWSER_ENGINE_DRAW_SCAN_LIMIT) {
        draw_scan_size = ZBROWSER_ENGINE_DRAW_SCAN_LIMIT;
    }

    while (pos < draw_scan_size && r.y + 18u < viewport_height && r.logical_line < max_logical_line) {
        uint8_t ch = html[pos];
        if (ch == '<') {
            pos = zbrowser_handle_tag(html, draw_scan_size, pos, &r);
            last_space = 1;
            continue;
        }
        if (ch == '&') {
            pos = zbrowser_decode_entity(html, draw_scan_size, pos, &ch);
        } else {
            ++pos;
        }
        if (zbrowser_ascii_space(ch)) {
            if (!last_space) {
                zbrowser_render_char(&r, ' ');
            }
            last_space = 1;
        } else {
            zbrowser_render_char(&r, ch);
            last_space = 0;
        }
    }
    zbrowser_render_flush(&r);
    return 0;
}
