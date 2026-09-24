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

/*
	Known glyph outlines.

	Many embedded symbol fonts have no ToUnicode CMap and name their glyphs
	after the Latin slot they occupy rather than the symbol they draw (Elsevier
	"Advent" fonts draw mu in the slot named "m", MathPi fonts draw a minus in
	"C0", TeX math italic draws a period in the "colon" slot). The mapping from
	character code to glyph name to unicode is valid at every step, so nothing
	downstream can tell the text is wrong.

	The table in known-glyph-outlines-table.h maps the hash of such outlines to
	the character they actually draw. It is generated offline from a PDF corpus
	and reviewed by hand; do not edit it directly.

	Outline hash (version 1). Load the glyph with
	FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_IGNORE_TRANSFORM and run
	64-bit FNV-1a over these values, each as a little-endian int32:

		n_contours, contours[0 .. n_contours-1],
		n_points, then x, y, (tags & 3) for every point.

	Glyphs with no points are never hashed. The table generator must use the
	same definition.
*/

#include "mupdf/fitz.h"
#include "mupdf/ucdn.h"

#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct
{
	uint64_t hash;
	int ucs;
} known_glyph_outline;

#include "known-glyph-outlines-table.h"

static uint64_t
fnv1a_int32(uint64_t h, int32_t v)
{
	uint32_t u = (uint32_t)v;
	int i;
	for (i = 0; i < 4; i++)
	{
		h ^= (u >> (8 * i)) & 0xff;
		h *= 0x100000001b3ULL;
	}
	return h;
}

static uint64_t
outline_hash(const FT_Outline *outline)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	int i;

	h = fnv1a_int32(h, outline->n_contours);
	for (i = 0; i < outline->n_contours; i++)
		h = fnv1a_int32(h, outline->contours[i]);
	h = fnv1a_int32(h, outline->n_points);
	for (i = 0; i < outline->n_points; i++)
	{
		h = fnv1a_int32(h, (int32_t)outline->points[i].x);
		h = fnv1a_int32(h, (int32_t)outline->points[i].y);
		h = fnv1a_int32(h, outline->tags[i] & 3);
	}
	return h;
}

static int
lookup_outline(uint64_t hash)
{
	int l = 0;
	int r = (int)nelem(fz_known_glyph_outlines) - 1;
	while (l <= r)
	{
		int m = (l + r) >> 1;
		if (hash < fz_known_glyph_outlines[m].hash)
			r = m - 1;
		else if (hash > fz_known_glyph_outlines[m].hash)
			l = m + 1;
		else
			return fz_known_glyph_outlines[m].ucs;
	}
	return 0;
}

int
fz_known_glyph_outline_unicode(fz_context *ctx, fz_font *font, int gid)
{
	FT_Face face;
	int ucs;

	if (!font || !font->ft_face || gid < 0 || gid >= font->glyph_count)
		return 0;
	face = font->ft_face;

	fz_ft_lock(ctx);
	if (!font->known_outline_ucs)
	{
		fz_try(ctx)
			font->known_outline_ucs = Memento_label(fz_calloc(ctx, font->glyph_count, sizeof(int)), "font_known_outline_ucs");
		fz_catch(ctx)
		{
			fz_ft_unlock(ctx);
			fz_rethrow(ctx);
		}
	}

	/* 0 = not yet computed, -1 = not in the table. */
	ucs = font->known_outline_ucs[gid];
	if (ucs == 0)
	{
		ucs = -1;
		if (!FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_IGNORE_TRANSFORM) &&
			face->glyph->format == FT_GLYPH_FORMAT_OUTLINE &&
			face->glyph->outline.n_points > 0)
		{
			int found = lookup_outline(outline_hash(&face->glyph->outline));
			if (found > 0)
				ucs = found;
		}
		font->known_outline_ucs[gid] = ucs;
	}
	fz_ft_unlock(ctx);

	return ucs > 0 ? ucs : 0;
}

static int
is_letter(int c)
{
	switch (ucdn_get_general_category(c))
	{
	case UCDN_GENERAL_CATEGORY_LL:
	case UCDN_GENERAL_CATEGORY_LM:
	case UCDN_GENERAL_CATEGORY_LO:
	case UCDN_GENERAL_CATEGORY_LT:
	case UCDN_GENERAL_CATEGORY_LU:
		return 1;
	}
	return 0;
}

/* A value no correct ToUnicode entry produces for a drawn glyph. */
static int
is_garbage_unicode(int c)
{
	return c == FZ_REPLACEMENT_CHARACTER || c < 32 || (c >= 127 && c < 160);
}

/* Latin-1 values that broken ToUnicode maps substitute for symbols. */
static int
is_suspicious_latin1(int c)
{
	if (c < 160 || c > 255)
		return 0;
	return c == 0xBC || c == 0xBD || c == 0xBE || is_letter(c);
}

static int
is_ascii_alnum(int c)
{
	return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int
is_symbol(int c)
{
	switch (ucdn_get_general_category(c))
	{
	case UCDN_GENERAL_CATEGORY_SC:
	case UCDN_GENERAL_CATEGORY_SK:
	case UCDN_GENERAL_CATEGORY_SM:
	case UCDN_GENERAL_CATEGORY_SO:
		return 1;
	}
	return 0;
}

/* Greek letters that do not look like a Latin letter: a ToUnicode that maps
 * such a drawing to an ASCII letter follows the Symbol font layout ("m" for
 * mu, "D" for Delta) and is wrong. Greek letters that can be drawn like a
 * Latin letter are excluded, since the ASCII letter could be right: capitals
 * such as Alpha or Rho, and lowercase iota, kappa, nu, omicron, rho, upsilon,
 * chi and omega, plus their symbol-form variants. */
static int
is_distinct_greek(int c)
{
	switch (c)
	{
	case 0x391: case 0x392: case 0x395: case 0x396: case 0x397: case 0x399: /* Α Β Ε Ζ Η Ι */
	case 0x39A: case 0x39C: case 0x39D: case 0x39F: case 0x3A1: case 0x3A4: /* Κ Μ Ν Ο Ρ Τ */
	case 0x3A5: case 0x3A7: /* Υ Χ */
	case 0x3B9: case 0x3BA: case 0x3BD: case 0x3BF: case 0x3C1: case 0x3C5: /* ι κ ν ο ρ υ */
	case 0x3C7: case 0x3C9: /* χ ω */
	case 0x3D2: case 0x3D3: case 0x3D4: /* ϒ ϓ ϔ (upsilon with hook) */
	case 0x3F0: case 0x3F1: case 0x3F2: case 0x3F3: /* ϰ ϱ ϲ ϳ */
		return 0;
	}
	if (c == 0xB5) /* micro sign: the table stores mu as U+03BC, but accept both */
		return 1;
	return (c >= 0x391 && c <= 0x3A9) || (c >= 0x3B1 && c <= 0x3C9) || (c >= 0x3D0 && c <= 0x3F5);
}

/* Spacing accents: text fonts draw them as separate glyphs over a letter
 * ("Ame´rica"), where they can still be recombined into a letter. */
static int
is_spacing_accent(int c)
{
	return c == 0xA8 || c == 0xAF || c == 0xB4 || c == 0xB8 || /* ¨ ¯ ´ ¸ */
		(c >= 0x2C6 && c <= 0x2DD); /* ˆ ˇ ˉ … ˘ ˙ ˚ ˛ ˜ ˝ */
}

/* Characters a spacing accent can be drawn identically to: primes, degree
 * and ring, quotes, tildes and dots. An accent is never overridden with one
 * of these; a symbol font that puts a real symbol in an accent slot (an
 * element-of in "ogonek") is still repaired. */
static int
is_accent_lookalike(int c)
{
	switch (c)
	{
	case 0x2032: case 0x2033: case 0x2034: case 0x2035: /* ′ ″ ‴ ‵ */
	case 0xB0: case 0x2DA: /* ° ˚ */
	case '\'': case '`': case 0x2018: case 0x2019: case 0x201C: case 0x201D: /* quotes */
	case '~': case 0x223C: case 0x2DC: /* tildes */
	case 0xB7: case 0x22C5: case 0x2D9: /* dots */
		return 1;
	}
	return 0;
}

int
fz_known_glyph_outline_override(fz_context *ctx, fz_font *font, int gid, int current)
{
	int known;

	if (!font || gid < 0)
		return current;
	if (!font->flags.unicode_from_glyph_names && !font->flags.unicode_from_tounicode)
		return current;
	if (font->flags.unicode_from_tounicode &&
		!is_garbage_unicode(current) && !is_suspicious_latin1(current) && !is_ascii_alnum(current))
		return current;

	known = fz_known_glyph_outline_unicode(ctx, font, gid);
	if (!known || known == current)
		return current;
	if (is_spacing_accent(current) && is_accent_lookalike(known))
		return current;

	if (font->flags.unicode_from_glyph_names || is_garbage_unicode(current))
		return known;
	/* ToUnicode gave a Latin-1 letter or fraction: override only with a non-letter. */
	if (is_suspicious_latin1(current))
		return is_letter(known) ? current : known;
	/* ToUnicode gave an ASCII letter or digit (a TeX extension font mapping
	 * its summation to "X", a Symbol-layout font mapping mu to "m"): override
	 * only with a symbol or a distinctly Greek letter. */
	return (is_symbol(known) || is_distinct_greek(known)) ? known : current;
}
