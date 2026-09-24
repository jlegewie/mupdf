"""Stage 4: contact sheets of candidates for human review.

usage: sheets.py candidates.jsonl outdir [start] [count] [--letters]
Each cell: glyph (rendered from the exemplar font), 'emitted' -> proposed label, source, volume.
"""
import json, io, sys, os, pymupdf, freetype, numpy as np
from PIL import Image, ImageDraw, ImageFont
pymupdf.TOOLS.mupdf_display_errors(False)
CORPUS = os.environ.get('GLYPH_OUTLINE_CORPUS', 'pdfs')
src, outdir = sys.argv[1], sys.argv[2]
start = int(sys.argv[3]) if len(sys.argv) > 3 else 0; count = int(sys.argv[4]) if len(sys.argv) > 4 else 200
rows = [json.loads(l) for l in open(src)][start:start + count]
os.makedirs(outdir, exist_ok=True)
F = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial Unicode.ttf', 13)
FB = ImageFont.truetype('/System/Library/Fonts/Supplemental/Arial Unicode.ttf', 22)
CW, CH, COLS, ROWS = 170, 130, 8, 6
def em(u): return '<cid>' if u < 0 else repr(chr(u))
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
        except Exception: pass
        d.line([(X + 4, Y + 66), (X + 80, Y + 66)], fill=210)
        d.text((X + 88, Y + 4), f"#{start + p + n}", font=F, fill=0)
        d.text((X + 88, Y + 22), f"{em(r['emit'])}", font=F, fill=0)
        d.text((X + 88, Y + 42), f"→ {r['label']}", font=FB, fill=0)
        d.text((X + 4, Y + 84), f"{r['src'][:3]} w={r['wrong_chars']} d={r['ndocs']}", font=F, fill=0)
        d.text((X + 4, Y + 100), next(iter(r['fonts']))[:22], font=F, fill=0)
        d.rectangle([X, Y, X + CW - 1, Y + CH - 1], outline=170)
    img.save(f'{outdir}/sheet_{start + p:04d}.png')
print('wrote', (len(rows) + COLS * ROWS - 1) // (COLS * ROWS), 'sheets')
