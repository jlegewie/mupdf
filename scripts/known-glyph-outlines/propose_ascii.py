import os
"""Round-5 proposer: ASCII letters/digits emitted by proven-broken ToUnicode symbol fonts.

A font is proven broken when it emits a wrong ASCII value for an outline already in
the table. Its other ASCII-emitted outlines are classified; candidates are those
the classifier confidently labels a symbol (Unicode S*). Text fonts are skipped.
"""
import json, re, sys, io, collections, unicodedata, numpy as np, pymupdf, freetype
from render_lib import render_face_glyph
pymupdf.TOOLS.mupdf_display_errors(False)
CORPUS = os.environ.get('GLYPH_OUTLINE_CORPUS', 'pdfs')
trace, table_h, review_path, out = sys.argv[1:5]
tab = {}
for l in open(table_h):
    m = re.match(r'\s*\{ 0x([0-9a-f]+)ULL, 0x([0-9a-f]+) \}', l)
    if m: tab[m.group(1)] = int(m.group(2), 16)
reviewed = set(json.load(open(review_path)))
rows = [json.loads(l) for l in open(trace)]
broken = collections.defaultdict(set)
for r in rows:
    for h, u, name, tu, c in r['rows']:
        if tu and h in tab and 0x20 < u < 0x7F and chr(u).isalnum() and u != tab[h]: broken[r['file']].add(name)
em = collections.defaultdict(collections.Counter); docs = collections.defaultdict(set); fonts = collections.defaultdict(collections.Counter); ex = {}
for r in rows:
    fs = broken.get(r['file'])
    if not fs: continue
    for h, u, name, tu, c in r['rows']:
        if tu and name in fs and h not in tab and h not in reviewed and 0x20 < u < 0x7F and chr(u).isalnum():
            em[h][u] += c; docs[h].add(r['file']); fonts[h][name] += c
            if h not in ex and h in r['ex']: ex[h] = [r['file']] + r['ex'][h]
R = np.load('refs.npz'); rv, rp, rc = R['vecs'], R['pos'], R['chars']
uchars = sorted(set(rc)); cidx = {c: i for i, c in enumerate(uchars)}; rci = np.array([cidx[c] for c in rc])
W = np.array([2.0, 2.0, 0.5, 1.0], dtype=np.float32)
cands = []
for h, e in em.items():
    if h not in ex: continue
    fn, x, gid = ex[h]
    try:
        face = freetype.Face(io.BytesIO(pymupdf.open(f'{CORPUS}/{fn}').extract_font(x)[3]))
        res = render_face_glyph(face, gid)
    except Exception: continue
    if res is None: continue
    d = (1 - rv @ res[0]) + 0.5 * (np.abs(rp - res[1]) @ W)
    best = np.full(len(uchars), 9, dtype=np.float32); np.minimum.at(best, rci, d.astype(np.float32))
    k = int(np.argmin(best)); lab = uchars[k]
    emit = max(e, key=e.get)
    emit_d = float(best[cidx[chr(emit)]]) if chr(emit) in cidx else 9
    if not unicodedata.category(lab).startswith('S') or best[k] > 0.3 or emit_d - best[k] < 0.25: continue
    cands.append({'h': h, 'emit': emit, 'label': lab, 'src': 'classifier-ascii', 'wrong_chars': sum(e.values()),
                  'ndocs': len(docs[h]), 'fonts': dict(fonts[h].most_common(3)), 'ex': ex[h],
                  'notu': {str(k): v for k, v in e.items()}})
cands.sort(key=lambda r: -r['wrong_chars'])
with open(out, 'w') as w:
    for r in cands: w.write(json.dumps(r, ensure_ascii=False) + '\n')
print('candidates', len(cands), 'chars', sum(r['wrong_chars'] for r in cands))
