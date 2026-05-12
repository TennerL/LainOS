#include "graphics.h"
#include "dma.h"
#include "kernel.h"

static uint64_t graphics_fb_base;
static uint32_t graphics_fb_width;
static uint32_t graphics_fb_height;
static uint32_t graphics_fb_pitch;
static uint32_t graphics_fb_format;
static uint32_t *graphics_backbuffer;
static uint32_t graphics_backbuffer_active_flag;
#define GRAPHICS_VIEWPORT_STACK_MAX 4u
#define GRAPHICS_SMP_MIN_PIXELS 65536u
#define GRAPHICS_SMP_MAX_JOBS 8u

typedef struct {
    uint32_t active;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} graphics_viewport_t;

static graphics_viewport_t graphics_viewport_stack[GRAPHICS_VIEWPORT_STACK_MAX];
static uint32_t graphics_viewport_depth;

typedef struct {
    uint32_t *fb;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t color;
} graphics_fill_job_t;

typedef struct {
    uint32_t *fb;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t total_height;
    uint32_t step_start;
    uint32_t pitch;
    uint32_t top_color;
    uint32_t bottom_color;
} graphics_gradient_job_t;

typedef struct {
    uint32_t *dst;
    const uint32_t *src;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
} graphics_blit_job_t;

static uint32_t abs_i32(int32_t value) {
    return value < 0 ? (uint32_t)(-value) : (uint32_t)value;
}

static uint32_t graphics_mix_packed_color(uint32_t top, uint32_t bottom, uint32_t step, uint32_t steps) {
    uint32_t tr = (top >> 16) & 0xffu;
    uint32_t tg = (top >> 8) & 0xffu;
    uint32_t tb = top & 0xffu;
    uint32_t br = (bottom >> 16) & 0xffu;
    uint32_t bg = (bottom >> 8) & 0xffu;
    uint32_t bb = bottom & 0xffu;

    if (steps == 0u) {
        steps = 1u;
    }

    return (((tr * (steps - step) + br * step) / steps) << 16) |
           (((tg * (steps - step) + bg * step) / steps) << 8) |
           ((tb * (steps - step) + bb * step) / steps);
}

static void graphics_fill_job(void *arg) {
    graphics_fill_job_t *job = (graphics_fill_job_t *)arg;

    for (uint32_t yy = 0; yy < job->height; ++yy) {
        uint32_t *row = job->fb + (uint64_t)(job->y + yy) * job->pitch + job->x;
        for (uint32_t xx = 0; xx < job->width; ++xx) {
            row[xx] = job->color;
        }
    }
}

static void graphics_gradient_job(void *arg) {
    graphics_gradient_job_t *job = (graphics_gradient_job_t *)arg;
    uint32_t steps = job->total_height > 1u ? job->total_height - 1u : 1u;

    for (uint32_t yy = 0; yy < job->height; ++yy) {
        uint32_t absolute_step = job->step_start + yy;
        uint32_t color = graphics_mix_packed_color(job->top_color, job->bottom_color, absolute_step, steps);
        uint32_t *row = job->fb + (uint64_t)(job->y + yy) * job->pitch + job->x;
        for (uint32_t xx = 0; xx < job->width; ++xx) {
            row[xx] = color;
        }
    }
}

static uint32_t *graphics_target_base(void) {
    if (graphics_backbuffer_active_flag && graphics_backbuffer != 0) {
        return graphics_backbuffer;
    }
    return (uint32_t *)(uintptr_t)graphics_fb_base;
}

static void graphics_blit_job(void *arg) {
    graphics_blit_job_t *job = (graphics_blit_job_t *)arg;

    for (uint32_t yy = 0; yy < job->height; ++yy) {
        uint32_t row = job->y + yy;
        uint32_t *dst = job->dst + (uint64_t)row * job->pitch;
        const uint32_t *src = job->src + (uint64_t)row * job->pitch;
        for (uint32_t xx = 0; xx < job->width; ++xx) {
            dst[xx] = src[xx];
        }
    }
}

static uint32_t graphics_clip_rect(uint32_t *x,
                                   uint32_t *y,
                                   uint32_t *width,
                                   uint32_t *height,
                                   uint32_t *abs_x,
                                   uint32_t *abs_y) {
    uint32_t limit_w = graphics_width();
    uint32_t limit_h = graphics_height();
    uint32_t right = *x + *width;
    uint32_t bottom = *y + *height;

    if (*width == 0u || *height == 0u || *x >= limit_w || *y >= limit_h) {
        return 0u;
    }
    if (right < *x || right > limit_w) {
        right = limit_w;
    }
    if (bottom < *y || bottom > limit_h) {
        bottom = limit_h;
    }

    *width = right - *x;
    *height = bottom - *y;
    *abs_x = *x;
    *abs_y = *y;
    if (graphics_viewport_depth > 0u) {
        graphics_viewport_t *viewport = &graphics_viewport_stack[graphics_viewport_depth - 1u];
        *abs_x += viewport->x;
        *abs_y += viewport->y;
    }

    return *abs_x < graphics_fb_width && *abs_y < graphics_fb_height;
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
    graphics_backbuffer = 0;
    graphics_backbuffer_active_flag = 0;
}

int graphics_backbuffer_enable(void) {
    uint64_t bytes;

    if (graphics_backbuffer_active_flag && graphics_backbuffer != 0) {
        return 1;
    }
    if (graphics_fb_base == 0 || graphics_fb_pitch == 0 || graphics_fb_height == 0) {
        return 0;
    }

    bytes = (uint64_t)graphics_fb_pitch * graphics_fb_height * sizeof(uint32_t);
    if (bytes == 0 || bytes > 0xffffffffull) {
        return 0;
    }

    if (graphics_backbuffer == 0) {
        graphics_backbuffer = (uint32_t *)dma_alloc((uint32_t)bytes, 4096u);
        if (graphics_backbuffer == 0) {
            return 0;
        }
    }

    graphics_backbuffer_active_flag = 1u;
    return 1;
}

void graphics_backbuffer_disable(void) {
    graphics_backbuffer_active_flag = 0;
}

int graphics_backbuffer_active(void) {
    return graphics_backbuffer_active_flag && graphics_backbuffer != 0;
}

void graphics_backbuffer_flush(void) {
    uint32_t *fb = (uint32_t *)(uintptr_t)graphics_fb_base;
    unsigned int workers;
    graphics_blit_job_t jobs[GRAPHICS_SMP_MAX_JOBS];
    unsigned int ids[GRAPHICS_SMP_MAX_JOBS];

    if (!graphics_backbuffer_active() || fb == 0) {
        return;
    }

    workers = cpu_online_core_count();
    if (workers > GRAPHICS_SMP_MAX_JOBS) {
        workers = GRAPHICS_SMP_MAX_JOBS;
    }
    if (workers > graphics_fb_height) {
        workers = graphics_fb_height;
    }

    if (workers > 1u) {
        uint32_t base_rows = graphics_fb_height / workers;
        uint32_t extra_rows = graphics_fb_height % workers;
        uint32_t row = 0;

        for (unsigned int i = 0; i < workers; ++i) {
            uint32_t rows = base_rows + (i < extra_rows ? 1u : 0u);
            jobs[i].dst = fb;
            jobs[i].src = graphics_backbuffer;
            jobs[i].y = row;
            jobs[i].width = graphics_fb_width;
            jobs[i].height = rows;
            jobs[i].pitch = graphics_fb_pitch;
            ids[i] = smp_submit_work(graphics_blit_job, &jobs[i]);
            if (ids[i] == 0u) {
                graphics_blit_job(&jobs[i]);
            }
            row += rows;
        }
        for (unsigned int i = 0; i < workers; ++i) {
            smp_wait_work(ids[i]);
        }
        return;
    }

    {
        graphics_blit_job_t job = {
            .dst = fb,
            .src = graphics_backbuffer,
            .y = 0,
            .width = graphics_fb_width,
            .height = graphics_fb_height,
            .pitch = graphics_fb_pitch,
        };
        graphics_blit_job(&job);
    }
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

uint32_t graphics_viewport_active(void) {
    return graphics_viewport_depth > 0u ? 1u : 0u;
}

uint32_t graphics_viewport_x(void) {
    if (graphics_viewport_depth > 0u) {
        return graphics_viewport_stack[graphics_viewport_depth - 1u].x;
    }
    return 0u;
}

uint32_t graphics_viewport_y(void) {
    if (graphics_viewport_depth > 0u) {
        return graphics_viewport_stack[graphics_viewport_depth - 1u].y;
    }
    return 0u;
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
    uint32_t *fb = graphics_target_base();

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
    uint32_t *fb = graphics_target_base();
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
    uint32_t *fb = graphics_target_base();
    uint32_t abs_x = 0;
    uint32_t abs_y = 0;
    uint32_t color;
    uint64_t pixels;
    unsigned int workers;
    graphics_fill_job_t jobs[GRAPHICS_SMP_MAX_JOBS];
    unsigned int ids[GRAPHICS_SMP_MAX_JOBS];

    if (fb == 0 || !graphics_clip_rect(&x, &y, &width, &height, &abs_x, &abs_y)) {
        return;
    }

    color = graphics_pack_color(rgb_color);
    pixels = (uint64_t)width * height;
    workers = cpu_online_core_count();
    if (workers > GRAPHICS_SMP_MAX_JOBS) {
        workers = GRAPHICS_SMP_MAX_JOBS;
    }
    if (workers > height) {
        workers = height;
    }

    if (workers > 1u && pixels >= GRAPHICS_SMP_MIN_PIXELS) {
        uint32_t base_rows = height / workers;
        uint32_t extra_rows = height % workers;
        uint32_t row = 0;

        for (unsigned int i = 0; i < workers; ++i) {
            uint32_t rows = base_rows + (i < extra_rows ? 1u : 0u);
            jobs[i].fb = fb;
            jobs[i].x = abs_x;
            jobs[i].y = abs_y + row;
            jobs[i].width = width;
            jobs[i].height = rows;
            jobs[i].pitch = graphics_fb_pitch;
            jobs[i].color = color;
            ids[i] = smp_submit_work(graphics_fill_job, &jobs[i]);
            if (ids[i] == 0u) {
                graphics_fill_job(&jobs[i]);
            }
            row += rows;
        }
        for (unsigned int i = 0; i < workers; ++i) {
            smp_wait_work(ids[i]);
        }
        return;
    }

    {
        graphics_fill_job_t job = {
            .fb = fb,
            .x = abs_x,
            .y = abs_y,
            .width = width,
            .height = height,
            .pitch = graphics_fb_pitch,
            .color = color,
        };
        graphics_fill_job(&job);
    }
}

void graphics_fill_vertical_gradient(uint32_t x,
                                     uint32_t y,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t top_rgb_color,
                                     uint32_t bottom_rgb_color) {
    uint32_t *fb = graphics_target_base();
    uint32_t abs_x = 0;
    uint32_t abs_y = 0;
    uint32_t top_color;
    uint32_t bottom_color;
    uint64_t pixels;
    unsigned int workers;
    graphics_gradient_job_t jobs[GRAPHICS_SMP_MAX_JOBS];
    unsigned int ids[GRAPHICS_SMP_MAX_JOBS];

    if (fb == 0 || !graphics_clip_rect(&x, &y, &width, &height, &abs_x, &abs_y)) {
        return;
    }

    top_color = graphics_pack_color(top_rgb_color);
    bottom_color = graphics_pack_color(bottom_rgb_color);
    pixels = (uint64_t)width * height;
    workers = cpu_online_core_count();
    if (workers > GRAPHICS_SMP_MAX_JOBS) {
        workers = GRAPHICS_SMP_MAX_JOBS;
    }
    if (workers > height) {
        workers = height;
    }

    if (workers > 1u && pixels >= GRAPHICS_SMP_MIN_PIXELS) {
        uint32_t base_rows = height / workers;
        uint32_t extra_rows = height % workers;
        uint32_t row = 0;

        for (unsigned int i = 0; i < workers; ++i) {
            uint32_t rows = base_rows + (i < extra_rows ? 1u : 0u);
            jobs[i].fb = fb;
            jobs[i].x = abs_x;
            jobs[i].y = abs_y + row;
            jobs[i].width = width;
            jobs[i].height = rows;
            jobs[i].total_height = height;
            jobs[i].step_start = row;
            jobs[i].pitch = graphics_fb_pitch;
            jobs[i].top_color = top_color;
            jobs[i].bottom_color = bottom_color;
            ids[i] = smp_submit_work(graphics_gradient_job, &jobs[i]);
            if (ids[i] == 0u) {
                graphics_gradient_job(&jobs[i]);
            }
            row += rows;
        }
        for (unsigned int i = 0; i < workers; ++i) {
            smp_wait_work(ids[i]);
        }
        return;
    }

    {
        graphics_gradient_job_t job = {
            .fb = fb,
            .x = abs_x,
            .y = abs_y,
            .width = width,
            .height = height,
            .total_height = height,
            .step_start = 0,
            .pitch = graphics_fb_pitch,
            .top_color = top_color,
            .bottom_color = bottom_color,
        };
        graphics_gradient_job(&job);
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
