#include "zbrowser_engine_abi.h"

static const zbrowser_engine_host_t *zbrowser_engine_host;
static const uint8_t zbrowser_engine_status_initial[] =
    "C engine ABI compiled; NetSurf core integration pending";
static const uint8_t zbrowser_engine_status_ready[] =
    "C engine ABI prepared raw document";

static const uint8_t *zbrowser_engine_status_text = zbrowser_engine_status_initial;

int zbrowser_engine_init_c(const zbrowser_engine_host_t *host) {
    zbrowser_engine_host = host;
    zbrowser_engine_status_text = zbrowser_engine_status_initial;
    return host != 0 && host->draw_text != 0 ? 0 : -1;
}

const uint8_t *zbrowser_engine_status_c(void) {
    return zbrowser_engine_status_text;
}

int zbrowser_engine_prepare_c(const uint8_t *url, const uint8_t *html, uint32_t size) {
    if (url == 0 || html == 0 || size == 0) {
        return -1;
    }
    zbrowser_engine_status_text = zbrowser_engine_status_ready;
    return 0;
}

int zbrowser_engine_draw_c(const uint8_t *html,
                           uint32_t size,
                           uint32_t scroll_line,
                           uint32_t viewport_width,
                           uint32_t viewport_height) {
    (void)html;
    (void)size;
    (void)scroll_line;
    (void)viewport_width;

    if (zbrowser_engine_host == 0 || zbrowser_engine_host->draw_text == 0 || viewport_height < 96u) {
        return -1;
    }

    zbrowser_engine_host->draw_text(10,
                                    76,
                                    (const uint8_t *)"C browser engine ABI is staged outside the kernel.",
                                    0xd8e1e8u,
                                    0x101820u);
    zbrowser_engine_host->draw_text(10,
                                    98,
                                    (const uint8_t *)"Next: replace the raw renderer with the staged NetSurf units.",
                                    0x9fb3c1u,
                                    0x101820u);
    return 0;
}
