import freetype, numpy as np, io
from PIL import Image, ImageFilter
EM=128  # pixels per em
def render_face_glyph(face, gindex, upm_scale=1.0):
    """Return (shape24 float array, pos features) or None. Renders at EM px/em."""
    face.set_pixel_sizes(0, EM)
    face.load_glyph(gindex, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_NO_HINTING)
    g=face.glyph; bm=g.bitmap
    if bm.width==0 or bm.rows==0: return None
    a=np.array(bm.buffer,dtype=np.uint8).reshape(bm.rows,bm.pitch)[:, :bm.width]
    left=g.bitmap_left; top=g.bitmap_top
    ymax=top/EM; ymin=(top-bm.rows)/EM; w=bm.width/EM; h=bm.rows/EM
    adv=g.advance.x/64/EM
    # shape: pad to square keeping aspect, resize to 24
    s=max(a.shape); pad=np.zeros((s,s),dtype=np.uint8)
    oy=(s-a.shape[0])//2; ox=(s-a.shape[1])//2
    pad[oy:oy+a.shape[0], ox:ox+a.shape[1]]=a
    im=Image.fromarray(pad).resize((24,24),Image.BILINEAR).filter(ImageFilter.GaussianBlur(0.8))
    v=np.asarray(im,dtype=np.float32).ravel(); n=np.linalg.norm(v)
    if n==0: return None
    return v/n, np.array([ymin,ymax,w,h],dtype=np.float32)
