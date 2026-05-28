#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "graphics.h"
#include "kmem.h"
#include "libc.h"
#include "netsurf_browser.h"
#include "netsurf_frontend.h"
#include "netsurf_port.h"
#include "utils/errors.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"

#define NETSURF_BROWSER_LINE_CAP 256u
#define NETSURF_BROWSER_SCAN_AHEAD_PX 900u

static char netsurf_browser_last_status[256] = "NetSurf browser core pending";
static netsurf_browser_render_result_t netsurf_browser_last_result;
static const uint8_t *netsurf_browser_cache_source;
static uint32_t netsurf_browser_cache_source_len;
static uint32_t netsurf_browser_cache_signature;
static int netsurf_browser_cache_valid;

void netsurf_core_invalidate_cache(void);

static void netsurf_browser_set_status(const char *status) {
    if (status == 0) {
        status = "";
    }
    strncpy(netsurf_browser_last_status, status, sizeof(netsurf_browser_last_status) - 1u);
    netsurf_browser_last_status[sizeof(netsurf_browser_last_status) - 1u] = '\0';
}

static int netsurf_browser_is_space(uint8_t ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static uint32_t netsurf_browser_signature(const uint8_t *html, uint32_t len) {
    uint32_t hash = 2166136261u;
    uint32_t step;
    uint32_t i;

    if (html == 0) {
        return 0;
    }
    hash ^= len;
    hash *= 16777619u;
    step = len > 4096u ? (len / 64u) : 1u;
    for (i = 0; i < len; i += step) {
        hash ^= html[i];
        hash *= 16777619u;
    }
    if (len != 0u) {
        uint32_t tail = len > 256u ? len - 256u : 0u;
        for (i = tail; i < len; ++i) {
            hash ^= html[i];
            hash *= 16777619u;
        }
    }
    return hash;
}

void netsurf_browser_invalidate_cache(void) {
    netsurf_browser_cache_source = 0;
    netsurf_browser_cache_source_len = 0;
    netsurf_browser_cache_signature = 0;
    netsurf_browser_cache_valid = 0;
    netsurf_core_invalidate_cache();
}

static int netsurf_browser_tag_breaks_line(const uint8_t *tag, uint32_t len) {
    static const char *break_tags[] = {
        "p", "/p", "div", "/div", "section", "/section", "article", "/article",
        "main", "/main", "header", "/header", "footer", "/footer", "aside", "/aside",
        "nav", "/nav", "br", "hr", "li", "/li", "ul", "/ul", "ol", "/ol",
        "table", "/table", "tr", "/tr", "td", "th", "h1", "/h1", "h2", "/h2",
        "h3", "/h3", "h4", "/h4", "h5", "/h5", "h6", "/h6", 0
    };
    uint32_t tag_len = 0;
    uint32_t i;

    while (tag_len < len &&
           tag[tag_len] != 0 &&
           tag[tag_len] != ' ' &&
           tag[tag_len] != '\t' &&
           tag[tag_len] != '\n' &&
           tag[tag_len] != '\r' &&
           tag[tag_len] != '>') {
        ++tag_len;
    }

    for (i = 0; break_tags[i] != 0; ++i) {
        const char *name = break_tags[i];
        uint32_t j = 0;
        while (j < tag_len && name[j] != 0) {
            char a = (char)tag[j];
            char b = name[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a + ('a' - 'A'));
            }
            if (a != b) {
                break;
            }
            ++j;
        }
        if (j == tag_len && name[j] == 0) {
            return 1;
        }
    }
    return 0;
}

typedef struct netsurf_browser_fallback_state {
    const struct redraw_context *ctx;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t scroll_px;
    uint32_t doc_y;
    uint32_t cursor_x;
    uint32_t indent;
    uint32_t line_height;
    uint32_t rendered_lines;
    uint32_t skipped_tags;
    uint32_t text_colour;
    uint32_t text_size;
    uint32_t heading_level;
    uint32_t link_depth;
    uint32_t skip_depth;
    char skip_tag[24];
    char word[128];
    uint32_t word_len;
    int pending_space;
} netsurf_browser_fallback_state_t;

static int netsurf_browser_tag_name_is(const char *tag, const char *name) {
    return strcmp(tag, name) == 0;
}

static int netsurf_browser_tag_is_void(const char *tag) {
    return netsurf_browser_tag_name_is(tag, "area") ||
           netsurf_browser_tag_name_is(tag, "base") ||
           netsurf_browser_tag_name_is(tag, "br") ||
           netsurf_browser_tag_name_is(tag, "col") ||
           netsurf_browser_tag_name_is(tag, "embed") ||
           netsurf_browser_tag_name_is(tag, "hr") ||
           netsurf_browser_tag_name_is(tag, "img") ||
           netsurf_browser_tag_name_is(tag, "input") ||
           netsurf_browser_tag_name_is(tag, "link") ||
           netsurf_browser_tag_name_is(tag, "meta") ||
           netsurf_browser_tag_name_is(tag, "param") ||
           netsurf_browser_tag_name_is(tag, "source") ||
           netsurf_browser_tag_name_is(tag, "track") ||
           netsurf_browser_tag_name_is(tag, "wbr");
}

static uint32_t netsurf_browser_font_px(uint32_t size) {
    if (size < 8u) {
        return 8u;
    }
    if (size > 28u) {
        return 28u;
    }
    return size;
}

static uint32_t netsurf_browser_glyph_width(uint32_t size) {
    uint32_t px = netsurf_browser_font_px(size);
    uint32_t width = (px * 6u) / 10u;
    return width < 5u ? 5u : width;
}

static void netsurf_browser_plot_text_line(const struct redraw_context *ctx,
                                           int x,
                                           int y,
                                           const char *line,
                                           uint32_t len,
                                           uint32_t colour,
                                           uint32_t size,
                                           uint32_t weight) {
    plot_font_style_t fstyle;

    if (len == 0u) {
        return;
    }
    memset(&fstyle, 0, sizeof(fstyle));
    fstyle.family = PLOT_FONT_FAMILY_SANS_SERIF;
    fstyle.size = (int)netsurf_browser_font_px(size) * PLOT_STYLE_SCALE;
    fstyle.weight = (int)weight;
    fstyle.foreground = colour;
    fstyle.background = 0xffffffu;
    (void)ctx->plot->text(ctx, &fstyle, x, y, line, len);
}

static int netsurf_browser_text_visible(const netsurf_browser_fallback_state_t *state,
                                        uint32_t top,
                                        uint32_t height) {
    if (top + height < state->scroll_px) {
        return 0;
    }
    if (top >= state->scroll_px + state->height) {
        return 0;
    }
    return 1;
}

static uint32_t netsurf_browser_screen_y(const netsurf_browser_fallback_state_t *state,
                                         uint32_t doc_y) {
    return state->y + doc_y - state->scroll_px;
}

static void netsurf_browser_new_line(netsurf_browser_fallback_state_t *state) {
    if (state->cursor_x != state->indent) {
        state->doc_y += state->line_height;
        state->cursor_x = state->indent;
    }
    state->pending_space = 0;
}

static void netsurf_browser_block_gap(netsurf_browser_fallback_state_t *state, uint32_t gap) {
    netsurf_browser_new_line(state);
    state->doc_y += gap;
}

static void netsurf_browser_draw_word(netsurf_browser_fallback_state_t *state,
                                      const char *word,
                                      uint32_t len) {
    uint32_t word_width;
    uint32_t space_width;
    uint32_t available;
    uint32_t draw_y;
    uint32_t colour = state->link_depth != 0u ? 0x0645ad : state->text_colour;
    uint32_t weight = state->heading_level != 0u ? 700u : 400u;

    if (len == 0u || state->skip_depth != 0u) {
        return;
    }
    word_width = len * netsurf_browser_glyph_width(state->text_size);
    space_width = netsurf_browser_glyph_width(state->text_size);
    available = state->width > 22u ? state->width - 22u : state->width;

    if (state->pending_space != 0 && state->cursor_x != state->indent) {
        if (state->cursor_x + space_width + word_width >= available) {
            netsurf_browser_new_line(state);
        } else {
            state->cursor_x += space_width;
        }
    }
    if (state->cursor_x != state->indent && state->cursor_x + word_width >= available) {
        netsurf_browser_new_line(state);
    }

    if (netsurf_browser_text_visible(state, state->doc_y, state->line_height)) {
        draw_y = netsurf_browser_screen_y(state, state->doc_y);
        netsurf_browser_plot_text_line(state->ctx,
                                       (int)(state->x + 12u + state->cursor_x),
                                       (int)(draw_y + state->text_size),
                                       word,
                                       len,
                                       colour,
                                       state->text_size,
                                       weight);
        state->rendered_lines++;
    }
    state->cursor_x += word_width;
    state->pending_space = 1;
}

static void netsurf_browser_flush_word(netsurf_browser_fallback_state_t *state) {
    if (state->word_len != 0u) {
        state->word[state->word_len] = 0;
        netsurf_browser_draw_word(state, state->word, state->word_len);
        state->word_len = 0;
    }
}

static void netsurf_browser_push_char(netsurf_browser_fallback_state_t *state, uint8_t ch) {
    if (state->skip_depth != 0u) {
        return;
    }
    if (netsurf_browser_is_space(ch)) {
        netsurf_browser_flush_word(state);
        state->pending_space = 1;
        return;
    }
    if (ch < 32u || ch >= 127u) {
        ch = '?';
    }
    if (state->word_len + 1u >= sizeof(state->word)) {
        netsurf_browser_flush_word(state);
    }
    state->word[state->word_len++] = (char)ch;
}

static uint32_t netsurf_browser_decode_entity(const uint8_t *html,
                                              uint32_t len,
                                              uint32_t pos,
                                              uint8_t *out_ch) {
    uint32_t i = pos + 1u;
    uint32_t value = 0;
    if (pos >= len || html[pos] != '&') {
        return 0;
    }
    while (i < len && i < pos + 12u && html[i] != ';') {
        ++i;
    }
    if (i >= len || html[i] != ';') {
        return 0;
    }
    if (i == pos + 4u && strncmp((const char *)html + pos, "&lt;", 4u) == 0) {
        *out_ch = '<';
    } else if (i == pos + 4u && strncmp((const char *)html + pos, "&gt;", 4u) == 0) {
        *out_ch = '>';
    } else if (i == pos + 5u && strncmp((const char *)html + pos, "&amp;", 5u) == 0) {
        *out_ch = '&';
    } else if (i == pos + 6u && strncmp((const char *)html + pos, "&quot;", 6u) == 0) {
        *out_ch = '"';
    } else if (i == pos + 6u && strncmp((const char *)html + pos, "&nbsp;", 6u) == 0) {
        *out_ch = ' ';
    } else if (pos + 3u < len && html[pos + 1u] == '#') {
        uint32_t j = pos + 2u;
        while (j < i && html[j] >= '0' && html[j] <= '9') {
            value = value * 10u + (uint32_t)(html[j] - '0');
            ++j;
        }
        *out_ch = value >= 32u && value < 127u ? (uint8_t)value : '?';
    } else {
        return 0;
    }
    return i - pos + 1u;
}

static void netsurf_browser_parse_tag_name(const uint8_t *tag,
                                           uint32_t len,
                                           char *out,
                                           uint32_t out_size,
                                           int *out_closing) {
    uint32_t i = 0;
    uint32_t used = 0;
    *out_closing = 0;
    if (out_size == 0u) {
        return;
    }
    if (i < len && tag[i] == '/') {
        *out_closing = 1;
        ++i;
    }
    while (i < len && netsurf_browser_is_space(tag[i])) {
        ++i;
    }
    while (i < len && used + 1u < out_size) {
        uint8_t ch = tag[i];
        if (ch == 0 || ch == '>' || ch == '/' || netsurf_browser_is_space(ch)) {
            break;
        }
        if (ch >= 'A' && ch <= 'Z') {
            ch = (uint8_t)(ch + ('a' - 'A'));
        }
        out[used++] = (char)ch;
        ++i;
    }
    out[used] = 0;
}

static uint32_t netsurf_browser_extract_attr(const uint8_t *tag,
                                             uint32_t len,
                                             const char *attr,
                                             char *out,
                                             uint32_t out_size) {
    uint32_t attr_len = (uint32_t)strlen(attr);
    uint32_t i;
    if (out_size == 0u) {
        return 0;
    }
    out[0] = 0;
    for (i = 0; i + attr_len < len; ++i) {
        uint32_t j;
        char quote;
        if (i != 0u && !netsurf_browser_is_space(tag[i - 1u])) {
            continue;
        }
        if (strncasecmp((const char *)tag + i, attr, attr_len) != 0) {
            continue;
        }
        j = i + attr_len;
        while (j < len && netsurf_browser_is_space(tag[j])) {
            ++j;
        }
        if (j >= len || tag[j] != '=') {
            continue;
        }
        ++j;
        while (j < len && netsurf_browser_is_space(tag[j])) {
            ++j;
        }
        if (j >= len) {
            return 0;
        }
        quote = (char)tag[j];
        if (quote == '"' || quote == '\'') {
            uint32_t used = 0;
            ++j;
            while (j < len && tag[j] != (uint8_t)quote && used + 1u < out_size) {
                out[used++] = (char)(tag[j] >= 32u && tag[j] < 127u ? tag[j] : '?');
                ++j;
            }
            out[used] = 0;
            return used;
        }
    }
    return 0;
}

static void netsurf_browser_draw_media_box(netsurf_browser_fallback_state_t *state,
                                           const char *label) {
    struct rect media;
    plot_style_t box_style;
    uint32_t top;
    uint32_t box_width;
    uint32_t box_height = 54u;

    if (state->skip_depth != 0u) {
        return;
    }
    netsurf_browser_block_gap(state, 4u);
    top = state->doc_y;
    box_width = state->width > 72u ? state->width - 40u - state->indent : state->width;

    if (netsurf_browser_text_visible(state, top, box_height)) {
        memset(&box_style, 0, sizeof(box_style));
        box_style.fill_type = PLOT_OP_TYPE_SOLID;
        box_style.fill_colour = 0xf6f8fau;
        box_style.stroke_type = PLOT_OP_TYPE_SOLID;
        box_style.stroke_colour = 0xa2a9b1u;
        box_style.stroke_width = plot_style_int_to_fixed(1);
        media.x0 = (int)(state->x + 12u + state->indent);
        media.y0 = (int)netsurf_browser_screen_y(state, top);
        media.x1 = (int)(media.x0 + box_width);
        media.y1 = (int)(media.y0 + box_height);
        (void)state->ctx->plot->rectangle(state->ctx, &box_style, &media);
        netsurf_browser_plot_text_line(state->ctx,
                                       media.x0 + 8,
                                       media.y0 + 26,
                                       label,
                                       (uint32_t)strlen(label),
                                       0x54595d,
                                       10u,
                                       400u);
    }
    state->doc_y += box_height + 8u;
    state->cursor_x = state->indent;
    state->pending_space = 0;
}

static void netsurf_browser_apply_heading(netsurf_browser_fallback_state_t *state,
                                          const char *tag) {
    if (netsurf_browser_tag_name_is(tag, "h1")) {
        state->heading_level = 1u;
        state->text_size = 22u;
        state->line_height = 30u;
    } else if (netsurf_browser_tag_name_is(tag, "h2")) {
        state->heading_level = 2u;
        state->text_size = 18u;
        state->line_height = 25u;
    } else {
        state->heading_level = 3u;
        state->text_size = 15u;
        state->line_height = 22u;
    }
    state->text_colour = 0x202122u;
}

static void netsurf_browser_apply_normal_text(netsurf_browser_fallback_state_t *state) {
    state->heading_level = 0;
    state->text_size = 11u;
    state->line_height = 17u;
    state->text_colour = 0x202122u;
}

static void netsurf_browser_handle_tag(netsurf_browser_fallback_state_t *state,
                                       const uint8_t *html,
                                       uint32_t tag_pos,
                                       const uint8_t *tag,
                                       uint32_t tag_len) {
    char name[24];
    char attr[96];
    int closing;
    uint32_t display = NETSURF_PORT_HINT_DISPLAY_UNKNOWN;
    uint32_t role = NETSURF_PORT_HINT_ROLE_NONE;
    uint32_t flags = 0;

    netsurf_browser_parse_tag_name(tag, tag_len, name, sizeof(name), &closing);
    if (name[0] == 0) {
        return;
    }

    if (state->skip_depth != 0u) {
        if (closing != 0 && strcmp(name, state->skip_tag) == 0) {
            state->skip_depth = 0;
            state->skip_tag[0] = 0;
        }
        return;
    }

    (void)netsurf_port_style_hint_for_tag(html, tag_pos, &display, &role, &flags);
    if (closing == 0 &&
        ((flags & (NETSURF_PORT_HINT_FLAG_SKIP | NETSURF_PORT_HINT_FLAG_HIDDEN)) != 0u ||
         role == NETSURF_PORT_HINT_ROLE_CHROME ||
         netsurf_browser_tag_name_is(name, "script") ||
         netsurf_browser_tag_name_is(name, "style") ||
         netsurf_browser_tag_name_is(name, "noscript") ||
         netsurf_browser_tag_name_is(name, "svg"))) {
        if (!netsurf_browser_tag_is_void(name)) {
            strncpy(state->skip_tag, name, sizeof(state->skip_tag) - 1u);
            state->skip_tag[sizeof(state->skip_tag) - 1u] = 0;
            state->skip_depth = 1u;
        }
        state->skipped_tags++;
        return;
    }

    if (closing != 0) {
        if (netsurf_browser_tag_name_is(name, "a") && state->link_depth != 0u) {
            state->link_depth--;
        }
        if (netsurf_browser_tag_name_is(name, "h1") ||
            netsurf_browser_tag_name_is(name, "h2") ||
            netsurf_browser_tag_name_is(name, "h3") ||
            netsurf_browser_tag_name_is(name, "h4") ||
            netsurf_browser_tag_name_is(name, "h5") ||
            netsurf_browser_tag_name_is(name, "h6")) {
            netsurf_browser_flush_word(state);
            netsurf_browser_block_gap(state, 8u);
            netsurf_browser_apply_normal_text(state);
        } else if (display == NETSURF_PORT_HINT_DISPLAY_BLOCK ||
                   display == NETSURF_PORT_HINT_DISPLAY_TABLE ||
                   display == NETSURF_PORT_HINT_DISPLAY_TABLE_ROW ||
                   netsurf_browser_tag_name_is(name, "p") ||
                   netsurf_browser_tag_name_is(name, "li") ||
                   netsurf_browser_tag_name_is(name, "tr")) {
            netsurf_browser_flush_word(state);
            netsurf_browser_block_gap(state, 4u);
        } else if (netsurf_browser_tag_name_is(name, "td") ||
                   netsurf_browser_tag_name_is(name, "th") ||
                   display == NETSURF_PORT_HINT_DISPLAY_TABLE_CELL) {
            netsurf_browser_flush_word(state);
            netsurf_browser_block_gap(state, 2u);
            if (state->indent >= 10u) {
                state->indent -= 10u;
                state->cursor_x = state->indent;
            }
        }
        return;
    }

    if (netsurf_browser_tag_name_is(name, "br")) {
        netsurf_browser_flush_word(state);
        netsurf_browser_block_gap(state, 0u);
    } else if (netsurf_browser_tag_name_is(name, "a")) {
        state->link_depth++;
    } else if (netsurf_browser_tag_name_is(name, "h1") ||
               netsurf_browser_tag_name_is(name, "h2") ||
               netsurf_browser_tag_name_is(name, "h3") ||
               netsurf_browser_tag_name_is(name, "h4") ||
               netsurf_browser_tag_name_is(name, "h5") ||
               netsurf_browser_tag_name_is(name, "h6")) {
        netsurf_browser_flush_word(state);
        netsurf_browser_block_gap(state, 10u);
        netsurf_browser_apply_heading(state, name);
    } else if (netsurf_browser_tag_name_is(name, "p") ||
               netsurf_browser_tag_name_is(name, "section") ||
               netsurf_browser_tag_name_is(name, "article") ||
               netsurf_browser_tag_name_is(name, "main") ||
               netsurf_browser_tag_name_is(name, "table") ||
               netsurf_browser_tag_name_is(name, "tr") ||
               display == NETSURF_PORT_HINT_DISPLAY_BLOCK ||
               display == NETSURF_PORT_HINT_DISPLAY_TABLE ||
               display == NETSURF_PORT_HINT_DISPLAY_TABLE_ROW) {
        netsurf_browser_flush_word(state);
        netsurf_browser_block_gap(state, 5u);
    } else if (netsurf_browser_tag_name_is(name, "li")) {
        netsurf_browser_flush_word(state);
        netsurf_browser_block_gap(state, 2u);
        netsurf_browser_draw_word(state, "*", 1u);
    } else if (netsurf_browser_tag_name_is(name, "td") ||
               netsurf_browser_tag_name_is(name, "th") ||
               display == NETSURF_PORT_HINT_DISPLAY_TABLE_CELL) {
        netsurf_browser_flush_word(state);
        netsurf_browser_block_gap(state, 2u);
        if (state->indent < 24u) {
            state->indent += 10u;
            state->cursor_x = state->indent;
        }
    } else if (netsurf_browser_tag_name_is(name, "img") ||
               netsurf_browser_tag_name_is(name, "picture") ||
               role == NETSURF_PORT_HINT_ROLE_MEDIA) {
        if (netsurf_browser_extract_attr(tag, tag_len, "alt", attr, sizeof(attr)) == 0u) {
            strncpy(attr, "[ image ]", sizeof(attr) - 1u);
            attr[sizeof(attr) - 1u] = 0;
        }
        netsurf_browser_flush_word(state);
        netsurf_browser_draw_media_box(state, attr);
    } else if (netsurf_browser_tag_name_is(name, "input") ||
               netsurf_browser_tag_name_is(name, "button") ||
               netsurf_browser_tag_name_is(name, "select") ||
               netsurf_browser_tag_name_is(name, "textarea")) {
        netsurf_browser_flush_word(state);
        netsurf_browser_draw_word(state, "[input]", 7u);
    }
    state->skipped_tags++;
}

static int netsurf_browser_render_dom_fallback(const uint8_t *html,
                                               uint32_t len,
                                               uint32_t x,
                                               uint32_t y,
                                               uint32_t width,
                                               uint32_t height,
                                               uint32_t scroll,
                                               netsurf_browser_render_result_t *out_result) {
    struct redraw_context ctx;
    struct rect clip;
    plot_style_t page_style;
    netsurf_browser_fallback_state_t state;
    uint32_t i = 0;

    if (html == 0 || len == 0u || width < 16u || height < 16u) {
        return -1;
    }

    memset(&page_style, 0, sizeof(page_style));
    page_style.fill_type = PLOT_OP_TYPE_SOLID;
    page_style.fill_colour = 0xffffffu;
    page_style.stroke_type = PLOT_OP_TYPE_SOLID;
    page_style.stroke_colour = 0xd8dee4u;
    page_style.stroke_width = plot_style_int_to_fixed(1);

    netsurf_kernel_redraw_context(&ctx, 0);
    clip.x0 = (int)x;
    clip.y0 = (int)y;
    clip.x1 = (int)(x + width);
    clip.y1 = (int)(y + height);
    (void)ctx.plot->clip(&ctx, &clip);
    (void)ctx.plot->rectangle(&ctx, &page_style, &clip);

    memset(&state, 0, sizeof(state));
    state.ctx = &ctx;
    state.x = x;
    state.y = y;
    state.width = width;
    state.height = height;
    state.scroll_px = scroll * 18u;
    state.doc_y = 12u;
    state.cursor_x = 0;
    state.indent = 0;
    netsurf_browser_apply_normal_text(&state);

    while (i < len && state.doc_y < state.scroll_px + height + NETSURF_BROWSER_SCAN_AHEAD_PX) {
        uint8_t ch = html[i];
        if (ch == '<') {
            uint32_t tag_start = i + 1u;
            uint32_t tag_len = 0;
            while (tag_start + tag_len < len && html[tag_start + tag_len] != '>') {
                ++tag_len;
            }
            netsurf_browser_flush_word(&state);
            netsurf_browser_handle_tag(&state, html, i, html + tag_start, tag_len);
            i = tag_start + tag_len;
        } else if (ch == '&') {
            uint8_t entity_ch = 0;
            uint32_t consumed = netsurf_browser_decode_entity(html, len, i, &entity_ch);
            if (consumed != 0u) {
                netsurf_browser_push_char(&state, entity_ch);
                i += consumed - 1u;
            } else {
                netsurf_browser_push_char(&state, ch);
            }
        } else {
            netsurf_browser_push_char(&state, ch);
        }
        ++i;
    }

    netsurf_browser_flush_word(&state);

    if (out_result != 0) {
        out_result->flags |= NETSURF_BROWSER_RENDER_DOM_FALLBACK;
        out_result->rendered_lines = state.rendered_lines;
        out_result->skipped_tags = state.skipped_tags;
    }
    return 0;
}

NETSURF_BROWSER_ENTRY int netsurf_core_render_html(const uint8_t *url,
                                                   const uint8_t *html,
                                                   uint32_t len,
                                                   uint32_t x,
                                                   uint32_t y,
                                                   uint32_t width,
                                                   uint32_t height,
                                                   uint32_t scroll,
                                                   netsurf_browser_render_result_t *out_result);
NETSURF_BROWSER_ENTRY int netsurf_core_prepare_html(const uint8_t *url,
                                                    const uint8_t *html,
                                                    uint32_t len,
                                                    uint32_t width,
                                                    uint32_t height,
                                                    netsurf_browser_render_result_t *out_result);
int netsurf_core_poll(void);
const char *netsurf_core_status(void);
int netsurf_core_mouse_event(uint32_t x, uint32_t y, uint32_t mouse_state);
int netsurf_core_scroll_event(uint32_t x, uint32_t y, int32_t scroll_x, int32_t scroll_y);
int netsurf_core_key_event(uint32_t key);
int netsurf_core_consume_navigation(uint8_t *out, uint32_t capacity);

int netsurf_browser_render_html(const uint8_t *url,
                                const uint8_t *html,
                                uint32_t len,
                                uint32_t x,
                                uint32_t y,
                                uint32_t width,
                                uint32_t height,
                                uint32_t scroll,
                                netsurf_browser_render_result_t *out_result) {
    uint32_t frontend_status;
    uint32_t signature;
    const char *core_status;
    char fallback_status[256];
    int cache_hit;
    int rc;

    if (out_result != 0) {
        memset(out_result, 0, sizeof(*out_result));
        out_result->input_bytes = len;
    }

    frontend_status = netsurf_kernel_frontend_smoke();
    if (out_result != 0) {
        out_result->frontend_status = frontend_status;
        out_result->dom_status = netsurf_port_dom_status();
        out_result->flags = NETSURF_BROWSER_RENDER_CORE_PENDING;
        if (frontend_status == 0x1fu) {
            out_result->flags |= NETSURF_BROWSER_RENDER_FRONTEND_OK;
        }
    }

    signature = netsurf_browser_signature(html, len);
    cache_hit = netsurf_browser_cache_valid != 0 &&
                netsurf_browser_cache_source == html &&
                netsurf_browser_cache_source_len == len &&
                netsurf_browser_cache_signature == signature;

    if (!cache_hit) {
        netsurf_browser_cache_source = html;
        netsurf_browser_cache_source_len = len;
        netsurf_browser_cache_signature = signature;
        netsurf_browser_cache_valid = 1;
    }

    if (html == 0 || len == 0u) {
        netsurf_browser_set_status("NetSurf browser render: allocation failed");
        return -1;
    }

    rc = netsurf_core_render_html(url,
                                  html,
                                  len,
                                  x,
                                  y,
                                  width,
                                  height,
                                  scroll * 18u,
                                  out_result);
    if (rc == 0) {
        netsurf_browser_set_status(netsurf_core_status());
        return 0;
    }
    core_status = netsurf_core_status();
    if (out_result != 0 &&
        (out_result->flags & NETSURF_BROWSER_RENDER_CORE_PENDING) != 0) {
        netsurf_browser_set_status(core_status);
        return 0;
    }

    rc = netsurf_browser_render_dom_fallback(html,
                                             len,
                                             x,
                                             y,
                                             width,
                                             height,
                                             scroll,
                                             out_result);

    if (out_result != 0) {
        out_result->dom_status = netsurf_port_dom_status();
    }

    if (rc == 0) {
        if (core_status != 0 && core_status[0] != 0) {
            snprintf(fallback_status,
                     sizeof(fallback_status),
                     "NetSurf browser render: DOM fallback after %s",
                     core_status);
            netsurf_browser_set_status(fallback_status);
        } else if (cache_hit) {
            netsurf_browser_set_status("NetSurf browser render: cached DOM fallback");
        } else {
            netsurf_browser_set_status("NetSurf browser render: structured DOM fallback");
        }
    } else {
        netsurf_browser_set_status("NetSurf browser render failed");
    }
    return rc;
}

const char *netsurf_browser_status(void) {
    return netsurf_browser_last_status;
}

int netsurf_browser_poll(void) {
    int rc = netsurf_core_poll();
    netsurf_browser_set_status(netsurf_core_status());
    return rc;
}

int netsurf_browser_prepare_html_view(const uint8_t *url,
                                      const uint8_t *html,
                                      uint32_t len,
                                      const netsurf_browser_view_t *view) {
    int rc;

    if (view == 0) {
        return -1;
    }
    memset(&netsurf_browser_last_result, 0, sizeof(netsurf_browser_last_result));
    netsurf_browser_last_result.input_bytes = len;
    netsurf_browser_last_result.frontend_status = netsurf_kernel_frontend_smoke();
    netsurf_browser_last_result.dom_status = netsurf_port_dom_status();
    netsurf_browser_last_result.flags = NETSURF_BROWSER_RENDER_CORE_PENDING;
    if (netsurf_browser_last_result.frontend_status == 0x1fu) {
        netsurf_browser_last_result.flags |= NETSURF_BROWSER_RENDER_FRONTEND_OK;
    }

    rc = netsurf_core_prepare_html(url,
                                   html,
                                   len,
                                   view->width,
                                   view->height,
                                   &netsurf_browser_last_result);
    if (rc == 0) {
        netsurf_browser_set_status(netsurf_core_status());
    } else if ((netsurf_browser_last_result.flags & NETSURF_BROWSER_RENDER_CORE_PENDING) != 0) {
        netsurf_browser_set_status(netsurf_core_status());
        return 0;
    } else {
        netsurf_browser_set_status(netsurf_core_status());
    }
    return rc;
}

int netsurf_browser_render_html_view(const uint8_t *url,
                                     const uint8_t *html,
                                     uint32_t len,
                                     const netsurf_browser_view_t *view) {
    if (view == 0) {
        return -1;
    }
    return netsurf_browser_render_html(url,
                                       html,
                                       len,
                                       view->x,
                                       view->y,
                                       view->width,
                                       view->height,
                                       view->scroll,
                                       &netsurf_browser_last_result);
}

NETSURF_BROWSER_ENTRY int netsurf_browser_mouse_html_view(const netsurf_browser_view_t *view,
                                                          uint32_t x,
                                                          uint32_t y,
                                                          uint32_t mouse_state) {
    uint32_t doc_x;
    uint32_t doc_y;

    if (view == 0 ||
        x < view->x ||
        y < view->y ||
        x >= view->x + view->width ||
        y >= view->y + view->height) {
        return 0;
    }

    doc_x = x - view->x;
    doc_y = y - view->y + view->scroll * 18u;
    return netsurf_core_mouse_event(doc_x, doc_y, mouse_state);
}

NETSURF_BROWSER_ENTRY int netsurf_browser_scroll_html_view(const netsurf_browser_view_t *view,
                                                           uint32_t x,
                                                           uint32_t y,
                                                           int32_t scroll_x,
                                                           int32_t scroll_y) {
    uint32_t doc_x;
    uint32_t doc_y;

    if (view == 0 ||
        x < view->x ||
        y < view->y ||
        x >= view->x + view->width ||
        y >= view->y + view->height) {
        return 0;
    }

    doc_x = x - view->x;
    doc_y = y - view->y + view->scroll * 18u;
    return netsurf_core_scroll_event(doc_x, doc_y, scroll_x, scroll_y);
}

NETSURF_BROWSER_ENTRY int netsurf_browser_key_event(uint32_t key) {
    return netsurf_core_key_event(key);
}

NETSURF_BROWSER_ENTRY int netsurf_browser_consume_navigation(uint8_t *out,
                                                             uint32_t capacity) {
    return netsurf_core_consume_navigation(out, capacity);
}
