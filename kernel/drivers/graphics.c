#include "graphics.h"

static uint64_t graphics_fb_base;
static uint32_t graphics_fb_width;
static uint32_t graphics_fb_height;
static uint32_t graphics_fb_pitch;
static uint32_t graphics_fb_format;
#define GRAPHICS_VIEWPORT_STACK_MAX 4u

typedef struct {
    uint32_t active;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} graphics_viewport_t;

static graphics_viewport_t graphics_viewport_stack[GRAPHICS_VIEWPORT_STACK_MAX];
static uint32_t graphics_viewport_depth;

static uint32_t abs_i32(int32_t value) {
    return value < 0 ? (uint32_t)(-value) : (uint32_t)value;
}

void graphics_init(uint64_t framebuffer_base,
                   uint32_t width,
                   uint32_t height,
                   uint32_t pixels_per_scanline,
                   uint32_t framebuffer_format) {
    graphics_fb_base = framebuffer_base;
    graphics_fb_width = width;
    graphics_fb_height = height;
    graphics_fb_pitch = pixels_per_scanline;
    graphics_fb_format = framebuffer_format;
}

uint32_t graphics_width(void) {
    if (graphics_viewport_depth > 0u) {
        return graphics_viewport_stack[graphics_viewport_depth - 1u].width;
    }
    return graphics_fb_width;
}

uint32_t graphics_height(void) {
    if (graphics_viewport_depth > 0u) {
        return graphics_viewport_stack[graphics_viewport_depth - 1u].height;
    }
    return graphics_fb_height;
}

uint32_t graphics_pitch(void) {
    return graphics_fb_pitch;
}

uint32_t graphics_format(void) {
    return graphics_fb_format;
}

uint32_t graphics_pack_color(uint32_t rgb_color) {
    uint32_t r = (rgb_color >> 16) & 0xFFu;
    uint32_t g = (rgb_color >> 8) & 0xFFu;
    uint32_t b = rgb_color & 0xFFu;

    if (graphics_fb_format == GRAPHICS_FORMAT_BGR) {
        return (b << 16) | (g << 8) | r;
    }

    return (r << 16) | (g << 8) | b;
}

void graphics_put_pixel(uint32_t x, uint32_t y, uint32_t rgb_color) {
    uint32_t *fb = (uint32_t *)(uintptr_t)graphics_fb_base;

    if (graphics_viewport_depth > 0u) {
        graphics_viewport_t *viewport = &graphics_viewport_stack[graphics_viewport_depth - 1u];
        if (x >= viewport->width || y >= viewport->height) {
            return;
        }
        x += viewport->x;
        y += viewport->y;
    }

    if (fb == 0 || x >= graphics_fb_width || y >= graphics_fb_height) {
        return;
    }

    fb[(uint64_t)y * graphics_fb_pitch + x] = graphics_pack_color(rgb_color);
}

uint32_t graphics_get_pixel(uint32_t x, uint32_t y) {
    uint32_t *fb = (uint32_t *)(uintptr_t)graphics_fb_base;
    uint32_t packed;
    uint32_t r;
    uint32_t g;
    uint32_t b;

    if (graphics_viewport_depth > 0u) {
        graphics_viewport_t *viewport = &graphics_viewport_stack[graphics_viewport_depth - 1u];
        if (x >= viewport->width || y >= viewport->height) {
            return 0;
        }
        x += viewport->x;
        y += viewport->y;
    }

    if (fb == 0 || x >= graphics_fb_width || y >= graphics_fb_height) {
        return 0;
    }

    packed = fb[(uint64_t)y * graphics_fb_pitch + x];
    if (graphics_fb_format == GRAPHICS_FORMAT_BGR) {
        b = (packed >> 16) & 0xFFu;
        g = (packed >> 8) & 0xFFu;
        r = packed & 0xFFu;
        return (r << 16) | (g << 8) | b;
    }

    return packed & 0x00FFFFFFu;
}

void graphics_fill_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t rgb_color) {
    uint32_t limit_w = graphics_width();
    uint32_t limit_h = graphics_height();
    uint32_t right = x + width;
    uint32_t bottom = y + height;

    if (x >= limit_w || y >= limit_h) {
        return;
    }
    if (right < x || right > limit_w) {
        right = limit_w;
    }
    if (bottom < y || bottom > limit_h) {
        bottom = limit_h;
    }

    for (uint32_t yy = y; yy < bottom; ++yy) {
        for (uint32_t xx = x; xx < right; ++xx) {
            graphics_put_pixel(xx, yy, rgb_color);
        }
    }
}

void graphics_draw_rect(uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t rgb_color) {
    uint32_t limit_w = graphics_width();
    uint32_t limit_h = graphics_height();
    uint32_t right;
    uint32_t bottom;

    if (width == 0 || height == 0) {
        return;
    }
    if (x >= limit_w || y >= limit_h) {
        return;
    }

    right = x + width - 1u;
    bottom = y + height - 1u;
    if (right < x || right >= limit_w) {
        right = limit_w - 1u;
    }
    if (bottom < y || bottom >= limit_h) {
        bottom = limit_h - 1u;
    }

    graphics_draw_line(x, y, right, y, rgb_color);
    graphics_draw_line(x, y, x, bottom, rgb_color);
    graphics_draw_line(right, y, right, bottom, rgb_color);
    graphics_draw_line(x, bottom, right, bottom, rgb_color);
}

void graphics_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t rgb_color) {
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t dx = (int32_t)abs_i32((int32_t)x1 - (int32_t)x0);
    int32_t dy = -(int32_t)abs_i32((int32_t)y1 - (int32_t)y0);
    int32_t err = dx + dy;
    int32_t x = (int32_t)x0;
    int32_t y = (int32_t)y0;

    for (;;) {
        if (x >= 0 && y >= 0) {
            graphics_put_pixel((uint32_t)x, (uint32_t)y, rgb_color);
        }

        if (x == (int32_t)x1 && y == (int32_t)y1) {
            break;
        }

        if ((err * 2) >= dy) {
            err += dy;
            x += sx;
        }
        if ((err * 2) <= dx) {
            err += dx;
            y += sy;
        }
    }
}

void graphics_clear(uint32_t rgb_color) {
    graphics_fill_rect(0, 0, graphics_width(), graphics_height(), rgb_color);
}

void graphics_viewport_push(uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    if (graphics_viewport_depth >= GRAPHICS_VIEWPORT_STACK_MAX) {
        return;
    }
    if (x >= graphics_fb_width || y >= graphics_fb_height) {
        width = 0;
        height = 0;
    } else {
        if (x + width < x || x + width > graphics_fb_width) {
            width = graphics_fb_width - x;
        }
        if (y + height < y || y + height > graphics_fb_height) {
            height = graphics_fb_height - y;
        }
    }

    graphics_viewport_stack[graphics_viewport_depth].active = 1u;
    graphics_viewport_stack[graphics_viewport_depth].x = x;
    graphics_viewport_stack[graphics_viewport_depth].y = y;
    graphics_viewport_stack[graphics_viewport_depth].width = width;
    graphics_viewport_stack[graphics_viewport_depth].height = height;
    ++graphics_viewport_depth;
}

void graphics_viewport_pop(void) {
    if (graphics_viewport_depth == 0u) {
        return;
    }
    --graphics_viewport_depth;
}
