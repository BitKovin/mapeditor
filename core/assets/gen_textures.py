"""Generates small procedural demo textures under textures/dev/ (PNG) plus one TGA, so texture
loading, the extension lookup order and the TGA decoder all have something real to chew on.
    python3 gen_textures.py        (needs Pillow)"""
import os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "textures", "dev")
os.makedirs(OUT, exist_ok=True)
SIZE = 128

def base(color, line, name, bricks=False):
    im = Image.new("RGB", (SIZE, SIZE), color)
    d = ImageDraw.Draw(im)
    if bricks:
        for row in range(0, SIZE, 32):
            d.line([(0, row), (SIZE, row)], fill=line, width=2)
            off = 0 if (row // 32) % 2 == 0 else 32
            for x in range(off, SIZE + 32, 64):
                d.line([(x, row), (x, row + 32)], fill=line, width=2)
    else:
        for i in range(0, SIZE, 32):
            d.line([(i, 0), (i, SIZE)], fill=line, width=1)
            d.line([(0, i), (SIZE, i)], fill=line, width=1)
    d.rectangle([0, 0, SIZE - 1, SIZE - 1], outline=line, width=2)
    d.polygon([(6, 6), (26, 6), (6, 26)], fill=line)  # corner marker: makes rotation/mirroring visible
    im.save(os.path.join(OUT, name))

base((150, 150, 150), (90, 90, 90), "grey.png")
base((120, 160, 110), (70, 100, 65), "floor.png")
base((150, 110, 100), (95, 60, 55), "wall.png", bricks=True)
base((165, 120, 70), (105, 70, 35), "wood.png")
base((120, 120, 128), (75, 75, 85), "concrete.png")
base((110, 125, 150), (60, 75, 100), "metal.png")

# TGA (uncompressed 24-bit, bottom-left origin) -- the format Quake 3 maps actually use.
im = Image.new("RGB", (64, 64), (230, 200, 60)); d = ImageDraw.Draw(im)
d.rectangle([0, 0, 63, 63], outline=(120, 90, 0), width=3); d.polygon([(4, 4), (24, 4), (4, 24)], fill=(120, 90, 0))
im.save(os.path.join(OUT, "tga_test.tga"))
print("wrote", sorted(os.listdir(OUT)))
