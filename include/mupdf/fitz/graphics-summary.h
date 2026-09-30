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

#ifndef MUPDF_FITZ_GRAPHICS_SUMMARY_H
#define MUPDF_FITZ_GRAPHICS_SUMMARY_H

#include "mupdf/fitz/system.h"
#include "mupdf/fitz/context.h"
#include "mupdf/fitz/geometry.h"
#include "mupdf/fitz/buffer.h"
#include "mupdf/fitz/device.h"
#include "mupdf/fitz/document.h"
#include "mupdf/fitz/structured-text.h"

/**
	Graphics summary (fork addition).

	A compact description of the drawing primitives on a page — filled and
	stroked paths, images, image masks and shadings — for layout analysis
	(finding figures, tables and rules) without calling back into a host
	language for every primitive.

	The summary is a buffer of little-endian 32-bit floats:

	Header (FZ_GRAPHICS_SUMMARY_HEADER floats):
		[0]  format version (FZ_GRAPHICS_SUMMARY_VERSION)
		[1]  number of records stored
		[2]  floats per record (FZ_GRAPHICS_SUMMARY_STRIDE)
		[3]  fill paths seen    [4] stroke paths seen
		[5]  images seen        [6] image masks seen   [7] shadings seen
		     (counts include primitives that were clipped away or only
		     added to the overflow grid, but not the contents of
		     soft-mask definitions; a tiling-pattern fill counts once,
		     as its tiled record)
		[8]  1 if the record cap was reached, else 0
		[9]  overflow grid size N (N x N cells over the area), 0 if none
		[10..13] area x0, y0, x1, y1 (the page bounds)
		[14] 1 if the summary is incomplete, else 0: recording stopped
		     at an error (for example out of memory or a failed colour
		     conversion), or an error in the passthrough device that
		     the interpreter continued past disabled both devices;
		     the records end at that point
		[15] reserved (0)

	Soft-mask definitions are not visible graphics: primitives drawn
	between begin_mask and end_mask are neither recorded nor counted.

	Records (FZ_GRAPHICS_SUMMARY_STRIDE floats each), in drawing order:
		[0]  kind (fz_graphics_summary_kind)
		[1..4] bbox x0, y0, x1, y1 in device space, intersected with the
		     current clip and the area; primitives clipped away entirely
		     are not stored
		[5]  flags (fz_graphics_summary_flags)
		[6]  colour as 0xRRGGBB (exact in a float); paths and image masks
		     only, 0 otherwise
		[7]  alpha, 0-255
		[8]  number of path segments (paths), 0 otherwise
		[9]  stroke width in device space (stroked paths); image width in
		     pixels (images and image masks)
		[10] image height in pixels (images and image masks)
		[11] 24-bit hash of the compressed image data (images and image
		     masks), for recognising the same image drawn on several pages

	Tiling patterns: when a pattern fill is run as a tile (the PDF
	interpreter does so when the fill spans more than one cell in either
	direction; smaller fills draw the cell contents directly, recorded as
	usual), the primitives of the cell are not recorded individually.
	The whole tiled fill becomes one record flagged
	FZ_GRAPHICS_SUMMARY_TILED whose bbox is the painted area (the fill's
	area in device space, intersected with the clip and the area) and
	whose kind and fields [6..11] are those of the first primitive the
	cell draws (flags other than has-curve and even-odd are not copied).
	A cell that draws nothing gives no record; a pattern nested in a
	pattern cell is part of the outer record.

	Overflow grid (only when the cap was reached): N x N floats, row-major,
	counting the primitives beyond the cap whose bbox centre falls in each
	cell of the area.
*/
enum
{
	FZ_GRAPHICS_SUMMARY_VERSION = 1,
	FZ_GRAPHICS_SUMMARY_HEADER = 16,
	FZ_GRAPHICS_SUMMARY_STRIDE = 12,
	FZ_GRAPHICS_SUMMARY_GRID = 32
};

typedef enum
{
	FZ_GRAPHICS_SUMMARY_FILL_PATH = 1,
	FZ_GRAPHICS_SUMMARY_STROKE_PATH = 2,
	FZ_GRAPHICS_SUMMARY_IMAGE = 3,
	FZ_GRAPHICS_SUMMARY_IMAGE_MASK = 4,
	FZ_GRAPHICS_SUMMARY_SHADE = 5
} fz_graphics_summary_kind;

typedef enum
{
	FZ_GRAPHICS_SUMMARY_IS_RECT = 1, /* the path is a single axis-aligned rectangle */
	FZ_GRAPHICS_SUMMARY_HAS_CURVE = 2, /* the path contains curve segments */
	FZ_GRAPHICS_SUMMARY_EVEN_ODD = 4, /* even-odd fill rule */
	FZ_GRAPHICS_SUMMARY_CLIPPED = 8, /* the bbox was reduced by the current clip or the area */
	FZ_GRAPHICS_SUMMARY_TILED = 16 /* a tiling-pattern fill: the bbox is the painted area, not one cell */
} fz_graphics_summary_flags;

/**
	Create a device that records a graphics summary.

	area: records are clipped to it and it bounds the overflow grid
	(normally the page bounds).

	max_records: records kept before counting further primitives only in
	the overflow grid; 0 or less means FZ_GRAPHICS_SUMMARY_DEFAULT_MAX.

	passthrough: if not NULL, every device call is forwarded unchanged to
	this device, so a single interpretation of the page feeds both (for
	example a structured-text device). Closing the summary device closes
	the passthrough device; dropping it does not drop the passthrough.

	out: receives the summary buffer when the device is closed (the caller
	owns it). Nothing is written if the device is dropped unclosed.
*/
fz_device *fz_new_graphics_summary_device(fz_context *ctx, fz_rect area, int max_records, fz_device *passthrough, fz_buffer **out);

#define FZ_GRAPHICS_SUMMARY_DEFAULT_MAX 4000

/**
	Summarise the graphics in a page's content stream (annotations and
	widgets are not included).
*/
fz_buffer *fz_new_graphics_summary_from_page(fz_context *ctx, fz_page *page, int max_records);

/**
	Build a structured-text page exactly as fz_new_stext_page_from_page
	does and, from the same interpretation of the page contents, its
	graphics summary (returned in *summary, owned by the caller).

	An error in the structured-text device disables it for the rest of
	the page. If the interpreter continues past the error (the PDF
	interpreter does so for syntax and try-later errors), the call
	returns the structured text exactly as fz_new_stext_page_from_page
	does, and the summary holds what was recorded before the error,
	flagged incomplete (header[14]). Any other error (out of memory,
	format, argument, ...) aborts the page and the call throws, as
	fz_new_stext_page_from_page does.

	*summary is set whenever the function returns: to NULL with a NULL
	page (the function then returns NULL), otherwise to the summary.
*/
fz_stext_page *fz_new_stext_page_with_graphics_summary(fz_context *ctx, fz_page *page, const fz_stext_options *options, int max_records, fz_buffer **summary);

#endif
