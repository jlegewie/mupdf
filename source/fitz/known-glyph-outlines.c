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

	An entry's value is a character, or FZ_KNOWN_OUTLINE_SEQUENCE + n for
	the n-th string in fz_known_glyph_outline_sequences: a ligature with no
	code point of its own ("tt", "ti"). Ligatures that have one (U+FB00-FB06)
	are stored as that character; the stext device expands them into letters
	unless ligatures are preserved.

	This file also holds two repairs that need no outline: the Symbol-font
	Private Use Area mapping (fz_symbol_font_private_use_unicode), where the
	font name identifies the encoding, and the glyph-name repair
	(fz_glyph_name_repair_unicode), where the glyph's own name does.
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
#include "symbol-encoding-table.h"
#include "adobe-private-use-table.h"

#include <string.h>

const char *
fz_known_glyph_outline_sequence(int value)
{
	if (value < FZ_KNOWN_OUTLINE_SEQUENCE)
		return NULL;
	value -= FZ_KNOWN_OUTLINE_SEQUENCE;
	if (value >= (int)nelem(fz_known_glyph_outline_sequences) || !fz_known_glyph_outline_sequences[value])
		return NULL;
	return fz_known_glyph_outline_sequences[value];
}

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
	if (c >= FZ_KNOWN_OUTLINE_SEQUENCE)
		return 1;
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

/* A value no correct ToUnicode entry produces for a drawn glyph. Private
 * Use Area values are font-specific (Word's Symbol-font codes, Adobe
 * alternate figures, publishers' ligature slots), so they carry no meaning
 * outside the font either. */
static int
is_garbage_unicode(int c)
{
	return c == FZ_REPLACEMENT_CHARACTER || c < 32 || (c >= 127 && c < 160) ||
		(c >= 0xE000 && c <= 0xF8FF);
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
	if (c >= FZ_KNOWN_OUTLINE_SEQUENCE)
		return 0;
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
	if (!font->flags.unicode_from_glyph_names && !font->flags.unicode_from_tounicode &&
		!font->flags.unicode_from_cid_font)
		return current;
	/* CID fonts: only values that cannot be right. */
	if (font->flags.unicode_from_cid_font && !is_garbage_unicode(current))
		return current;
	if (font->flags.unicode_from_tounicode &&
		!is_garbage_unicode(current) && !is_suspicious_latin1(current) && !is_ascii_alnum(current))
		return current;

	known = fz_known_glyph_outline_unicode(ctx, font, gid);
	if (!known || known == current)
		return current;
	/* A sequence (a ligature such as "tt") only replaces garbage: a single
	 * letter from the font's own mapping may be right. */
	if (known >= FZ_KNOWN_OUTLINE_SEQUENCE)
		return is_garbage_unicode(current) ? known : current;
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

/* Symbol-layout fonts: Adobe's Symbol and its clones (SymbolMT, Symbol Greek,
 * Euclid Symbol, MT Symbol, SymbolProportionalBT, OpenSymbol's Symbol
 * range). Every font name in the test corpus that contains "symbol" and
 * emits U+F020-U+F0FF follows this layout. */
static int
is_symbol_layout_font(fz_context *ctx, fz_font *font)
{
	const char *name = fz_font_name(ctx, font);
	const char *p;

	if (!name)
		return 0;
	for (p = name; *p; p++)
		if (fz_strncasecmp(p, "symbol", 6) == 0)
			return 1;
	return 0;
}

int
fz_symbol_font_private_use_unicode(fz_context *ctx, fz_font *font, int current)
{
	int u;

	if (current < 0xF020 || current > 0xF0FF || !font)
		return current;
	if (!is_symbol_layout_font(ctx, font))
		return current;
	u = fz_symbol_encoding_unicode[current - 0xF020];
	return u ? u : current;
}

static int
adobe_private_use(int c)
{
	int l = 0;
	int r = (int)nelem(fz_adobe_private_use) - 1;
	while (l <= r)
	{
		int m = (l + r) >> 1;
		if (c < fz_adobe_private_use[m].pua)
			r = m - 1;
		else if (c > fz_adobe_private_use[m].pua)
			l = m + 1;
		else
			return fz_adobe_private_use[m].ucs;
	}
	return 0;
}

static int
parse_hex(const char *s, int min, int max)
{
	int n = 0, v = 0;
	for (; *s; s++, n++)
	{
		int d;
		if (n == max)
			return -1; /* too long; checked first so the value cannot overflow */
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else return -1;
		v = v * 16 + d;
	}
	return (n >= min && n <= max) ? v : -1;
}

/* One component of a glyph name: an Adobe Glyph List name, or "uniXXXX" /
 * "uXXXX[XX]". Only exact forms count; the looser heuristics of
 * fz_unicode_from_glyph_name ("g123", "C45", ...) are guesses. */
static int
glyph_name_component(const char *s, int *from_list)
{
	int u = fz_unicode_from_glyph_name_strict(s);
	*from_list = 0;
	if (u)
	{
		*from_list = 1;
		return u;
	}
	if (!strncmp(s, "uni", 3))
		u = parse_hex(s + 3, 4, 4);
	else if (s[0] == 'u')
		u = parse_hex(s + 1, 4, 6);
	else
		return 0;
	return (u < 0 || (u >= 0xD800 && u <= 0xDFFF) || u > 0x10FFFF) ? 0 : u;
}

int
fz_glyph_name_repair_unicode(fz_context *ctx, fz_font *font, int gid, int current, int *out, int max)
{
	char name[64];
	char *p, *q, *dot;
	int n = 0, suffixed, variant = 0, i;

	if (!font || gid < 0 || !is_garbage_unicode(current))
		return 0;
	/* Simple fonts only: glyph names in CID fonts are rare and, where
	 * present, often arbitrary (a CNKI font's dash is named "parenright"). */
	if (!font->flags.unicode_from_glyph_names && !font->flags.unicode_from_tounicode)
		return 0;

	name[0] = 0;
	fz_get_glyph_name(ctx, font, gid, name, sizeof name);
	name[sizeof name - 1] = 0;
	dot = strchr(name, '.');
	suffixed = dot != NULL;
	if (dot)
		*dot = 0;
	if (!name[0])
		return 0;

	for (p = name; p; p = q)
	{
		int u, from_list;
		q = strchr(p, '_');
		if (q)
			*q++ = 0;
		u = glyph_name_component(p, &from_list);
		/* A ToUnicode CMap that gives a Private Use value on purpose is
		 * only overruled by a name that means the same value ("Asmall" for
		 * U+F761): CNKI fonts map a dash to U+E5D0 and name it
		 * "parenright". */
		if (font->flags.unicode_from_tounicode && current >= 0xE000 && current <= 0xF8FF &&
			(u != current || q || n > 0))
			return 0;
		if (u >= 0xE000 && u <= 0xF8FF)
		{
			/* Adobe's Corporate Use values name a variant of a character
			 * ("Asmall", "oneoldstyle"). A "uniF761" name only repeats the
			 * private-use value, which an icon font may use for anything. */
			if (!from_list)
				return 0;
			u = adobe_private_use(u);
			variant = 1;
		}
		if (!u || is_garbage_unicode(u) || n == max)
			return 0;
		out[n++] = u;
	}

	if (n > 1)
	{
		/* Ligatures ("f_i", "t_t"): letters only. */
		for (i = 0; i < n; i++)
			if (!is_letter(out[i]))
				return 0;
		return n;
	}
	/* A plain letter or digit name ("a", "one") is the case where symbol
	 * fonts lie, and a font of such glyphs without Unicode is an unmapped
	 * text layer, which is recovered or OCRed as a whole. Only variants
	 * ("a.sc", "one.osf", "Asmall") and other characters are repaired. */
	if (is_ascii_alnum(out[0]) && !suffixed && !variant)
		return 0;
	return n;
}
