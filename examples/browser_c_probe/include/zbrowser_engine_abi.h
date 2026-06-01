#ifndef ZBROWSER_ENGINE_ABI_H
#define ZBROWSER_ENGINE_ABI_H

#include <stdint.h>

typedef void (*zbrowser_engine_draw_text_fn)(uint32_t x,
                                             uint32_t y,
                                             const uint8_t *text,
                                             uint32_t fg,
                                             uint32_t bg);

typedef struct {
    zbrowser_engine_draw_text_fn draw_text;
} zbrowser_engine_host_t;

typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t scroll;
} zbrowser_engine_view_t;

int zbrowser_engine_init_c(const zbrowser_engine_host_t *host);
const uint8_t *zbrowser_engine_status_c(void);
int zbrowser_engine_prepare_c(const uint8_t *url, const uint8_t *html, uint32_t size);
int zbrowser_engine_prepare_html_view_c(const uint8_t *url,
                                        const uint8_t *html,
                                        uint32_t size,
                                        const zbrowser_engine_view_t *view);
int zbrowser_engine_render_html_view_c(const uint8_t *url,
                                       const uint8_t *html,
                                       uint32_t size,
                                       const zbrowser_engine_view_t *view);
uint32_t zbrowser_engine_root_color_c(void);
uint32_t zbrowser_engine_root_background_c(void);
uint32_t zbrowser_engine_root_display_c(void);
int zbrowser_engine_draw_c(const uint8_t *html,
                           uint32_t size,
                           uint32_t scroll_line,
                           uint32_t viewport_width,
                           uint32_t viewport_height);
int zbrowser_engine_poll_c(void);
void zbrowser_engine_invalidate_cache_c(void);

#endif
