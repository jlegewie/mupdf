"""Contact sheets with text context, for round-8 candidates (propose_garbage.py).

usage: sheets_ctx.py candidates.jsonl outdir [start] [count]
Each cell: the glyph rendered from its exemplar font, candidate index, emitted
value -> proposed label (source), classifier / dictionary labels, glyph name,
font, volume, and two word contexts with the glyph shown as [].
"""
import json, io, sys, os, pymupdf, freetype, numpy as np
from PIL import Image, ImageDraw, ImageFont
pymupdf.TOOLS.mupdf_display_errors(False)
CORPUS = os.environ.get('GLYPH_OUTLINE_CORPUS', 'pdfs')
src, outdir = sys.argv[1], sys.argv[2]
start = int(sys.argv[3]) if len(sys.argv) > 3 else 0
count = int(sys.argv[4]) if len(sys.argv) > 4 else 200
rows = [json.loads(l) for l in open(src)][start:start + count]
os.makedirs(outdir, exist_ok=True)
F = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial Unicode.ttf', 13)
FB = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial Unicode.ttf', 20)
CW, CH, COLS, ROWS = 330, 150, 4, 6


def em(e):
    return repr(e) if isinstance(e, str) else ('U+%04X' % e)


def clean(s):
    return ''.join(c if c.isprintable() else '·' for c in s)


for p in range(0, len(rows), COLS * ROWS):
    chunk = rows[p:p + COLS * ROWS]
    img = Image.new('L', (CW * COLS, CH * ROWS), 255); d = ImageDraw.Draw(img)
    for n, r in enumerate(chunk):
        X, Y = (n % COLS) * CW, (n // COLS) * CH
        try:
            fn, x, gid = r['ex']
            face = freetype.Face(io.BytesIO(pymupdf.open(f'{CORPUS}/{fn}').extract_font(x)[3]))
            face.set_pixel_sizes(0, 60); face.load_glyph(gid, freetype.FT_LOAD_RENDER); bm = face.glyph.bitmap
            if bm.width:
                a = 255 - np.array(bm.buffer, dtype=np.uint8).reshape(bm.rows, bm.pitch)[:, :bm.width]
                img.paste(Image.fromarray(a), (X + 8 + max(0, face.glyph.bitmap_left), Y + 66 - face.glyph.bitmap_top))
        except Exception:
            pass
        d.line([(X + 4, Y + 66), (X + 80, Y + 66)], fill=210)
        d.text((X + 96, Y + 2), f"#{start + p + n}  {em(r['emit'])}", font=F, fill=0)
        d.text((X + 96, Y + 18), f"→ {r['label']}  ({r['src'][:4]})", font=FB, fill=0)
        d.text((X + 96, Y + 44), f"cls={r.get('cls', '')} dict={r.get('dict', '')}", font=F, fill=0)
        d.text((X + 96, Y + 60), f"name={r.get('gname', '')[:24]}", font=F, fill=0)
        d.text((X + 4, Y + 80), f"{next(iter(r['fonts']))[:30]}  w={r['wrong_chars']} d={r['ndocs']}", font=F, fill=0)
        for k, (left, right) in enumerate(r.get('ctx', [])[:3]):
            d.text((X + 4, Y + 96 + 16 * k), clean(f"{left[-16:]}[]{right[:16]}"), font=F, fill=0)
        d.rectangle([X, Y, X + CW - 1, Y + CH - 1], outline=170)
    img.save(f'{outdir}/sheet_{start + p:04d}.png')
print('wrote', (len(rows) + COLS * ROWS - 1) // (COLS * ROWS), 'sheets')
