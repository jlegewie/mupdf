"""Round-8 proposer: outlines emitted as garbage (U+FFFD, controls, Private Use
Area) in any embedded font, including CID fonts. Input is trace_garbage.py output.

Symbol-layout fonts' U+F020-U+F0FF values are left out: the stext option
map-symbol-private-use translates them through the Symbol encoding. So are
glyphs whose names the option use-glyph-name-for-garbage repairs (namerepair.py).

Label sources, in priority order:
  consensus  - the identical outline's non-garbage emissions elsewhere (>= 80%, >= 3
               glyphs in >= 2 documents); may be a multi-letter ToUnicode value ("ti")
  dictionary - the glyph sits inside words: the ligature or letter that turns the
               most contexts into dictionary words (needs >= 2 contexts, a clear winner)
  classifier - nearest reference render (refs.npz)
Documents whose glyphs are more than 20% garbage are skipped: they are unmapped
text layers, which the extractor's recovery and OCR paths handle, and partial
repair would hide them from that detection.
Output: candidates sorted by garbage volume, excluding reviewed outlines.

usage: propose_garbage.py trace.jsonl review.json refs.npz out.jsonl [words]
"""
import os, re, sys, io, json, collections, numpy as np, pymupdf, freetype
from render_lib import render_face_glyph
from namerepair import repair as name_repair
pymupdf.TOOLS.mupdf_display_errors(False)
CORPUS = os.environ.get('GLYPH_OUTLINE_CORPUS', 'pdfs')
trace, review_path, refs, out = sys.argv[1:5]
WORDS = sys.argv[5] if len(sys.argv) > 5 else '/usr/share/dict/words'
reviewed = set(json.load(open(review_path)))
LIGS = ['fi', 'fl', 'ff', 'ffi', 'ffl', 'ft', 'fft', 'tt', 'ti', 'tf', 'fb', 'fh', 'fj', 'fk', 'ffj', 'ffb', 'ffh',
        'ffk', 'Th', 'st', 'ct', 'sp', 'tz']
SINGLE = [chr(c) for c in range(ord('a'), ord('z') + 1)]


def garbage(e):
    return isinstance(e, int) and (e == 0xFFFD or e < 0x20 or 0x7F <= e < 0xA0 or 0xE000 <= e <= 0xF8FF)


def symbol_font_pua(e, name):
    return isinstance(e, int) and 0xF020 <= e <= 0xF0FF and 'symbol' in name.lower()


def key(e):
    return e if isinstance(e, str) else chr(e)


bad = collections.defaultdict(collections.Counter); bad_docs = collections.defaultdict(set)
good = collections.defaultdict(collections.Counter); good_docs = collections.defaultdict(lambda: collections.defaultdict(set))
fonts = collections.defaultdict(collections.Counter); ex = {}; ctx = collections.defaultdict(list)
kinds = collections.defaultdict(collections.Counter)
# An outline seen in one document still qualifies with enough glyphs: publisher and
# journal fonts recur in users' libraries even when a 10k sample has one paper.
MIN_DOCS, MIN_SINGLE_DOC_CHARS = 2, 3
HEAVY = 0.2   # documents whose text is mostly garbage are unmapped text layers: recovery/OCR territory
skipped = 0
for l in open(trace):
    r = json.loads(l)
    tot = sum(c for h, e, name, kind, c in r['rows'])
    gar = sum(c for h, e, name, kind, c in r['rows'] if garbage(e) and not symbol_font_pua(e, name))
    if tot and gar / tot > HEAVY:
        skipped += 1
        continue
    for h, e, name, kind, c in r['rows']:
        if h in reviewed:
            continue
        if garbage(e):
            if symbol_font_pua(e, name):
                continue
            bad[h][e] += c; bad_docs[h].add(r['file']); fonts[h][name] += c; kinds[h][kind] += c
            if h not in ex and h in r['ex']:
                ex[h] = [r['file']] + r['ex'][h]
            if len(ctx[h]) < 40:
                ctx[h].extend(r['ctx'].get(h, []))
        else:
            k = key(e)
            if k.strip():
                good[h][k] += c; good_docs[h][k].add(r['file'])

words = set(w.strip().lower() for w in open(WORDS) if w.strip())


def dictionary_label(cs):
    """Best ligature/letter for in-word contexts, or None."""
    parts = []
    for left, right in cs:
        lw = re.search(r'[A-Za-z]*$', left).group(0)
        rw = re.match(r'[A-Za-z]*', right).group(0)
        if lw or rw:
            parts.append((lw, rw))
    if len(parts) < 2:
        return None, 0, len(parts)
    score = collections.Counter()
    for cand in LIGS + SINGLE:
        for lw, rw in parts:
            if (lw + cand + rw).lower() in words and len(lw + rw) >= 2:
                score[cand] += 1
    if not score:
        return None, 0, len(parts)
    (best, n), *rest = score.most_common(2) + [(None, 0)]
    if n < 2 or n < 0.5 * len(parts) or (rest and rest[0][1] >= n):
        return None, n, len(parts)
    return best, n, len(parts)


R = np.load(refs); rv, rp, rc = R['vecs'], R['pos'], R['chars']
uchars = sorted(set(rc)); cidx = {c: i for i, c in enumerate(uchars)}; rci = np.array([cidx[c] for c in rc])
W = np.array([2.0, 2.0, 0.5, 1.0], dtype=np.float32)
cands = []
for h, em in bad.items():
    if len(bad_docs[h]) < MIN_DOCS and sum(em.values()) < MIN_SINGLE_DOC_CHARS:
        continue
    r = {'h': h, 'notu': {str(k): v for k, v in em.items()}, 'ndocs': len(bad_docs[h]),
         'fonts': dict(fonts[h].most_common(5)), 'ex': ex.get(h), 'wrong_chars': sum(em.values()),
         'ctx': ctx[h][:6]}
    r['emit'] = max(em, key=em.get)
    g = good.get(h)
    if g:
        top = max(g, key=g.get)
        if g[top] >= 0.8 * sum(g.values()) and g[top] >= 3 and len(good_docs[h][top]) >= 2:
            r['label'], r['src'] = top, 'consensus'
    lab, n, m = dictionary_label(ctx[h])
    if lab:
        r['dict'] = f'{lab} {n}/{m}'
        if 'label' not in r:
            r['label'], r['src'] = lab, 'dictionary'
    cands.append(r)
byfile = collections.defaultdict(list)
for r in cands:
    if r['ex']:
        byfile[r['ex'][0]].append(r)
for fn, rs in byfile.items():
    try:
        doc = pymupdf.open(f'{CORPUS}/{fn}')
    except Exception:
        continue
    faces = {}
    for r in rs:
        _, x, gid = r['ex']
        try:
            if x not in faces:
                faces[x] = freetype.Face(io.BytesIO(doc.extract_font(x)[3]))
            f = faces[x]
            r['gname'] = f.get_glyph_name(gid).decode('latin-1') if f.has_glyph_names else ''
            # Glyphs the use-glyph-name-for-garbage option already repairs need no entry.
            kind = kinds[r['h']].most_common(1)[0][0]
            if name_repair(r['gname'], r['emit'], kind) is not None:
                r['name_repaired'] = True
                continue
            res = render_face_glyph(f, gid)
        except Exception:
            res = None
        if res is None:
            r['blank'] = True
            continue
        d = (1 - rv @ res[0]) + 0.5 * (np.abs(rp - res[1]) @ W)
        best = np.full(len(uchars), 9, dtype=np.float32); np.minimum.at(best, rci, d.astype(np.float32))
        k = int(np.argmin(best)); r['cls'] = uchars[k]; r['cls_d'] = round(float(best[k]), 3)
        if 'label' not in r:
            r['label'], r['src'] = r['cls'], 'classifier'
cands = [r for r in cands if 'label' in r and not r.get('name_repaired')]


def in_scope(r):
    """U+FFFD and controls are only repaired to symbols and ligatures. A single
    letter or digit there belongs to unmapped-text-layer recovery and OCR:
    repairing part of such a layer would hide it from their detection."""
    if isinstance(r['emit'], int) and 0xE000 <= r['emit'] <= 0xF8FF:
        return True
    lab = r['label']
    return len(lab) > 1 or not lab.isalnum() or 'dict' in r


cands = [r for r in cands if in_scope(r)]
cands.sort(key=lambda r: -r['wrong_chars'])
with open(out, 'w') as w:
    for r in cands:
        w.write(json.dumps(r, ensure_ascii=False) + '\n')
print('skipped unmapped-layer documents', skipped)
print('candidates', len(cands), collections.Counter(r['src'] for r in cands), 'garbage chars', sum(r['wrong_chars'] for r in cands))
tot = sum(r['wrong_chars'] for r in cands) or 1; c = 0
for k, r in enumerate(cands, 1):
    c += r['wrong_chars']
    if k in (50, 100, 200, 300, 500, 800): print(f'  top {k}: {c/tot:.0%}')
