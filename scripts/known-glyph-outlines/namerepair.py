"""Python mirror of fz_glyph_name_repair_unicode (known-glyph-outlines.c), used by
the proposer to leave out glyphs the stext option use-glyph-name-for-garbage
already repairs. Keep the two in sync."""
import os, re

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')


def _load():
    src = open(os.path.join(ROOT, 'source/fitz/glyphlist.h')).read()
    blk = lambda n: re.search(r'%s\[\] = \{(.*?)\};' % n, src, re.S).group(1)
    names = re.findall(r'"([^"]*)"', blk('single_name_list'))
    codes = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', blk('single_code_list'))]
    tab = open(os.path.join(ROOT, 'source/fitz/adobe-private-use-table.h')).read()
    cus = {int(a, 16): int(b, 16) for a, b in re.findall(r'\{ 0x([0-9a-f]+), 0x([0-9a-f]+) \}', tab)}
    return dict(zip(names, codes)), cus


AGL, CUS = _load()


def garbage(u):
    return u == 0xFFFD or u < 0x20 or 0x7F <= u < 0xA0 or 0xE000 <= u <= 0xF8FF


def _component(s):
    if s in AGL:
        return AGL[s], True
    m = re.fullmatch(r'uni([0-9A-F]{4})', s) or re.fullmatch(r'u([0-9A-F]{4,6})', s)
    if not m:
        return 0, False
    u = int(m.group(1), 16)
    return (0 if 0xD800 <= u <= 0xDFFF or u > 0x10FFFF else u), False


def repair(name, current, kind):
    """Characters the fork emits for a garbage value, or None. kind: 0 simple
    font without ToUnicode, 1 with ToUnicode, 2 CID font."""
    if not isinstance(current, int) or not garbage(current) or kind == 2 or not name:
        return None
    suffixed = '.' in name
    name = name.split('.', 1)[0]
    if not name:
        return None
    parts = name.split('_')
    out, variant = [], False
    for i, p in enumerate(parts):
        u, from_list = _component(p)
        if kind == 1 and 0xE000 <= current <= 0xF8FF and (u != current or len(parts) > 1):
            return None
        if 0xE000 <= u <= 0xF8FF:
            if not from_list:
                return None
            u = CUS.get(u, 0)
            variant = True
        if not u or garbage(u) or len(out) == 8:
            return None
        out.append(chr(u))
    if len(out) > 1:
        return ''.join(out) if all(c.isalpha() for c in out) else None
    c = out[0]
    if c.isascii() and c.isalnum() and not suffixed and not variant:
        return None
    return c
