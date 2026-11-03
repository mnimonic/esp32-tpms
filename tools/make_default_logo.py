# Draws the neutral default splash logo (TPMS warning symbol: tyre section with "!")
# and writes it as logo.h via img2bitmap.py.  Usage:  python tools/make_default_logo.py
import os, subprocess, sys
from PIL import Image, ImageDraw

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.dirname(here)
W, H = 240, 200                       # drawn large, scaled down by img2bitmap
img = Image.new("L", (W, H), 255)     # white background, black symbol
d = ImageDraw.Draw(img)

# Tyre cross-section: thick arc, open at the top
d.arc((30, 10, 210, 190), start=-55, end=235, fill=0, width=24)
# Tread blocks along the bottom
for x in (62, 102, 142):
    d.rectangle((x, 182, x + 34, 198), fill=0)
# Exclamation mark
d.rounded_rectangle((108, 40, 132, 120), radius=10, fill=0)
d.ellipse((106, 134, 134, 162), fill=0)

png = os.path.join(here, "default_logo.png")
img.save(png)
subprocess.check_call([sys.executable, os.path.join(here, "img2bitmap.py"), png,
                       os.path.join(root, "logo.h"), "70", "58"])
