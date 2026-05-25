#ifndef NETSURF_FRONTEND_H
#define NETSURF_FRONTEND_H

#include <stdint.h>

struct gui_layout_table;
struct plotter_table;
struct redraw_context;
struct bitmap;

typedef struct netsurf_kernel_plot_stats {
    uint32_t clips;
    uint32_t arcs;
    uint32_t discs;
    uint32_t lines;
    uint32_t rectangles;
    uint32_t polygons;
    uint32_t paths;
    uint32_t bitmaps;
    uint32_t texts;
    uint32_t groups_started;
    uint32_t groups_ended;
    uint32_t flushes;
    uint32_t text_bytes;
    uint32_t text_width;
    uint32_t visible_texts;
    uint32_t visible_bitmaps;
    int clip_x0;
    int clip_y0;
    int clip_x1;
    int clip_y1;
} netsurf_kernel_plot_stats_t;

struct gui_layout_table *netsurf_kernel_layout_table(void);
const struct plotter_table *netsurf_kernel_plotter_table(void);
void netsurf_kernel_redraw_context(struct redraw_context *ctx, void *priv);
void netsurf_kernel_plot_stats_reset(void);
void netsurf_kernel_plot_stats_snapshot(netsurf_kernel_plot_stats_t *out);
uint32_t netsurf_kernel_frontend_smoke(void);
const char *netsurf_kernel_frontend_status(void);
int netsurf_kernel_bitmap_width(struct bitmap *bitmap);
int netsurf_kernel_bitmap_height(struct bitmap *bitmap);
int netsurf_kernel_bitmap_rowstride(struct bitmap *bitmap);
int netsurf_kernel_bitmap_opaque(struct bitmap *bitmap);
uint8_t *netsurf_kernel_bitmap_buffer(struct bitmap *bitmap);

#endif
