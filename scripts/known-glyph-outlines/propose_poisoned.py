import os
"""Round-6 proposer: outlines whose ToUnicode consensus 'confirmed' an ASCII letter/digit,
but whose drawing the classifier confidently reads as a non-lookalike Greek letter or a symbol.
Broken Symbol-layout ToUnicode maps (e.g. AdvPS7DA6: mu -> 'm') can outvote the truth."""
import json, sys, io, collections, unicodedata, numpy as np, pymupdf, freetype
from render_lib import render_face_glyph
pymupdf.TOOLS.mupdf_display_errors(False)
CORPUS = os.environ.get('GLYPH_OUTLINE_CORPUS', 'pdfs')
agg, review_path, out = sys.argv[1:4]
reviewed = set(json.load(open(review_path)))
LOOK = set('ΑΒΕΖΗΙΚΜΝΟΡΤΥΧοϲ')
def target(c):
    if unicodedata.category(c).startswith('S'): return True
    u = ord(c); return (0x391 <= u <= 0x3c9 or 0x3d0 <= u <= 0x3f5) and c not in LOOK
R = np.load('refs.npz'); rv, rp, rc = R['vecs'], R['pos'], R['chars']
uchars = sorted(set(rc)); cidx = {c: i for i, c in enumerate(uchars)}; rci = np.array([cidx[c] for c in rc])
W = np.array([2.0, 2.0, 0.5, 1.0], dtype=np.float32)
todo = []
for l in open(agg):
    r = json.loads(l)
    if r['h'] in reviewed or not r['ex'] or r['ndocs'] < 2: continue
    notu = {int(k): v for k, v in r['notu'].items()}; e = max(notu, key=notu.get)
    if not (0x20 < e < 0x7f and chr(e).isalnum()): continue
    tu = {int(k): v for k, v in r['tu'].items()}
    if not tu or max(tu, key=tu.get) != e: continue          # only consensus-"confirmed" outlines
    r['emit'] = e; todo.append(r)
byfile = collections.defaultdict(list)
for r in todo: byfile[r['ex'][0]].append(r)
cands = []
for fn, rs in byfile.items():
    try: doc = pymupdf.open(f'{CORPUS}/{fn}')
    except Exception: continue
    faces = {}
    for r in rs:
        _, x, gid = r['ex']
        try:
            if x not in faces: faces[x] = freetype.Face(io.BytesIO(doc.extract_font(x)[3]))
            res = render_face_glyph(faces[x], gid)
        except Exception: continue
        if res is None: continue
        d = (1 - rv @ res[0]) + 0.5 * (np.abs(rp - res[1]) @ W)
        best = np.full(len(uchars), 9, dtype=np.float32); np.minimum.at(best, rci, d.astype(np.float32))
        k = int(np.argmin(best)); lab = uchars[k]; e = chr(r['emit'])
        ed = float(best[cidx[e]]) if e in cidx else 9
        up = float(best[cidx[e.upper()]]) if e.upper() in cidx else 9
        if not target(lab) or best[k] > 0.25 or ed - best[k] < 0.3 or up - best[k] < 0.15: continue
        r['label'], r['src'] = lab, 'classifier-poisoned'
        r['wrong_chars'] = sum(v for kk, v in r['notu'].items()) + sum(r['tu'].values())
        cands.append(r)
cands.sort(key=lambda r: -r['wrong_chars'])
with open(out, 'w') as w:
    for r in cands: w.write(json.dumps(r, ensure_ascii=False) + '\n')
print('checked', len(todo), 'consensus-confirmed ASCII outlines; candidates', len(cands), 'chars', sum(r['wrong_chars'] for r in cands))
