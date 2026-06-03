#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int nserror;
typedef uint32_t colour;
typedef uint64_t bitmap_flags_t;

#define NSERROR_OK 0
#define TOP 0
#define RIGHT 1
#define BOTTOM 2
#define LEFT 3
#define CSS_BORDER_STYLE_SOLID 5

struct bitmap;
struct rect {
	int x0;
	int y0;
	int x1;
	int y1;
};

typedef struct plot_style_s {
	int stroke_type;
	int stroke_width;
	colour stroke_colour;
	int fill_type;
	colour fill_colour;
} plot_style_t;

typedef struct plot_font_style {
	uint8_t **families;
	int family;
	int size;
	int weight;
	int flags;
	colour background;
	colour foreground;
} plot_font_style_t;

struct redraw_context;
struct plotter_table {
	nserror (*clip)(const struct redraw_context *ctx,
			const struct rect *clip);
	nserror (*arc)(const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			int x, int y, int radius, int angle1, int angle2);
	nserror (*disc)(const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			int x, int y, int radius);
	nserror (*line)(const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			const struct rect *line);
	nserror (*rectangle)(const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			const struct rect *rectangle);
	nserror (*polygon)(const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			const int *p,
			unsigned int n);
	nserror (*path)(const struct redraw_context *ctx,
			const plot_style_t *pstyle,
			const float *p,
			unsigned int n,
			const float transform[6]);
	nserror (*bitmap)(const struct redraw_context *ctx,
			struct bitmap *bitmap,
			int x, int y, int width, int height,
			colour bg,
			bitmap_flags_t flags);
	nserror (*text)(const struct redraw_context *ctx,
			const plot_font_style_t *fstyle,
			int x, int y,
			const char *text,
			size_t length);
	nserror (*group_start)(const struct redraw_context *ctx,
			const char *name);
	nserror (*group_end)(const struct redraw_context *ctx);
	nserror (*flush)(const struct redraw_context *ctx);
	bool option_knockout;
};

struct redraw_context {
	bool interactive;
	bool background_images;
	const struct plotter_table *plot;
	void *priv;
};

struct box_border {
	int style;
	colour c;
	int width;
};

struct box {
	uint8_t pad0[120];
	int x;
	int y;
	int width;
	int height;
	uint8_t pad1[48];
	struct box_border border[4];
};

extern bool html_redraw_borders(struct box *box,
		int x_parent,
		int y_parent,
		int p_width,
		int p_height,
		const struct rect *clip,
		float scale,
		const struct redraw_context *ctx);

static int rect_count;
static int line_count;
static int poly_count;
static colour last_fill;

static nserror tier5_smoke_clip(const struct redraw_context *ctx,
		const struct rect *clip)
{
	(void)ctx;
	(void)clip;
	return NSERROR_OK;
}

static nserror tier5_smoke_arc(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		int x, int y, int radius, int angle1, int angle2)
{
	(void)ctx;
	(void)pstyle;
	(void)x;
	(void)y;
	(void)radius;
	(void)angle1;
	(void)angle2;
	return NSERROR_OK;
}

static nserror tier5_smoke_disc(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		int x, int y, int radius)
{
	(void)ctx;
	(void)pstyle;
	(void)x;
	(void)y;
	(void)radius;
	return NSERROR_OK;
}

static nserror tier5_smoke_line(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		const struct rect *line)
{
	(void)ctx;
	(void)pstyle;
	(void)line;
	line_count++;
	return NSERROR_OK;
}

static nserror tier5_smoke_rectangle(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		const struct rect *rectangle)
{
	(void)ctx;
	(void)rectangle;
	rect_count++;
	last_fill = pstyle->fill_colour;
	return NSERROR_OK;
}

static nserror tier5_smoke_polygon(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		const int *p,
		unsigned int n)
{
	(void)ctx;
	(void)pstyle;
	(void)p;
	(void)n;
	poly_count++;
	return NSERROR_OK;
}

static nserror tier5_smoke_path(const struct redraw_context *ctx,
		const plot_style_t *pstyle,
		const float *p,
		unsigned int n,
		const float transform[6])
{
	(void)ctx;
	(void)pstyle;
	(void)p;
	(void)n;
	(void)transform;
	return NSERROR_OK;
}

static nserror tier5_smoke_bitmap(const struct redraw_context *ctx,
		struct bitmap *bitmap,
		int x, int y, int width, int height,
		colour bg,
		bitmap_flags_t flags)
{
	(void)ctx;
	(void)bitmap;
	(void)x;
	(void)y;
	(void)width;
	(void)height;
	(void)bg;
	(void)flags;
	return NSERROR_OK;
}

static nserror tier5_smoke_text(const struct redraw_context *ctx,
		const plot_font_style_t *fstyle,
		int x, int y,
		const char *text,
		size_t length)
{
	(void)ctx;
	(void)fstyle;
	(void)x;
	(void)y;
	(void)text;
	(void)length;
	return NSERROR_OK;
}

static nserror tier5_smoke_group_start(const struct redraw_context *ctx,
		const char *name)
{
	(void)ctx;
	(void)name;
	return NSERROR_OK;
}

static nserror tier5_smoke_group_end(const struct redraw_context *ctx)
{
	(void)ctx;
	return NSERROR_OK;
}

static nserror tier5_smoke_flush(const struct redraw_context *ctx)
{
	(void)ctx;
	return NSERROR_OK;
}

int tier5_html_redraw_border_smoke(void)
{
	struct box box = {0};
	struct rect clip = {0, 0, 200, 120};
	struct redraw_context ctx = {0};
	struct plotter_table plotters = {0};
	unsigned int side;

	for (side = TOP; side <= LEFT; side++) {
		box.border[side].style = CSS_BORDER_STYLE_SOLID;
		box.border[side].c = 0x334455;
		box.border[side].width = 2;
	}
	box.x = 7;
	box.y = 11;
	box.width = 100;
	box.height = 40;

	plotters.clip = tier5_smoke_clip;
	plotters.arc = tier5_smoke_arc;
	plotters.disc = tier5_smoke_disc;
	plotters.line = tier5_smoke_line;
	plotters.rectangle = tier5_smoke_rectangle;
	plotters.polygon = tier5_smoke_polygon;
	plotters.path = tier5_smoke_path;
	plotters.bitmap = tier5_smoke_bitmap;
	plotters.text = tier5_smoke_text;
	plotters.group_start = tier5_smoke_group_start;
	plotters.group_end = tier5_smoke_group_end;
	plotters.flush = tier5_smoke_flush;

	ctx.background_images = true;
	ctx.plot = &plotters;

	rect_count = 0;
	line_count = 0;
	poly_count = 0;
	last_fill = 0;

	if (!html_redraw_borders(&box, 3, 5, 100, 40, &clip, 1.0f, &ctx)) {
		return 0;
	}
	if (rect_count != 4 || line_count != 0 || poly_count != 0) {
		return 0;
	}
	if (last_fill != 0x334455) {
		return 0;
	}
	return 1;
}
