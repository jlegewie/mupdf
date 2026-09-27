# Known glyph outlines: table pipeline

Generates `source/fitz/known-glyph-outlines-table.h`, the table behind the
`use-known-glyph-outlines` stext option (see `FORK.md`). Each entry maps the hash
of a glyph outline to the character the outline actually draws.

`review.json` holds every review decision and is the source of truth.
Regenerating the table needs nothing else:

```sh
python3 emit_table.py review.json ../../source/fitz/known-glyph-outlines-table.h
```

## Outline hash (version 1)

Defined in `source/fitz/known-glyph-outlines.c`, implemented in Python by
`glyphhash.py`. `fthash.c` is a C reference that can be built against the
fork's bundled FreeType to check that both produce identical hashes on a font
file:

```sh
cc -O2 -Ithirdparty/freetype/include -Iscripts/freetype \
  -DFT_CONFIG_MODULES_H='"slimftmodules.h"' -DFT_CONFIG_OPTIONS_H='"slimftoptions.h"' \
  scripts/known-glyph-outlines/fthash.c build/release/libmupdf-third.a -o fthash
```

If the hash definition or FreeType's outline loading changes, every entry must
be regenerated. The fork regression samples (`make fork-regression-test`) fail
if the table stops matching.

## Adding entries

The scripts need Python with `pymupdf`, `fonttools`, `freetype-py`, `numpy` and
`pillow` (for example `uv run --with ...`). Set `GLYPH_OUTLINE_CORPUS` to a
directory of PDFs.

1. `trace_all.py <corpus> <trace.jsonl>`: for every embedded simple font, count
   (outline hash, emitted character, font, has ToUnicode) per document.
2. `aggregate.py <trace.jsonl> <agg.jsonl>`: per-outline statistics.
3. `refs.py`: renders reference glyphs (Latin, Greek, math) from system fonts
   into `refs.npz` for the classifier. The font paths are macOS paths.
4. Propose candidates, each with a label from ToUnicode consensus or the
   render-and-match classifier:
   - `propose.py`: fonts without ToUnicode.
   - `propose_tu.py`: fonts whose ToUnicode emits garbage (U+FFFD, controls,
     Latin-1 letters).
   - `propose_ascii.py`: ASCII values in proven-broken ToUnicode symbol fonts.
   - `propose_poisoned.py`: outlines whose ToUnicode consensus "confirmed" an
     ASCII letter that the drawing contradicts (Symbol-layout fonts map mu to
     `m`).
5. `sheets.py <candidates.jsonl> <outdir>`: contact sheets for review.
   `record.py <candidates.jsonl> review.json '<decisions>'` records decisions
   (`<idx> a`, `<idx> r <note>`, `<idx> = <char> <note>`).
6. `emit_table.py review.json <table.h>`.

Labels may be several letters for a ligature. `emit_table.py` stores
`ff fi fl ffi ffl st` as their U+FB0x code points, which MuPDF expands unless
ligatures are preserved, and any other ligature (`tt`, `ti`, `ft`) as a
sequence in `fz_known_glyph_outline_sequences`.

### Garbage in any font (round 8)

Values that cannot be right (U+FFFD, controls, Private Use Area) in fonts
with a ToUnicode CMap and in CID fonts:

1. `trace_garbage.py <corpus> <trace.jsonl>`: like `trace_all.py`, but for every
   embedded font including CID fonts, with multi-character ToUnicode values
   (ligature consensus) and word contexts of garbage glyphs.
2. `propose_garbage.py <trace.jsonl> review.json refs.npz <candidates.jsonl>`:
   labels from consensus, a dictionary vote over word contexts (ligatures) or
   the classifier. It leaves out documents that are mostly garbage
   (unmapped text layers), Symbol-font U+F020-U+F0FF (`map-symbol-private-use`)
   and glyphs whose names `use-glyph-name-for-garbage` already repairs
   (`namerepair.py` mirrors that C function; keep them in sync). U+FFFD and
   controls are only proposed for symbols and ligatures, never a plain
   letter or digit (see the review rules).
3. `sheets_ctx.py <candidates.jsonl> <outdir>`: contact sheets that show the
   glyph name and word contexts, then `record.py` and `emit_table.py` as
   above.

The fixed tables behind the two name-based options are generated too:
`emit_symbol_encoding.py <mupdf root> <out.h>` (Adobe Symbol encoding, needs
`pypdf`, `fonttools`, `freetype-py`) and `emit_adobe_private_use.py <mupdf
root> <out.h>` (Adobe Corporate Use values of glyph list names).

Then rebuild, run `make fork-regression-test`, and compare extraction with and
without the option on a corpus. Every changed character must be a table
character.

## Review rules

Reject:

- extensible delimiter, radical, integral and wide-accent pieces;
- small-caps, display and italic Latin letters the classifier reads as Greek or
  symbols (J with a descender, italic p with an ascender);
- drawings that stand for more than one character: hyphen, minus or en dash;
  `l`, `I` or `|`; `O` or `0`; degree, ring accent or white bullet (settle these
  from text context when possible);
- a plain letter or digit for a glyph that emits U+FFFD or a control
  character: that is an unmapped text layer, recovered or OCRed as a whole
  by callers that detect it by its U+FFFD;
- icons with no Unicode character (ORCID, book), positional forms of Arabic
  letters, and glyphs a producer maps to a Private Use value on purpose
  (CNKI decorations).

Small capitals and oldstyle figures at Private Use values are labelled with
the lowercase letter and the digit (as `emit_adobe_private_use.py` does for
Adobe's own values).

Labels can be wrong even when they come from ToUnicode consensus, because some
producers' ToUnicode maps are broken themselves (Elsevier maps `+` to `þ`).
