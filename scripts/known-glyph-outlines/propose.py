import os
import re
"""Stage 3: label outlines and propose table entries.

Label sources, in priority order:
  tounicode  - fonts WITH ToUnicode emit a consistent char for the identical outline (>=90%, >=2 docs)
  classifier - nearest reference render (refs.npz), only when no ToUnicode evidence exists
An outline becomes a candidate when its no-ToUnicode output differs from the label.
Output: candidates.jsonl sorted by wrong-char volume.
"""
import json, sys, io, collections, unicodedata, numpy as np, pymupdf, freetype
from render_lib import render_face_glyph
pymupdf.TOOLS.mupdf_display_errors(False)
CORPUS = os.environ.get('GLYPH_OUTLINE_CORPUS', 'pdfs')
agg, out = sys.argv[1], sys.argv[2]
GROUPS = ["AΑ","BΒ","EΕ","ZΖ","HΗ","IΙ","KΚ","MΜ","NΝ","OΟ","PΡ","TΤ","XΧ","YΥ","oο","µμ","ϕφ","εϵ","θϑ","Δ∆","Σ∑","Π∏","ΩΩ","'’","`‘","*∗","~∼","·⋅∙","-‐"]
EQ = {c: set(g) for g in GROUPS for c in g}
def ch(u): return None if u is None or u < 0 else chr(u)
def eqv(a, b):
    if a is None or b is None: return False
    if a == b or b in EQ.get(a, ()): return True
    na, nb = unicodedata.normalize('NFKC', a), unicodedata.normalize('NFKC', b)
    if na == nb: return True
    # ligatures: a one-to-many ToUnicode yields only the first char per glyph
    if len(na) > 1 and na.startswith(nb) or len(nb) > 1 and nb.startswith(na): return True
    # small caps / case variants share outlines across fonts
    if na.isalpha() and nb.isalpha() and na.lower() == nb.lower(): return True
    return False
def wrong(r, label):
    # emissions that disagree with the label, counted only when seen in >=2 documents
    ed = {int(k): v for k, v in r.get('emit_docs', {}).items()}
    return sum(v for u, v in ((int(k), v) for k, v in r['notu'].items())
               if not eqv(ch(u), label) and ed.get(u, 0) >= 2
               and not (u < 0 and label.isascii()))   # CID fallback likely already right for ASCII
R = np.load('refs.npz'); rv, rp, rc = R['vecs'], R['pos'], R['chars']
uchars = sorted(set(rc)); cidx = {c: i for i, c in enumerate(uchars)}; rci = np.array([cidx[c] for c in rc])
W = np.array([2.0, 2.0, 0.5, 1.0], dtype=np.float32)
def classify(vec, pos):
    d = (1 - rv @ vec) + 0.5 * (np.abs(rp - pos) @ W)
    best = np.full(len(uchars), 9, dtype=np.float32)
    np.minimum.at(best, rci, d.astype(np.float32))
    return best
rows = [json.loads(l) for l in open(agg)]
cands = []; todo = []
stat = collections.Counter()
for r in rows:
    notu = {int(k): v for k, v in r['notu'].items()}
    e = max(notu, key=notu.get); r['emit'] = e
    tu = {int(k): v for k, v in r['tu'].items()}
    t = None
    if tu and r['tu_ndocs'] >= 2:
        tt = max(tu, key=tu.get)
        share = tu[tt] / sum(tu.values())
        if share >= 0.6 and tt >= 0x20 and tt != 0xFFFD: t = tt
    if t is not None:
        if wrong(r, chr(t)) == 0: stat['ok_tounicode'] += 1; continue
        # a split ToUnicode vote usually means some producers' ToUnicode maps are wrong too
        r['label'], r['src'] = chr(t), ('tounicode' if share >= 0.9 else 'tounicode-weak'); cands.append(r)
    else:
        wrongish = e < 0 or e < 0x20 or r['ndocs'] >= 2 or sum(notu.values()) >= 20
        if not wrongish: stat['skip_rare'] += 1; continue
        todo.append(r)
print('outlines', len(rows), dict(stat), 'tounicode candidates', len(cands), 'to classify', len(todo))
# classify: group by exemplar file to open each doc once
byfile = collections.defaultdict(list)
for r in todo + cands:
    if r['ex']: byfile[r['ex'][0]].append(r)
for i, (fn, rs) in enumerate(byfile.items()):
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
        best = classify(*res)
        k = int(np.argmin(best)); r['cls'] = uchars[k]; r['cls_d'] = round(float(best[k]), 3)
        e = ch(r['emit']); r['emit_d'] = round(float(best[cidx[e]]), 3) if e in cidx else None
        if e and e.isascii() and e.isalpha() and e.upper() in cidx:
            r['upper_d'] = round(float(best[cidx[e.upper()]]), 3)
    if i % 2000 == 0: print('classified files', i, flush=True)
def auto_reject(r, e):
    """Systematic classifier false positives seen in review."""
    lab = r['cls']
    if e is None: return None
    if e in 'Jj' and not lab.isalpha(): return 'J with descender'
    if e.isascii() and e.isalpha() and 'upper_d' in r and r['upper_d'] - r['cls_d'] < 0.12:
        return 'small caps'
    if r['fonts'] and any(re.match(r'(CMEX|cmex|.*EX\d*$|.*BLEX|LCIRCLE|lcircle)', f) for f in r['fonts']) \
       and lab in '¡¸¯ˆ˜‾¿': return 'TeX extension piece'
    return None
isL = lambda c: c is not None and len(c) == 1 and c.isalpha()
for r in todo:
    if 'cls' not in r: continue
    e = ch(r['emit'])
    if eqv(e, r['cls']):
        # dominant output is right; a minority of fonts may still emit something else
        if r['cls_d'] < 0.3 and wrong(r, r['cls']) > 0:
            r['label'], r['src'] = r['cls'], 'classifier-minority'
            r['letter_to_letter'] = False; r['auto_reject'] = None
            cands.append(r)
        continue
    if r['emit'] >= 0x20 and e not in cidx: continue          # emitted script not covered by references
    if r['emit'] < 0 and r['cls'].isascii(): continue         # CID fallback likely already emits this
    garbage = r['emit'] < 0x20
    confident = r['cls_d'] < 0.3 and (r['emit_d'] is None or r['emit_d'] - r['cls_d'] > 0.25)
    if garbage or confident:
        r['label'], r['src'] = r['cls'], 'classifier'
        r['letter_to_letter'] = isL(e) and isL(r['cls'])
        r['auto_reject'] = auto_reject(r, e)
        cands.append(r)
for r in cands: r['wrong_chars'] = wrong(r, r['label'])
cands = [r for r in cands if r['wrong_chars'] > 0]
cands.sort(key=lambda r: -r['wrong_chars'])
with open(out, 'w') as w:
    for r in cands: w.write(json.dumps(r, ensure_ascii=False) + '\n')
print('candidates', len(cands), collections.Counter(r['src'] for r in cands))
