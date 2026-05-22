#include "weblayout.h"

#include <stddef.h>

enum {
    WEB_PROP_DISPLAY = 0,
    WEB_PROP_VISIBILITY,
    WEB_PROP_TEXT_ALIGN,
    WEB_PROP_POSITION,
    WEB_PROP_LEFT,
    WEB_PROP_RIGHT,
    WEB_PROP_TOP,
    WEB_PROP_BOTTOM,
    WEB_PROP_WIDTH,
    WEB_PROP_HEIGHT,
    WEB_PROP_MARGIN_AUTO_X,
    WEB_PROP_CENTER_X,
    WEB_PROP_COUNT
};

typedef struct {
    web_style_t style;
    uint32_t score[WEB_PROP_COUNT];
    uint32_t order;
} web_style_state_t;

#define WEB_STYLE_MAX_CACHED_RULES 1024u

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
static web_cached_rule_t web_cached_rules[WEB_STYLE_MAX_CACHED_RULES];

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
    state->style.flags &= ~(WEB_STYLE_FLAG_DISPLAY_NONE | WEB_STYLE_FLAG_DISPLAY_FLEX);
    if (web_range_contains_cstr_ci(value, start, end, "none")) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_NONE;
    } else if (web_range_contains_cstr_ci(value, start, end, "flex")) {
        state->style.flags |= WEB_STYLE_FLAG_DISPLAY_FLEX;
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
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "inset")) {
            uint32_t px = 0;
            if (web_parse_length_px(css, value_start, value_end, viewport_width, viewport_width, viewport_height, &px)) {
                web_set_length_property(state, WEB_PROP_LEFT, WEB_STYLE_FLAG_HAS_LEFT, &state->style.left, px, specificity, important);
                web_set_length_property(state, WEB_PROP_RIGHT, WEB_STYLE_FLAG_HAS_RIGHT, &state->style.right, px, specificity, important);
                web_set_length_property(state, WEB_PROP_TOP, WEB_STYLE_FLAG_HAS_TOP, &state->style.top, px, specificity, important);
                web_set_length_property(state, WEB_PROP_BOTTOM, WEB_STYLE_FLAG_HAS_BOTTOM, &state->style.bottom, px, specificity, important);
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
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "justify-content")) {
            if (web_range_contains_cstr_ci(css, value_start, value_end, "center")) {
                web_set_flag_property(state, WEB_PROP_CENTER_X, WEB_STYLE_FLAG_CENTER_X, 1, specificity, important);
            }
        } else if (web_range_equal_cstr_ci(css, prop_start, prop_end, "transform")) {
            if (web_range_contains_cstr_ci(css, value_start, value_end, "translate") &&
                web_range_contains_cstr_ci(css, value_start, value_end, "-50")) {
                web_set_flag_property(state, WEB_PROP_CENTER_X, WEB_STYLE_FLAG_CENTER_X, 1, specificity, important);
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
    if (html == NULL) {
        web_prepared_cache_reset(NULL, 0, 0);
        return -1;
    }
    web_prepared_cache_reset(html, viewport_width, viewport_height);
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
    return web_cached_rule_saturated ? -(int)web_cached_rule_count : (int)web_cached_rule_count;
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
    state->style.left = 0;
    state->style.right = 0;
    state->style.top = 0;
    state->style.bottom = 0;
    state->style.width = 0;
    state->style.height = 0;
    for (uint32_t i = 0; i < WEB_PROP_COUNT; ++i) {
        state->score[i] = 0;
    }
    state->order = 1;
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
    if (web_attr_value_range_cstr(html, tag_pos, "aria-hidden", &attr_start, &attr_end) &&
        web_range_contains_cstr_ci(html, attr_start, attr_end, "true")) {
        web_set_flag_property(state, WEB_PROP_VISIBILITY, WEB_STYLE_FLAG_VISIBILITY_HIDDEN, 1, 65535, 1);
    }
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
    if (html == NULL || out_style == NULL || html[tag_pos] != '<' || web_is_closing_tag(html, tag_pos)) {
        return -1;
    }
    web_style_state_t state;
    web_state_init(&state);
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
