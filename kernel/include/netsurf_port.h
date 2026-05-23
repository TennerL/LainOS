#ifndef NETSURF_PORT_H
#define NETSURF_PORT_H

#include <stddef.h>
#include <stdint.h>

#define NETSURF_PORT_HINT_DISPLAY_UNKNOWN 255u
#define NETSURF_PORT_HINT_DISPLAY_INLINE 0u
#define NETSURF_PORT_HINT_DISPLAY_BLOCK 1u
#define NETSURF_PORT_HINT_DISPLAY_FLEX 2u
#define NETSURF_PORT_HINT_DISPLAY_TABLE 3u
#define NETSURF_PORT_HINT_DISPLAY_TABLE_ROW 4u
#define NETSURF_PORT_HINT_DISPLAY_TABLE_CELL 5u
#define NETSURF_PORT_HINT_DISPLAY_LIST_ITEM 6u

#define NETSURF_PORT_HINT_ROLE_NONE 0u
#define NETSURF_PORT_HINT_ROLE_PRIMARY 1u
#define NETSURF_PORT_HINT_ROLE_CHROME 2u
#define NETSURF_PORT_HINT_ROLE_TABLE 3u
#define NETSURF_PORT_HINT_ROLE_ROW 4u
#define NETSURF_PORT_HINT_ROLE_CELL 5u
#define NETSURF_PORT_HINT_ROLE_LIST 6u
#define NETSURF_PORT_HINT_ROLE_MEDIA 7u
#define NETSURF_PORT_HINT_ROLE_HEADING 8u
#define NETSURF_PORT_HINT_ROLE_FORM 9u

#define NETSURF_PORT_HINT_FLAG_SKIP 1u
#define NETSURF_PORT_HINT_FLAG_PRIMARY 2u
#define NETSURF_PORT_HINT_FLAG_STRUCTURAL 4u
#define NETSURF_PORT_HINT_FLAG_HIDDEN 8u

uint32_t netsurf_port_dom_smoke(void);
uint32_t netsurf_port_dom_status(void);
uint32_t netsurf_port_parse_html_smoke(const char *html, size_t len);
int netsurf_port_rewrite_html(const uint8_t *html, uint32_t len, uint8_t *out, uint32_t out_capacity);
int netsurf_port_rewrite_render_html(const uint8_t *html, uint32_t len, uint8_t *out, uint32_t out_capacity);
int netsurf_port_style_hint_for_tag(const uint8_t *html,
                                    uint32_t tag_pos,
                                    uint32_t *out_display,
                                    uint32_t *out_role,
                                    uint32_t *out_flags);
const char *netsurf_port_status(void);

#endif
