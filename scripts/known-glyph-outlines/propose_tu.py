import os
"""Round-4 proposer: outlines that fonts WITH a ToUnicode emit as garbage.

Garbage = what fz_known_glyph_outline_override may replace in a ToUnicode font:
U+FFFD / CID fallback, C0/C1 controls, Latin-1 letters and vulgar fractions.
Label sources: consensus of non-garbage emissions of the identical outline in
other fonts (any ToUnicode state), else the render-and-match classifier.
Output: candidates-tu.jsonl sorted by garbage volume, excluding outlines
already reviewed.
"""
import json, sys, io, collections, unicodedata, numpy as np, pymupdf, freetype
from render_lib import render_face_glyph
pymupdf.TOOLS.mupdf_display_errors(False)
CORPUS = os.environ.get('GLYPH_OUTLINE_CORPUS', 'pdfs')
trace, review_path, out = sys.argv[1], sys.argv[2], sys.argv[3]
reviewed = set(json.load(open(review_path)))
def garbage(u):
    if u < 0 or u == 0xFFFD or u < 0x20 or 0x7F <= u < 0xA0: return True
    return 0xA0 <= u <= 0xFF and (unicodedata.category(chr(u)).startswith('L') or u in (0xBC, 0xBD, 0xBE))
bad = collections.defaultdict(collections.Counter); bad_docs = collections.defaultdict(set)
good = collections.defaultdict(collections.Counter); fonts = collections.defaultdict(collections.Counter); ex = {}
for l in open(trace):
    r = json.loads(l)
    for h, u, name, tu, c in r['rows']:
        if h in reviewed: continue
        if tu and garbage(u):
            bad[h][u] += c; bad_docs[h].add(r['file']); fonts[h][name] += c
            if h not in ex and h in r['ex']: ex[h] = [r['file']] + r['ex'][h]
        elif not garbage(u):
            good[h][u] += c
R = np.load('refs.npz'); rv, rp, rc = R['vecs'], R['pos'], R['chars']
uchars = sorted(set(rc)); cidx = {c: i for i, c in enumerate(uchars)}; rci = np.array([cidx[c] for c in rc])
W = np.array([2.0, 2.0, 0.5, 1.0], dtype=np.float32)
cands = []
for h, em in bad.items():
    if len(bad_docs[h]) < 2: continue
    r = {'h': h, 'notu': {str(k): v for k, v in em.items()}, 'emit_docs': {}, 'ndocs': len(bad_docs[h]),
         'fonts': dict(fonts[h].most_common(5)), 'ex': ex.get(h), 'wrong_chars': sum(em.values())}
    r['emit'] = max(em, key=em.get)
    g = good.get(h)
    if g:
        top = max(g, key=g.get)
        if g[top] >= 0.8 * sum(g.values()) and sum(g.values()) >= 5:
            r['label'], r['src'] = chr(top), 'consensus'
    cands.append(r)
byfile = collections.defaultdict(list)
for r in cands:
    if r['ex']: byfile[r['ex'][0]].append(r)
for fn, rs in byfile.items():
    try: doc = pymupdf.open(f'{CORPUS}/{fn}')
    except Exception: continue
    faces = {}
    for r in rs:
        _, x, gid = r['ex']
        try:
            if x not in faces: faces[x] = freetype.Face(io.BytesIO(doc.extract_font(x)[3]))
            res = render_face_glyph(faces[x], gid)
        except Exception: res = None
        if res is None: continue
        d = (1 - rv @ res[0]) + 0.5 * (np.abs(rp - res[1]) @ W)
        best = np.full(len(uchars), 9, dtype=np.float32); np.minimum.at(best, rci, d.astype(np.float32))
        k = int(np.argmin(best)); r['cls'] = uchars[k]; r['cls_d'] = round(float(best[k]), 3)
        if 'label' not in r:
            r['label'], r['src'] = r['cls'], 'classifier'
cands = [r for r in cands if 'label' in r]
cands.sort(key=lambda r: -r['wrong_chars'])
with open(out, 'w') as w:
    for r in cands: w.write(json.dumps(r, ensure_ascii=False) + '\n')
print('candidates', len(cands), collections.Counter(r['src'] for r in cands), 'garbage chars', sum(r['wrong_chars'] for r in cands))
tot = sum(r['wrong_chars'] for r in cands); c = 0
for k, r in enumerate(cands, 1):
    c += r['wrong_chars']
    if k in (50, 100, 200, 300, 500): print(f'  top {k}: {c/tot:.0%}')
