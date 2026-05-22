#ifndef WEBLAYOUT_H
#define WEBLAYOUT_H

#include <stdint.h>

#define WEB_STYLE_FLAG_DISPLAY_NONE      (1u << 0)
#define WEB_STYLE_FLAG_VISIBILITY_HIDDEN (1u << 1)
#define WEB_STYLE_FLAG_HAS_TEXT_ALIGN    (1u << 2)
#define WEB_STYLE_FLAG_HAS_POSITION      (1u << 3)
#define WEB_STYLE_FLAG_HAS_LEFT          (1u << 4)
#define WEB_STYLE_FLAG_HAS_RIGHT         (1u << 5)
#define WEB_STYLE_FLAG_HAS_TOP           (1u << 6)
#define WEB_STYLE_FLAG_HAS_BOTTOM        (1u << 7)
#define WEB_STYLE_FLAG_HAS_WIDTH         (1u << 8)
#define WEB_STYLE_FLAG_HAS_HEIGHT        (1u << 9)
#define WEB_STYLE_FLAG_MARGIN_AUTO_X     (1u << 10)
#define WEB_STYLE_FLAG_CENTER_X          (1u << 11)
#define WEB_STYLE_FLAG_DISPLAY_FLEX      (1u << 12)
#define WEB_STYLE_FLAG_HAS_COLOR         (1u << 13)
#define WEB_STYLE_FLAG_HAS_BG_COLOR      (1u << 14)
#define WEB_STYLE_FLAG_HAS_MARGIN        (1u << 15)
#define WEB_STYLE_FLAG_HAS_PADDING       (1u << 16)
#define WEB_STYLE_FLAG_HAS_BORDER        (1u << 17)
#define WEB_STYLE_FLAG_HAS_BORDER_COLOR  (1u << 18)

#define WEB_STYLE_ALIGN_LEFT   0u
#define WEB_STYLE_ALIGN_CENTER 1u
#define WEB_STYLE_ALIGN_RIGHT  2u

#define WEB_STYLE_POS_STATIC   0u
#define WEB_STYLE_POS_ABSOLUTE 1u
#define WEB_STYLE_POS_FIXED    2u

typedef struct {
    uint32_t flags;
    uint32_t text_align;
    uint32_t position;
    uint32_t left;
    uint32_t right;
    uint32_t top;
    uint32_t bottom;
    uint32_t width;
    uint32_t height;
    uint32_t color;
    uint32_t background_color;
    uint32_t margin_left;
    uint32_t margin_right;
    uint32_t margin_top;
    uint32_t margin_bottom;
    uint32_t padding_left;
    uint32_t padding_right;
    uint32_t padding_top;
    uint32_t padding_bottom;
    uint32_t border_left;
    uint32_t border_right;
    uint32_t border_top;
    uint32_t border_bottom;
    uint32_t border_color;
} web_style_t;

typedef struct {
    uint64_t selectors;
    uint64_t selector_stride;
    uint64_t decls;
    uint64_t decl_stride;
    uint64_t rule_count;
} web_style_rules_t;

int web_style_for_tag(const uint8_t *html,
                      uint32_t tag_pos,
                      uint32_t viewport_width,
                      uint32_t viewport_height,
                      web_style_t *out_style);

int web_style_prepare_document(const uint8_t *html,
                               uint32_t viewport_width,
                               uint32_t viewport_height);

int web_style_for_cached_rules(const uint8_t *html,
                               uint32_t tag_pos,
                               uint32_t viewport_width,
                               uint32_t viewport_height,
                               const web_style_rules_t *rules,
                               web_style_t *out_style);

#endif
