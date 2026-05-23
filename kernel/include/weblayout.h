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
#define WEB_STYLE_FLAG_FLOAT_LEFT        (1u << 19)
#define WEB_STYLE_FLAG_FLOAT_RIGHT       (1u << 20)
#define WEB_STYLE_FLAG_DISPLAY_TABLE     (1u << 21)
#define WEB_STYLE_FLAG_DISPLAY_TABLE_ROW (1u << 22)
#define WEB_STYLE_FLAG_DISPLAY_TABLE_CELL (1u << 23)
#define WEB_STYLE_FLAG_FONT_BOLD         (1u << 24)
#define WEB_STYLE_FLAG_FONT_ITALIC       (1u << 25)
#define WEB_STYLE_FLAG_TEXT_UNDERLINE    (1u << 26)
#define WEB_STYLE_FLAG_HAS_MIN_WIDTH     (1u << 27)
#define WEB_STYLE_FLAG_HAS_MAX_WIDTH     (1u << 28)
#define WEB_STYLE_FLAG_HAS_MIN_HEIGHT    (1u << 29)
#define WEB_STYLE_FLAG_HAS_MAX_HEIGHT    (1u << 30)

#define WEB_STYLE_ALIGN_LEFT   0u
#define WEB_STYLE_ALIGN_CENTER 1u
#define WEB_STYLE_ALIGN_RIGHT  2u

#define WEB_STYLE_POS_STATIC   0u
#define WEB_STYLE_POS_ABSOLUTE 1u
#define WEB_STYLE_POS_FIXED    2u

#define WEB_STYLE_DISPLAY_INLINE     0u
#define WEB_STYLE_DISPLAY_BLOCK      1u
#define WEB_STYLE_DISPLAY_FLEX       2u
#define WEB_STYLE_DISPLAY_TABLE      3u
#define WEB_STYLE_DISPLAY_TABLE_ROW  4u
#define WEB_STYLE_DISPLAY_TABLE_CELL 5u
#define WEB_STYLE_DISPLAY_LIST_ITEM  6u

#define WEB_STYLE_FLOAT_NONE  0u
#define WEB_STYLE_FLOAT_LEFT  1u
#define WEB_STYLE_FLOAT_RIGHT 2u

#define WEB_STYLE_FONT_NORMAL 0u
#define WEB_STYLE_FONT_BOLD   1u

#define WEB_STYLE_FONT_STYLE_NORMAL 0u
#define WEB_STYLE_FONT_STYLE_ITALIC 1u

#define WEB_STYLE_TEXT_TRANSFORM_NONE       0u
#define WEB_STYLE_TEXT_TRANSFORM_UPPERCASE  1u
#define WEB_STYLE_TEXT_TRANSFORM_LOWERCASE  2u
#define WEB_STYLE_TEXT_TRANSFORM_CAPITALIZE 3u

#define WEB_STYLE_WHITE_SPACE_NORMAL   0u
#define WEB_STYLE_WHITE_SPACE_PRE      1u
#define WEB_STYLE_WHITE_SPACE_NOWRAP   2u
#define WEB_STYLE_WHITE_SPACE_PRE_WRAP 3u
#define WEB_STYLE_WHITE_SPACE_PRE_LINE 4u

#define WEB_STYLE_LIST_DISC    0u
#define WEB_STYLE_LIST_CIRCLE  1u
#define WEB_STYLE_LIST_SQUARE  2u
#define WEB_STYLE_LIST_DECIMAL 3u
#define WEB_STYLE_LIST_NONE    4u

#define WEB_STYLE_OVERFLOW_VISIBLE 0u
#define WEB_STYLE_OVERFLOW_HIDDEN  1u
#define WEB_STYLE_OVERFLOW_SCROLL  2u
#define WEB_STYLE_OVERFLOW_AUTO    3u

#define WEB_STYLE_BOX_CONTENT_BOX 0u
#define WEB_STYLE_BOX_BORDER_BOX  1u

#define WEB_STYLE_BORDER_SEPARATE 0u
#define WEB_STYLE_BORDER_COLLAPSE 1u

#define WEB_STYLE_CLEAR_NONE  0u
#define WEB_STYLE_CLEAR_LEFT  1u
#define WEB_STYLE_CLEAR_RIGHT 2u
#define WEB_STYLE_CLEAR_BOTH  3u

#define WEB_STYLE_VERTICAL_BASELINE 0u
#define WEB_STYLE_VERTICAL_SUB      1u
#define WEB_STYLE_VERTICAL_SUPER    2u
#define WEB_STYLE_VERTICAL_TOP      3u
#define WEB_STYLE_VERTICAL_MIDDLE   4u
#define WEB_STYLE_VERTICAL_BOTTOM   5u

#define WEB_STYLE_LIST_POSITION_OUTSIDE 0u
#define WEB_STYLE_LIST_POSITION_INSIDE  1u

#define WEB_STYLE_CAPTION_TOP    0u
#define WEB_STYLE_CAPTION_BOTTOM 1u

#define WEB_STYLE_DIRECTION_LTR 0u
#define WEB_STYLE_DIRECTION_RTL 1u

#define WEB_STYLE_TABLE_LAYOUT_AUTO  0u
#define WEB_STYLE_TABLE_LAYOUT_FIXED 1u

#define WEB_STYLE_EMPTY_CELLS_SHOW 0u
#define WEB_STYLE_EMPTY_CELLS_HIDE 1u

#define WEB_STYLE_BG_REPEAT      0u
#define WEB_STYLE_BG_REPEAT_X    1u
#define WEB_STYLE_BG_REPEAT_Y    2u
#define WEB_STYLE_BG_NO_REPEAT   3u

typedef struct {
    uint32_t flags;
    uint32_t text_align;
    uint32_t position;
    uint32_t display;
    uint32_t float_side;
    uint32_t font_weight;
    uint32_t font_style;
    uint32_t left;
    uint32_t right;
    uint32_t top;
    uint32_t bottom;
    uint32_t width;
    uint32_t height;
    uint32_t min_width;
    uint32_t max_width;
    uint32_t min_height;
    uint32_t max_height;
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
    uint32_t font_size;
    uint32_t line_height;
    uint32_t text_transform;
    uint32_t white_space;
    uint32_t list_style_type;
    uint32_t overflow_x;
    uint32_t overflow_y;
    uint32_t box_sizing;
    uint32_t border_collapse;
    uint32_t border_spacing_h;
    uint32_t border_spacing_v;
    uint32_t text_indent;
    uint32_t clear_side;
    uint32_t vertical_align;
    uint32_t list_style_position;
    uint32_t caption_side;
    uint32_t direction;
    uint32_t table_layout;
    uint32_t empty_cells;
    uint32_t opacity;
    uint32_t z_index;
    uint32_t letter_spacing;
    uint32_t word_spacing;
    uint32_t background_repeat;
    uint32_t background_position_x;
    uint32_t background_position_y;
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
