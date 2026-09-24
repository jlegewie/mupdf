"""Stage 2: aggregate the trace into per-outline statistics.

For every outline hash drawn by a font WITHOUT ToUnicode, collect:
  notu: emitted char -> chars, docs;   tu: char emitted by fonts WITH ToUnicode for the same outline
  fonts, exemplar (file, xref, gid) from a no-ToUnicode font.
"""
import json, sys, collections
src, out = sys.argv[1], sys.argv[2]
notu = collections.defaultdict(lambda: collections.Counter())
notu_docs = collections.defaultdict(set)
emit_docs = collections.defaultdict(lambda: collections.defaultdict(set))
fonts = collections.defaultdict(collections.Counter)
ex = {}
# pass 1: no-ToUnicode usage
for l in open(src):
    r = json.loads(l)
    for h, u, name, tu, c in r['rows']:
        if tu: continue
        notu[h][u] += c; notu_docs[h].add(r['file']); emit_docs[h][u].add(r['file']); fonts[h][name] += c
        if h not in ex and h in r['ex']: ex[h] = [r['file']] + r['ex'][h]
# pass 2: ToUnicode ground truth for the same outlines
tu = collections.defaultdict(collections.Counter); tu_docs = collections.defaultdict(set)
for l in open(src):
    r = json.loads(l)
    for h, u, name, t, c in r['rows']:
        if t and h in notu:
            tu[h][u] += c; tu_docs[h].add(r['file'])
with open(out, 'w') as w:
    for h in notu:
        w.write(json.dumps({'h': h, 'notu': notu[h], 'emit_docs': {u: len(v) for u, v in emit_docs[h].items()}, 'ndocs': len(notu_docs[h]), 'fonts': dict(fonts[h].most_common(5)),
                            'tu': tu.get(h, {}), 'tu_ndocs': len(tu_docs.get(h, ())), 'ex': ex.get(h)}) + '\n')
print('outlines', len(notu), 'with ToUnicode evidence', len(tu))
