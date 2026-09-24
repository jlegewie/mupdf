import freetype, numpy as np, unicodedata
from render_lib import render_face_glyph
F='/System/Library/Fonts/'; S=F+'Supplemental/'
fonts=[(F+'Times.ttc',i) for i in range(4)]+[(F+'Helvetica.ttc',i) for i in range(4)]+[(S+'Arial.ttf',0),(S+'Arial Italic.ttf',0),(S+'Georgia.ttf',0),(S+'Georgia Italic.ttf',0),(F+'Symbol.ttf',0),(S+'STIXGeneral.otf',0),(S+'STIXGeneralItalic.otf',0),(S+'STIXGeneralBol.otf',0),(S+'Arial Unicode.ttf',0),(F+'Apple Symbols.ttf',0)]
cands=set(range(0x21,0x7f))|set(range(0xa1,0x100))|set(range(0x391,0x3aa))|set(range(0x3b1,0x3ca))|{0x3d1,0x3d5,0x3d6,0x3f5,0x3c2}
cands|=set(map(ord,"′″‴•…–—‰†‡‹›€™←↑→↓↔↕⇐⇑⇒⇓⇔∀∂∃∅∆∇∈∉∋∏∑−∓∗∘∙√∝∞∠∥∧∨∩∪∫∮∴∵∼≃≅≈≠≡≤≥≪≫⊂⊃⊆⊇⊕⊗⊥⋅⌀□■▲△▼▽◆◇○●★☆♀♂✓✔✗✕⩽⩾ℓℏℜℑ℘ℵ°±×÷·¬µ⁻⁺"))
cands.discard(0xad); cands.discard(0xa0)
vecs=[];pos=[];chars=[]
for path,idx in fonts:
    face=freetype.Face(path,idx)
    for c in sorted(cands):
        gi=face.get_char_index(c)
        if not gi: continue
        r=render_face_glyph(face,gi)
        if r is None: continue
        vecs.append(r[0]); pos.append(r[1]); chars.append(chr(c))
np.savez('refs.npz',vecs=np.array(vecs),pos=np.array(pos),chars=np.array(chars))
print(len(chars))
