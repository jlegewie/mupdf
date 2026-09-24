"""Glyph outline hash v1 — must match the C implementation in the MuPDF fork.

FT_Load_Glyph(face, gid, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING | FT_LOAD_IGNORE_TRANSFORM),
then FNV-1a 64 over little-endian int32s:
  n_contours, contour_end[0..], n_points, (x, y, tag & 3) per point.
Empty outlines return None.
"""
import struct, freetype
FNV_OFF, FNV_PRIME, MASK = 0xcbf29ce484222325, 0x100000001b3, (1 << 64) - 1
LOAD = freetype.FT_LOAD_NO_SCALE | freetype.FT_LOAD_NO_HINTING | freetype.FT_LOAD_IGNORE_TRANSFORM
def fnv1a64(data: bytes) -> int:
    h = FNV_OFF
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK
    return h
def outline_bytes(face, gid):
    face.load_glyph(gid, LOAD)
    o = face.glyph.outline
    if not o.points: return None
    parts = [struct.pack('<i', len(o.contours))] + [struct.pack('<i', c) for c in o.contours]
    parts.append(struct.pack('<i', len(o.points)))
    for (x, y), t in zip(o.points, o.tags):
        parts.append(struct.pack('<iii', int(x), int(y), t & 3))
    return b''.join(parts)
def glyph_hash(face, gid):
    b = outline_bytes(face, gid)
    return None if b is None else '%016x' % fnv1a64(b)
