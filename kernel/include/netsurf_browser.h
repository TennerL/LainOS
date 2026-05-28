#ifndef NETSURF_BROWSER_H
#define NETSURF_BROWSER_H

#include <stdint.h>

#define NETSURF_BROWSER_RENDER_CORE_PENDING 1u
#define NETSURF_BROWSER_RENDER_DOM_FALLBACK 2u
#define NETSURF_BROWSER_RENDER_FRONTEND_OK 4u
#define NETSURF_BROWSER_RENDER_BOX_TREE 8u

#if defined(__x86_64__) || defined(__i386__)
#define NETSURF_BROWSER_ENTRY __attribute__((force_align_arg_pointer))
#else
#define NETSURF_BROWSER_ENTRY
#endif

typedef struct netsurf_browser_render_result {
    uint32_t flags;
    uint32_t frontend_status;
    uint32_t dom_status;
    uint32_t input_bytes;
    uint32_t rendered_lines;
    uint32_t skipped_tags;
} netsurf_browser_render_result_t;

typedef struct netsurf_browser_view {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t scroll;
} netsurf_browser_view_t;

NETSURF_BROWSER_ENTRY int netsurf_browser_render_html(const uint8_t *url,
                                                      const uint8_t *html,
                                                      uint32_t len,
                                                      uint32_t x,
                                                      uint32_t y,
                                                      uint32_t width,
                                                      uint32_t height,
                                                      uint32_t scroll,
                                                      netsurf_browser_render_result_t *out_result);
NETSURF_BROWSER_ENTRY int netsurf_browser_render_html_view(const uint8_t *url,
                                                           const uint8_t *html,
                                                           uint32_t len,
                                                           const netsurf_browser_view_t *view);
NETSURF_BROWSER_ENTRY int netsurf_browser_prepare_html_view(const uint8_t *url,
                                                            const uint8_t *html,
                                                            uint32_t len,
                                                            const netsurf_browser_view_t *view);
NETSURF_BROWSER_ENTRY int netsurf_browser_mouse_html_view(const netsurf_browser_view_t *view,
                                                          uint32_t x,
                                                          uint32_t y,
                                                          uint32_t mouse_state);
NETSURF_BROWSER_ENTRY int netsurf_browser_scroll_html_view(const netsurf_browser_view_t *view,
                                                           uint32_t x,
                                                           uint32_t y,
                                                           int32_t scroll_x,
                                                           int32_t scroll_y);
NETSURF_BROWSER_ENTRY int netsurf_browser_key_event(uint32_t key);
NETSURF_BROWSER_ENTRY int netsurf_browser_consume_navigation(uint8_t *out,
                                                             uint32_t capacity);
NETSURF_BROWSER_ENTRY int netsurf_browser_consume_history_navigation(int32_t *out_direction);
uint32_t netsurf_browser_content_height(void);
void netsurf_browser_invalidate_cache(void);
int netsurf_browser_poll(void);
const char *netsurf_browser_status(void);

#endif
