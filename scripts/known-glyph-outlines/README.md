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
- ligatures without a single code point (`ti`, `ft`, `ty`).

Labels can be wrong even when they come from ToUnicode consensus, because some
producers' ToUnicode maps are broken themselves (Elsevier maps `+` to `þ`).
