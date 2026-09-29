// Copyright (C) 2026 Artifex Software, Inc.
//
// This file is part of MuPDF.
//
// MuPDF is free software: you can redistribute it and/or modify it under the
// terms of the GNU Affero General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// MuPDF is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
// details.
//
// You should have received a copy of the GNU Affero General Public License
// along with MuPDF. If not, see <https://www.gnu.org/licenses/agpl-3.0.en.html>
//
// Alternative licensing terms are available from the licensor.
// For commercial licensing, see <https://www.artifex.com/> or contact
// Artifex Software, Inc., 39 Mesa Street, Suite 108A, San Francisco,
// CA 94129, USA, for further information.

/*
	Graphics summary device (fork addition); see graphics-summary.h.

	Every call is first forwarded unchanged to the optional passthrough
	device. Recording happens afterwards and never throws: a failure while
	recording (e.g. out of memory) only stops further recording, so it cannot
	disable or alter the passthrough device's output.

	Tiling patterns: the interpreter runs the pattern cell once between
	begin_tile and end_tile, in pattern space, and the device is expected to
	repeat it over the tiled area. The primitives of the cell are therefore
	not recorded themselves; the first one only supplies the kind, colour and
	shape details of a single record covering the painted area, emitted when
	the outermost tile ends.
*/

#include "mupdf/fitz.h"

#include <string.h>

#define GRID FZ_GRAPHICS_SUMMARY_GRID
#define STRIDE FZ_GRAPHICS_SUMMARY_STRIDE
#define HASH_PREFIX 4096

typedef struct
{
	fz_device super;
	fz_device *passthrough;
	fz_buffer **out;
	fz_rect area;
	int max_records;
	int count;
	int cap;
	float *records;
	int seen[6]; /* indexed by fz_graphics_summary_kind */
	int overflow;
	int in_mask; /* inside a soft-mask definition: not visible graphics */
	int failed; /* recording stopped after an error */
	int pass_skip; /* tile nesting depth inside a tile the passthrough has cached */
	int tile_depth; /* tile nesting depth */
	int tile_mask; /* in_mask when the outermost tile began */
	int tile_first; /* tile_rec holds the first primitive of the tile */
	fz_rect tile_area; /* device-space area painted by the outermost tile */
	float tile_rec[STRIDE];
	float grid[GRID * GRID];
} fz_graphics_summary_device;

/* Path statistics */

typedef struct
{
	int segments;
	int curves;
} path_stats;

static void stats_moveto(fz_context *ctx, void *arg, float x, float y) { ((path_stats *)arg)->segments++; }
static void stats_lineto(fz_context *ctx, void *arg, float x, float y) { ((path_stats *)arg)->segments++; }
static void stats_closepath(fz_context *ctx, void *arg) { }
static void stats_curveto(fz_context *ctx, void *arg, float x1, float y1, float x2, float y2, float x3, float y3)
{
	path_stats *s = arg;
	s->segments++;
	s->curves++;
}

static const fz_path_walker stats_walker = { stats_moveto, stats_lineto, stats_curveto, stats_closepath };

/* Recording */

static float
pack_rgb(fz_context *ctx, fz_colorspace *cs, const float *color, fz_color_params params)
{
	float rgb[3];
	int r, g, b;
	if (!cs || !color)
		return 0;
	fz_convert_color(ctx, cs, color, fz_device_rgb(ctx), rgb, NULL, params);
	r = fz_clampi((int)(rgb[0] * 255 + 0.5f), 0, 255);
	g = fz_clampi((int)(rgb[1] * 255 + 0.5f), 0, 255);
	b = fz_clampi((int)(rgb[2] * 255 + 0.5f), 0, 255);
	return (float)((r << 16) | (g << 8) | b);
}

static float
image_hash(fz_context *ctx, fz_image *image)
{
	/* FNV-1a over the image size and the start of the compressed data. */
	uint32_t h = 2166136261u;
	fz_compressed_buffer *cbuf = fz_compressed_image_buffer(ctx, image);
	size_t i, n = 0, len = 0;
	unsigned char *data = NULL;
	if (cbuf && cbuf->buffer)
		len = fz_buffer_storage(ctx, cbuf->buffer, &data);
	n = len < HASH_PREFIX ? len : HASH_PREFIX;
	for (i = 0; i < n; i++)
		h = (h ^ data[i]) * 16777619u;
	h = (h ^ (uint32_t)len) * 16777619u;
	h = (h ^ (uint32_t)image->w) * 16777619u;
	h = (h ^ (uint32_t)image->h) * 16777619u;
	return (float)(h & 0xffffff);
}

static void
add_to_grid(fz_graphics_summary_device *dev, fz_rect r)
{
	float w = dev->area.x1 - dev->area.x0;
	float h = dev->area.y1 - dev->area.y0;
	int gx, gy;
	if (w <= 0 || h <= 0)
		return;
	gx = fz_clampi((int)(((r.x0 + r.x1) / 2 - dev->area.x0) / w * GRID), 0, GRID - 1);
	gy = fz_clampi((int)(((r.y0 + r.y1) / 2 - dev->area.y0) / h * GRID), 0, GRID - 1);
	dev->grid[gy * GRID + gx] += 1;
}

/* Returns a record to fill in, or NULL when the primitive is not stored.
 * Inside a tile, the first primitive of the cell is captured in tile_rec
 * (without bbox) and nothing is counted; see gs_end_tile. */
static float *
begin_record(fz_context *ctx, fz_graphics_summary_device *dev, fz_graphics_summary_kind kind, fz_rect bbox)
{
	fz_rect clipped;
	float *rec;

	if (dev->tile_depth > 0)
	{
		if (dev->failed || dev->tile_first || dev->in_mask != dev->tile_mask)
			return NULL;
		dev->tile_first = 1;
		memset(dev->tile_rec, 0, sizeof dev->tile_rec);
		dev->tile_rec[0] = (float)kind;
		return dev->tile_rec;
	}

	dev->seen[kind]++;
	if (dev->failed || dev->in_mask)
		return NULL;
	clipped = fz_intersect_rect(bbox, fz_device_current_scissor(ctx, &dev->super));
	clipped = fz_intersect_rect(clipped, dev->area);
	if (!fz_is_valid_rect(clipped))
		return NULL;
	if (dev->count >= dev->max_records)
	{
		dev->overflow = 1;
		add_to_grid(dev, clipped);
		return NULL;
	}
	if (dev->count == dev->cap)
	{
		int cap = dev->cap ? dev->cap * 2 : 256;
		if (cap > dev->max_records)
			cap = dev->max_records;
		dev->records = fz_realloc_array(ctx, dev->records, (size_t)cap * STRIDE, float);
		dev->cap = cap;
	}
	rec = dev->records + (size_t)dev->count++ * STRIDE;
	memset(rec, 0, STRIDE * sizeof(float));
	rec[0] = (float)kind;
	rec[1] = clipped.x0;
	rec[2] = clipped.y0;
	rec[3] = clipped.x1;
	rec[4] = clipped.y1;
	if (clipped.x0 != bbox.x0 || clipped.y0 != bbox.y0 || clipped.x1 != bbox.x1 || clipped.y1 != bbox.y1)
		rec[5] = FZ_GRAPHICS_SUMMARY_CLIPPED;
	return rec;
}

static void
record_path(fz_context *ctx, fz_graphics_summary_device *dev, const fz_path *path, const fz_stroke_state *stroke, int even_odd,
	fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
	fz_graphics_summary_kind kind = stroke ? FZ_GRAPHICS_SUMMARY_STROKE_PATH : FZ_GRAPHICS_SUMMARY_FILL_PATH;
	fz_try(ctx)
	{
		float *rec = begin_record(ctx, dev, kind, fz_bound_path(ctx, path, stroke, ctm));
		if (rec)
		{
			path_stats stats = { 0, 0 };
			int flags = (int)rec[5];
			fz_walk_path(ctx, path, &stats_walker, &stats);
			if (stats.curves)
				flags |= FZ_GRAPHICS_SUMMARY_HAS_CURVE;
			else if (fz_path_is_rect(ctx, path, ctm))
				flags |= FZ_GRAPHICS_SUMMARY_IS_RECT;
			if (!stroke && even_odd)
				flags |= FZ_GRAPHICS_SUMMARY_EVEN_ODD;
			rec[5] = (float)flags;
			rec[6] = pack_rgb(ctx, cs, color, params);
			rec[7] = fz_clamp(alpha, 0, 1) * 255;
			rec[8] = (float)stats.segments;
			if (stroke)
				rec[9] = stroke->linewidth * fz_matrix_expansion(ctm);
		}
	}
	fz_catch(ctx)
	{
		fz_report_error(ctx);
		dev->failed = 1;
	}
}

static void
record_image(fz_context *ctx, fz_graphics_summary_device *dev, fz_graphics_summary_kind kind, fz_image *image, fz_matrix ctm,
	fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
	fz_try(ctx)
	{
		float *rec = begin_record(ctx, dev, kind, fz_transform_rect(fz_unit_rect, ctm));
		if (rec)
		{
			if (kind == FZ_GRAPHICS_SUMMARY_IMAGE_MASK)
				rec[6] = pack_rgb(ctx, cs, color, params);
			rec[7] = fz_clamp(alpha, 0, 1) * 255;
			rec[9] = (float)image->w;
			rec[10] = (float)image->h;
			rec[11] = image_hash(ctx, image);
		}
	}
	fz_catch(ctx)
	{
		fz_report_error(ctx);
		dev->failed = 1;
	}
}

/* Device callbacks: forward first, then record. */

#define GS(d) ((fz_graphics_summary_device *)(d))
/* NULL inside a tile whose content the passthrough has declined (cached). */
#define PASS(d) (GS(d)->pass_skip ? NULL : GS(d)->passthrough)

static void
gs_fill_path(fz_context *ctx, fz_device *d, const fz_path *path, int even_odd, fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
	if (PASS(d))
		fz_fill_path(ctx, PASS(d), path, even_odd, ctm, cs, color, alpha, params);
	record_path(ctx, GS(d), path, NULL, even_odd, ctm, cs, color, alpha, params);
}

static void
gs_stroke_path(fz_context *ctx, fz_device *d, const fz_path *path, const fz_stroke_state *stroke, fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
	if (PASS(d))
		fz_stroke_path(ctx, PASS(d), path, stroke, ctm, cs, color, alpha, params);
	record_path(ctx, GS(d), path, stroke, 0, ctm, cs, color, alpha, params);
}

static void
gs_clip_path(fz_context *ctx, fz_device *d, const fz_path *path, int even_odd, fz_matrix ctm, fz_rect scissor)
{
	if (PASS(d))
		fz_clip_path(ctx, PASS(d), path, even_odd, ctm, scissor);
}

static void
gs_clip_stroke_path(fz_context *ctx, fz_device *d, const fz_path *path, const fz_stroke_state *stroke, fz_matrix ctm, fz_rect scissor)
{
	if (PASS(d))
		fz_clip_stroke_path(ctx, PASS(d), path, stroke, ctm, scissor);
}

static void
gs_fill_text(fz_context *ctx, fz_device *d, const fz_text *text, fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
	if (PASS(d))
		fz_fill_text(ctx, PASS(d), text, ctm, cs, color, alpha, params);
}

static void
gs_stroke_text(fz_context *ctx, fz_device *d, const fz_text *text, const fz_stroke_state *stroke, fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
	if (PASS(d))
		fz_stroke_text(ctx, PASS(d), text, stroke, ctm, cs, color, alpha, params);
}

static void
gs_clip_text(fz_context *ctx, fz_device *d, const fz_text *text, fz_matrix ctm, fz_rect scissor)
{
	if (PASS(d))
		fz_clip_text(ctx, PASS(d), text, ctm, scissor);
}

static void
gs_clip_stroke_text(fz_context *ctx, fz_device *d, const fz_text *text, const fz_stroke_state *stroke, fz_matrix ctm, fz_rect scissor)
{
	if (PASS(d))
		fz_clip_stroke_text(ctx, PASS(d), text, stroke, ctm, scissor);
}

static void
gs_ignore_text(fz_context *ctx, fz_device *d, const fz_text *text, fz_matrix ctm)
{
	if (PASS(d))
		fz_ignore_text(ctx, PASS(d), text, ctm);
}

static void
gs_fill_shade(fz_context *ctx, fz_device *d, fz_shade *shade, fz_matrix ctm, float alpha, fz_color_params params)
{
	if (PASS(d))
		fz_fill_shade(ctx, PASS(d), shade, ctm, alpha, params);
	fz_try(ctx)
	{
		float *rec = begin_record(ctx, GS(d), FZ_GRAPHICS_SUMMARY_SHADE, fz_bound_shade(ctx, shade, ctm));
		if (rec)
			rec[7] = fz_clamp(alpha, 0, 1) * 255;
	}
	fz_catch(ctx)
	{
		fz_report_error(ctx);
		GS(d)->failed = 1;
	}
}

static void
gs_fill_image(fz_context *ctx, fz_device *d, fz_image *image, fz_matrix ctm, float alpha, fz_color_params params)
{
	if (PASS(d))
		fz_fill_image(ctx, PASS(d), image, ctm, alpha, params);
	record_image(ctx, GS(d), FZ_GRAPHICS_SUMMARY_IMAGE, image, ctm, NULL, NULL, alpha, params);
}

static void
gs_fill_image_mask(fz_context *ctx, fz_device *d, fz_image *image, fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params params)
{
	if (PASS(d))
		fz_fill_image_mask(ctx, PASS(d), image, ctm, cs, color, alpha, params);
	record_image(ctx, GS(d), FZ_GRAPHICS_SUMMARY_IMAGE_MASK, image, ctm, cs, color, alpha, params);
}

static void
gs_clip_image_mask(fz_context *ctx, fz_device *d, fz_image *image, fz_matrix ctm, fz_rect scissor)
{
	if (PASS(d))
		fz_clip_image_mask(ctx, PASS(d), image, ctm, scissor);
}

static void
gs_pop_clip(fz_context *ctx, fz_device *d)
{
	if (PASS(d))
		fz_pop_clip(ctx, PASS(d));
}

static void
gs_begin_mask(fz_context *ctx, fz_device *d, fz_rect area, int luminosity, fz_colorspace *cs, const float *bc, fz_color_params params)
{
	if (PASS(d))
		fz_begin_mask(ctx, PASS(d), area, luminosity, cs, bc, params);
	GS(d)->in_mask++;
}

static void
gs_end_mask(fz_context *ctx, fz_device *d, fz_function *fn)
{
	if (PASS(d))
		fz_end_mask_tr(ctx, PASS(d), fn);
	if (GS(d)->in_mask > 0)
		GS(d)->in_mask--;
}

static void
gs_begin_group(fz_context *ctx, fz_device *d, fz_rect area, fz_colorspace *cs, int isolated, int knockout, int blendmode, float alpha)
{
	if (PASS(d))
		fz_begin_group(ctx, PASS(d), area, cs, isolated, knockout, blendmode, alpha);
}

static void
gs_end_group(fz_context *ctx, fz_device *d)
{
	if (PASS(d))
		fz_end_group(ctx, PASS(d));
}

static int
gs_begin_tile(fz_context *ctx, fz_device *d, fz_rect area, fz_rect view, float xstep, float ystep, fz_matrix ctm, int id, int doc_id)
{
	fz_graphics_summary_device *dev = GS(d);
	int cached = 0;

	if (dev->pass_skip)
		dev->pass_skip++;
	else if (dev->passthrough)
		cached = fz_begin_tile_tid(ctx, dev->passthrough, area, view, xstep, ystep, ctm, id, doc_id);

	if (dev->tile_depth++ == 0)
	{
		/* area is in pattern space; the cell repeats over all of it. */
		dev->tile_area = fz_transform_rect(area, ctm);
		dev->tile_mask = dev->in_mask;
		dev->tile_first = 0;
	}

	if (!cached)
		return 0;
	/* The passthrough has the tile cached. If the summary has no use for
	 * the cell either, let the interpreter skip it; otherwise have it run
	 * the cell and withhold it from the passthrough until the tile ends. */
	if (dev->failed || dev->tile_first)
		return 1;
	dev->pass_skip = 1;
	return 0;
}

static void
gs_end_tile(fz_context *ctx, fz_device *d)
{
	fz_graphics_summary_device *dev = GS(d);

	if (dev->pass_skip)
	{
		if (--dev->pass_skip == 0)
			fz_end_tile(ctx, dev->passthrough);
	}
	else if (dev->passthrough)
		fz_end_tile(ctx, dev->passthrough);

	if (dev->tile_depth == 0 || --dev->tile_depth > 0 || !dev->tile_first)
		return;

	/* One record for everything the outermost tile painted, clipped to
	 * the clip it was painted through (the tile itself is already popped
	 * from the device's clip stack). The cell's first primitive supplies
	 * the other fields, except that the record is never a single
	 * rectangle. */
	fz_try(ctx)
	{
		float *rec = begin_record(ctx, dev, (fz_graphics_summary_kind)dev->tile_rec[0], dev->tile_area);
		if (rec)
		{
			int flags = (int)rec[5];
			memcpy(rec + 5, dev->tile_rec + 5, (STRIDE - 5) * sizeof(float));
			flags |= ((int)dev->tile_rec[5] & ~(FZ_GRAPHICS_SUMMARY_IS_RECT | FZ_GRAPHICS_SUMMARY_CLIPPED)) | FZ_GRAPHICS_SUMMARY_TILED;
			rec[5] = (float)flags;
		}
	}
	fz_catch(ctx)
	{
		fz_report_error(ctx);
		dev->failed = 1;
	}
}

static void
gs_render_flags(fz_context *ctx, fz_device *d, int set, int clear)
{
	if (PASS(d))
		fz_render_flags(ctx, PASS(d), set, clear);
}

static void
gs_set_default_colorspaces(fz_context *ctx, fz_device *d, fz_default_colorspaces *cs)
{
	if (PASS(d))
		fz_set_default_colorspaces(ctx, PASS(d), cs);
}

static void
gs_begin_layer(fz_context *ctx, fz_device *d, const char *name)
{
	if (PASS(d))
		fz_begin_layer(ctx, PASS(d), name);
}

static void
gs_end_layer(fz_context *ctx, fz_device *d)
{
	if (PASS(d))
		fz_end_layer(ctx, PASS(d));
}

static void
gs_begin_structure(fz_context *ctx, fz_device *d, fz_structure standard, const char *raw, int idx)
{
	if (PASS(d))
		fz_begin_structure(ctx, PASS(d), standard, raw, idx);
}

static void
gs_end_structure(fz_context *ctx, fz_device *d)
{
	if (PASS(d))
		fz_end_structure(ctx, PASS(d));
}

static void
gs_begin_metatext(fz_context *ctx, fz_device *d, fz_metatext meta, const char *text)
{
	if (PASS(d))
		fz_begin_metatext(ctx, PASS(d), meta, text);
}

static void
gs_end_metatext(fz_context *ctx, fz_device *d)
{
	if (PASS(d))
		fz_end_metatext(ctx, PASS(d));
}

static void
gs_close_device(fz_context *ctx, fz_device *d)
{
	fz_graphics_summary_device *dev = GS(d);
	float header[FZ_GRAPHICS_SUMMARY_HEADER] = { 0 };
	fz_buffer *buf;

	if (dev->passthrough)
		fz_close_device(ctx, dev->passthrough);

	header[0] = FZ_GRAPHICS_SUMMARY_VERSION;
	header[1] = (float)dev->count;
	header[2] = STRIDE;
	header[3] = (float)dev->seen[FZ_GRAPHICS_SUMMARY_FILL_PATH];
	header[4] = (float)dev->seen[FZ_GRAPHICS_SUMMARY_STROKE_PATH];
	header[5] = (float)dev->seen[FZ_GRAPHICS_SUMMARY_IMAGE];
	header[6] = (float)dev->seen[FZ_GRAPHICS_SUMMARY_IMAGE_MASK];
	header[7] = (float)dev->seen[FZ_GRAPHICS_SUMMARY_SHADE];
	header[8] = (float)dev->overflow;
	header[9] = dev->overflow ? GRID : 0;
	header[10] = dev->area.x0;
	header[11] = dev->area.y0;
	header[12] = dev->area.x1;
	header[13] = dev->area.y1;

	buf = fz_new_buffer(ctx, sizeof header + (size_t)dev->count * STRIDE * sizeof(float) + (dev->overflow ? sizeof dev->grid : 0));
	fz_try(ctx)
	{
		fz_append_data(ctx, buf, header, sizeof header);
		if (dev->count)
			fz_append_data(ctx, buf, dev->records, (size_t)dev->count * STRIDE * sizeof(float));
		if (dev->overflow)
			fz_append_data(ctx, buf, dev->grid, sizeof dev->grid);
	}
	fz_catch(ctx)
	{
		fz_drop_buffer(ctx, buf);
		fz_rethrow(ctx);
	}
	if (dev->out)
	{
		fz_drop_buffer(ctx, *dev->out);
		*dev->out = buf;
	}
	else
		fz_drop_buffer(ctx, buf);
}

static void
gs_drop_device(fz_context *ctx, fz_device *d)
{
	fz_free(ctx, GS(d)->records);
}

fz_device *
fz_new_graphics_summary_device(fz_context *ctx, fz_rect area, int max_records, fz_device *passthrough, fz_buffer **out)
{
	fz_graphics_summary_device *dev = fz_new_derived_device(ctx, fz_graphics_summary_device);

	dev->super.close_device = gs_close_device;
	dev->super.drop_device = gs_drop_device;
	dev->super.fill_path = gs_fill_path;
	dev->super.stroke_path = gs_stroke_path;
	dev->super.clip_path = gs_clip_path;
	dev->super.clip_stroke_path = gs_clip_stroke_path;
	dev->super.fill_text = gs_fill_text;
	dev->super.stroke_text = gs_stroke_text;
	dev->super.clip_text = gs_clip_text;
	dev->super.clip_stroke_text = gs_clip_stroke_text;
	dev->super.ignore_text = gs_ignore_text;
	dev->super.fill_shade = gs_fill_shade;
	dev->super.fill_image = gs_fill_image;
	dev->super.fill_image_mask = gs_fill_image_mask;
	dev->super.clip_image_mask = gs_clip_image_mask;
	dev->super.pop_clip = gs_pop_clip;
	dev->super.begin_mask = gs_begin_mask;
	dev->super.end_mask = gs_end_mask;
	dev->super.begin_group = gs_begin_group;
	dev->super.end_group = gs_end_group;
	dev->super.begin_tile = gs_begin_tile;
	dev->super.end_tile = gs_end_tile;
	dev->super.render_flags = gs_render_flags;
	dev->super.set_default_colorspaces = gs_set_default_colorspaces;
	dev->super.begin_layer = gs_begin_layer;
	dev->super.end_layer = gs_end_layer;
	dev->super.begin_structure = gs_begin_structure;
	dev->super.end_structure = gs_end_structure;
	dev->super.begin_metatext = gs_begin_metatext;
	dev->super.end_metatext = gs_end_metatext;

	/* The interpreter consults the outermost device's hints and flags.
	 * FZ_DONT_DECODE_IMAGES (set by the structured-text device when it
	 * neither keeps images nor tracks ActualText bounds) makes the PDF
	 * interpreter skip image XObjects altogether, so the summary must not
	 * inherit it. Loading an image only reads its compressed data; pixels
	 * are decoded on demand, which neither device asks for. */
	if (passthrough)
	{
		dev->super.hints = passthrough->hints & ~FZ_DONT_DECODE_IMAGES;
		dev->super.flags = passthrough->flags;
	}
	dev->passthrough = passthrough;
	dev->out = out;
	dev->area = area;
	dev->max_records = max_records > 0 ? max_records : FZ_GRAPHICS_SUMMARY_DEFAULT_MAX;
	return &dev->super;
}

fz_buffer *
fz_new_graphics_summary_from_page(fz_context *ctx, fz_page *page, int max_records)
{
	fz_buffer *buf = NULL;
	fz_device *dev = NULL;

	fz_var(buf);
	fz_var(dev);

	fz_try(ctx)
	{
		dev = fz_new_graphics_summary_device(ctx, fz_bound_page(ctx, page), max_records, NULL, &buf);
		fz_run_page_contents(ctx, page, dev, fz_identity, NULL);
		fz_close_device(ctx, dev);
	}
	fz_always(ctx)
		fz_drop_device(ctx, dev);
	fz_catch(ctx)
	{
		fz_drop_buffer(ctx, buf);
		fz_rethrow(ctx);
	}
	return buf;
}

fz_stext_page *
fz_new_stext_page_with_graphics_summary(fz_context *ctx, fz_page *page, const fz_stext_options *options, int max_records, fz_buffer **summary)
{
	fz_stext_page *text;
	fz_device *stext = NULL;
	fz_device *dev = NULL;
	fz_buffer *buf = NULL;
	fz_rect bounds;

	fz_var(stext);
	fz_var(dev);
	fz_var(buf);

	if (page == NULL)
		return NULL;

	/* Same page bounds, device and run call as fz_new_stext_page_from_page. */
	bounds = fz_bound_page(ctx, page);
	text = fz_new_stext_page(ctx, bounds);
	fz_try(ctx)
	{
		stext = fz_new_stext_device(ctx, text, options);
		dev = fz_new_graphics_summary_device(ctx, bounds, max_records, stext, &buf);
		fz_run_page_contents(ctx, page, dev, fz_identity, NULL);
		fz_close_device(ctx, dev); /* closes the stext device too */
	}
	fz_always(ctx)
	{
		fz_drop_device(ctx, dev);
		fz_drop_device(ctx, stext);
	}
	fz_catch(ctx)
	{
		fz_drop_buffer(ctx, buf);
		fz_drop_stext_page(ctx, text);
		fz_rethrow(ctx);
	}
	*summary = buf;
	return text;
}
