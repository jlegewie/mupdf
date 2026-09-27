"""Round 8 tracer: every embedded font, including CID (Type0) fonts.

For each document, count (outline hash, emission, font, kind) for every glyph
drawn, where
  emission = the emitted text: a code point (int), or a string when a
             ToUnicode entry maps one glyph to several characters (the
             extra characters arrive with gid -1);
  kind     = 0 simple font without ToUnicode, 1 simple font with ToUnicode,
             2 CID font.
For garbage emissions (U+FFFD, controls, Private Use Area) up to three word
contexts per outline are kept, for dictionary-based labelling of ligatures.

usage: trace_garbage.py <corpus> <out.jsonl> [limit]
"""
import pymupdf, io, os, re, sys, json, collections, freetype
from multiprocessing import Pool
from glyphhash import glyph_hash
pymupdf.TOOLS.mupdf_display_errors(False)
CORPUS = sys.argv[1]; OUT = sys.argv[2]; MAXPAGES = 60
USABLE = ('Type1', 'MMType1', 'TrueType', 'Type0')


def strip(b):
    return re.sub(r'^[A-Z]{6}\+', '', b)


def garbage(u):
    return isinstance(u, int) and (u == 0xFFFD or u < 0x20 or 0x7F <= u < 0xA0 or 0xE000 <= u <= 0xF8FF)


def work(fn):
    try:
        return _work(fn)
    except Exception:
        return fn, [], {}, {}


def _work(fn):
    try:
        doc = pymupdf.open(os.path.join(CORPUS, fn))
    except Exception:
        return fn, [], {}, {}
    fonts = {}
    byname = collections.defaultdict(list)
    unusable = set()
    counts = collections.Counter()
    ex = {}
    ctx = collections.defaultdict(list)
    hcache = {}
    for pno in range(min(len(doc), MAXPAGES)):
        page = doc[pno]
        try:
            for f in page.get_fonts(full=True):
                xref, ext, typ, base = f[0], f[1], f[2], f[3]
                if xref in fonts:
                    continue
                fonts[xref] = None
                if typ not in USABLE or ext == 'n/a':
                    unusable.add(strip(base))
                    continue
                try:
                    info = doc.extract_font(xref)
                    if not info[3]:
                        unusable.add(strip(base))
                        continue
                    face = freetype.Face(io.BytesIO(info[3]))
                except Exception:
                    unusable.add(strip(base))
                    continue
                if typ == 'Type0':
                    kind = 2
                else:
                    kind = int(doc.xref_get_key(xref, 'ToUnicode')[0] != 'null')
                fonts[xref] = dict(name=strip(base), kind=kind, face=face)
                byname[strip(base)].append(xref)
            tr = page.get_texttrace()
        except Exception:
            continue
        # Word context comes from the page's glyphs in drawing order: spans are
        # often a single glyph long.
        flat = [chr(c[0]) if 0 <= c[0] <= 0x10FFFF else '\ufffd' for s in tr for c in s['chars']]
        ptext = ''.join(flat)
        base = 0
        for s in tr:
            sbase = base
            base += len(s['chars'])
            xs = byname.get(s['font'], [])
            if not xs or not all(fonts[x] for x in xs) or s['font'] in unusable:
                continue
            chars = s['chars']
            text = ''.join(chr(c[0]) if 0 <= c[0] <= 0x10FFFF else '�' for c in chars)
            i = 0
            while i < len(chars):
                u, gid = chars[i][0], chars[i][1]
                j = i + 1
                while j < len(chars) and (chars[j][1] is None or chars[j][1] < 0):
                    j += 1
                if gid is None or gid < 0:
                    i = j
                    continue
                em = u if j == i + 1 else ''.join(chr(c[0]) for c in chars[i:j])
                hs = set()
                for x in xs:
                    k = (x, gid)
                    if k not in hcache:
                        try:
                            hcache[k] = glyph_hash(fonts[x]['face'], gid) if gid < fonts[x]['face'].num_glyphs else None
                        except Exception:
                            hcache[k] = None
                    hs.add(hcache[k])
                if len(hs) == 1:
                    h = hs.pop()
                    if h is not None:
                        fi = fonts[xs[0]]
                        counts[(h, em, fi['name'], fi['kind'])] += 1
                        if h not in ex:
                            ex[h] = (xs[0], gid)
                        if garbage(em) and len(ctx[h]) < 3:
                            ctx[h].append([ptext[max(0, sbase + i - 14):sbase + i], ptext[sbase + j:sbase + j + 14]])
                i = j
    rows = [[h, em, n, k, c] for (h, em, n, k), c in counts.items()]
    return fn, rows, ex, ctx


if __name__ == '__main__':
    files = sorted(f for f in os.listdir(CORPUS) if f.lower().endswith('.pdf'))
    if len(sys.argv) > 3:
        files = files[:int(sys.argv[3])]
    done = set()
    if os.path.exists(OUT):
        for l in open(OUT):
            try:
                done.add(json.loads(l)['file'])
            except Exception:
                pass
    files = [f for f in files if f not in done]
    with Pool(10, maxtasksperchild=200) as p, open(OUT, 'a') as w:
        for i, (fn, rows, ex, ctx) in enumerate(p.imap_unordered(work, files, chunksize=2)):
            w.write(json.dumps({'file': fn, 'rows': rows, 'ex': ex, 'ctx': ctx}, ensure_ascii=True) + '\n')
            w.flush()
            if i % 500 == 0:
                print(i, flush=True)
