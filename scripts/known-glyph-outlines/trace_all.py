"""Stage 1: per document, count (outline hash, emitted char) for every simple-font glyph drawn.

Emitted char models Beaver's MuPDF flags: U+FFFD -> numeric 'C<n>' glyph name if n>0,
else the raw char code (recorded as -1 = CID fallback garbage).
"""
import pymupdf, io, os, re, sys, json, collections, freetype
from multiprocessing import Pool
from glyphhash import glyph_hash
from fontTools.agl import toUnicode
pymupdf.TOOLS.mupdf_display_errors(False)
CORPUS = sys.argv[1]; OUT = sys.argv[2]; MAXPAGES = 60
def strip(b): return re.sub(r'^[A-Z]{6}\+', '', b)
NUM = re.compile(r'C(\d+)$')
def work(fn):
    try: return _work(fn)
    except Exception as e: return fn, [], {}
def _work(fn):
    try: doc = pymupdf.open(os.path.join(CORPUS, fn))
    except Exception: return fn, [], {}
    fonts = {}        # xref -> dict(name, tu, face)
    byname = collections.defaultdict(list)
    counts = collections.defaultdict(collections.Counter); ex = {}
    agree = collections.defaultdict(lambda: [0, 0]); bad = set(); names = {}
    hcache = {}
    unusable = set()
    for pno in range(min(len(doc), MAXPAGES)):
        page = doc[pno]
        try:
            for f in page.get_fonts(full=True):
                xref, ext, typ, base = f[0], f[1], f[2], f[3]
                if xref in fonts: continue
                fonts[xref] = None
                if typ not in ('Type1', 'MMType1', 'TrueType'): unusable.add(strip(base)); continue
                try:
                    info = doc.extract_font(xref)
                    if not info[3]: unusable.add(strip(base)); continue
                    face = freetype.Face(io.BytesIO(info[3]))
                except Exception: unusable.add(strip(base)); continue
                tu = doc.xref_get_key(xref, 'ToUnicode')[0] != 'null'
                fonts[xref] = dict(name=strip(base), tu=tu, face=face)
                byname[strip(base)].append(xref)
            tr = page.get_texttrace()
        except Exception: continue
        for s in tr:
            # Every same-named font on the document must be a usable embedded simple font;
            # otherwise the span may come from a substituted/other font whose glyph ids we can't hash.
            xs = byname.get(s['font'], [])
            if not xs or not all(fonts[x] for x in xs) or s['font'] in unusable: continue
            for ch in s['chars']:
                u, gid = ch[0], ch[1]
                if gid is None or gid < 0: continue
                hs = set()
                for x in xs:
                    k = (x, gid)
                    if k not in hcache:
                        try: hcache[k] = glyph_hash(fonts[x]['face'], gid)
                        except Exception: hcache[k] = None
                    hs.add(hcache[k])
                if len(hs) != 1: continue
                h = hs.pop()
                x = xs[0]; fi = fonts[x]
                if gid >= fi['face'].num_glyphs: bad.add(x); continue
                if h is None: continue
                if (x, gid) not in names:
                    try: names[(x, gid)] = fi['face'].get_glyph_name(gid).decode('latin-1')
                    except Exception: names[(x, gid)] = ''
                gname = names[(x, gid)]
                m = NUM.match(gname)
                if u == 0xFFFD:
                    u = int(m.group(1)) if (m and 0 < int(m.group(1)) <= 0x10ffff) else -1
                # indexing check: without ToUnicode, MuPDF's char comes from the glyph name
                if not fi['tu'] and gname and gname != '.notdef':
                    exp = (int(m.group(1)) or None) if m else (ord(toUnicode(gname)) if len(toUnicode(gname)) == 1 else None)
                    if exp is not None and u >= 0:
                        agree[x][1] += 1; agree[x][0] += (exp == u)
                counts[x][(h, u, fi['name'], int(fi['tu']))] += 1
                if h not in ex: ex[h] = (x, gid)
    ok = [x for x in counts if x not in bad and not (agree[x][1] >= 5 and agree[x][0] < 0.8 * agree[x][1])]
    merged = collections.Counter()
    for x in ok: merged.update(counts[x])
    okset = set(ok)
    ex = {h: v for h, v in ex.items() if v[0] in okset}
    return fn, [[h, u, n_, tu, c] for (h, u, n_, tu), c in merged.items()], ex
if __name__ == '__main__':
    files = sorted(os.listdir(CORPUS)); n = int(sys.argv[3]) if len(sys.argv) > 3 else len(files)
    done = set()
    if os.path.exists(OUT):
        for l in open(OUT):
            try: done.add(json.loads(l)['file'])
            except Exception: pass
    files = [f for f in files[:n] if f not in done]; n = len(files)
    with Pool(10, maxtasksperchild=200) as p, open(OUT, 'a') as w:
        for i, (fn, rows, ex) in enumerate(p.imap_unordered(work, files[:n], chunksize=2)):
            w.write(json.dumps({'file': fn, 'rows': rows, 'ex': ex}) + '\n')
            if i % 500 == 0: print(i, flush=True)
