# Convert a logo image to a 1-bit Adafruit-GFX bitmap header (dark pixels = 1)
import sys
from PIL import Image
src, dst, maxw, maxh = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
im = Image.open(src).convert("RGBA")
bg = Image.new("RGBA", im.size, (255, 255, 255, 255))
im = Image.alpha_composite(bg, im).convert("L")
bw = im.point(lambda v: 255 if v < 128 else 0)          # logo pixels -> white
box = bw.getbbox()
im = im.crop(box)
scale = min(maxw / im.width, maxh / im.height)
w, h = max(1, round(im.width * scale)), max(1, round(im.height * scale))
im = im.resize((w, h), Image.LANCZOS)
bits = [1 if im.getpixel((x, y)) < 128 else 0 for y in range(h) for x in range(w)]
bpr = (w + 7) // 8
out = []
for y in range(h):
    for bx in range(bpr):
        b = 0
        for k in range(8):
            x = bx * 8 + k
            if x < w and bits[y * w + x]:
                b |= 0x80 >> k
        out.append(b)
with open(dst, "w", newline="\n") as f:
    f.write("// Generated from %s by img2bitmap.py - 1-bit bitmap for Adafruit GFX drawBitmap()\n" % src.split("\\")[-1].split("/")[-1])
    f.write("#pragma once\n#include <Arduino.h>\n\n")
    f.write("static const int LOGO_W = %d;\nstatic const int LOGO_H = %d;\n\n" % (w, h))
    f.write("static const uint8_t LOGO_BITMAP[] PROGMEM = {\n")
    for i in range(0, len(out), 16):
        f.write("  " + ", ".join("0x%02X" % v for v in out[i:i+16]) + ",\n")
    f.write("};\n")
# ASCII preview
for y in range(0, h, 2):
    print("".join("#" if bits[y*w+x] else " " for x in range(0, w)))
print("size", w, "x", h)
