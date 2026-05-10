#ifndef GRAPHICS_H
#define GRAPHICS_H

#include <stdint.h>

enum {
    GRAPHICS_FORMAT_RGB = 0,
    GRAPHICS_FORMAT_BGR = 1,
    GRAPHICS_FORMAT_BITMASK = 2,
    GRAPHICS_FORMAT_BLT_ONLY = 3,
};

void graphics_init(uint64_t framebuffer_base,
                   uint32_t width,
                   uint32_t height,
                   uint32_t pixels_per_scanline,
                   uint32_t framebuffer_format);
uint32_t graphics_width(void);
uint32_t graphics_height(void);
uint32_t graphics_pitch(void);
uint32_t graphics_format(void);
uint32_t graphics_viewport_active(void);
uint32_t graphics_pack_color(uint32_t rgb_color);
void graphics_put_pixel(uint32_t x, uint32_t y, uint32_t rgb_color);
uint32_t graphics_get_pixel(uint32_t x, uint32_t y);
void graphics_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t rgb_color);
void graphics_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t rgb_color);
void graphics_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t rgb_color);
void graphics_clear(uint32_t rgb_color);
void graphics_viewport_push(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
void graphics_viewport_pop(void);

#endif
