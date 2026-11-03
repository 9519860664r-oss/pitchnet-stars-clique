"""Tracks-list toolbar icon: three stacked track lanes, each a small header
chip and a lane bar, in the 4-state sprite format of the other toolbar icons
(34x34 per state, stacked: normal, hover, on, on+hover)."""
import sys
from PIL import Image, ImageDraw

STATES = [(154, 154, 154), (255, 255, 255), (255, 86, 0), (249, 133, 93)]
SIZE, SS = 34, 8  # frame size, supersampling


def frame(colour):
    big = SIZE * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = colour + (255,)
    s = SS
    rows = [9, 17, 25]          # lane centres (34 px frame)
    h = 5.2                      # lane height
    for i, y in enumerate(rows):
        top, bottom = (y - h / 2) * s, (y + h / 2) * s
        # header chip
        d.rounded_rectangle((3 * s, top, 9 * s, bottom), radius=1.6 * s, fill=c)
        # lane: middle one shorter, like clips of different length
        right = 31 if i != 1 else 25
        d.rounded_rectangle((11.5 * s, top, right * s, bottom), radius=2.6 * s, fill=c)
    return img.resize((SIZE, SIZE), Image.LANCZOS)


sheet = Image.new("RGBA", (SIZE, SIZE * 4), (0, 0, 0, 0))
for i, col in enumerate(STATES):
    sheet.alpha_composite(frame(col), (0, i * SIZE))
sheet.save(sys.argv[1])

preview = Image.new("RGBA", (SIZE * 4 * 4, SIZE * 4), (24, 24, 24, 255))
for i in range(4):
    f = sheet.crop((0, i * SIZE, SIZE, (i + 1) * SIZE)).resize((SIZE * 4, SIZE * 4), Image.NEAREST)
    preview.alpha_composite(f, (i * SIZE * 4, 0))
preview.save(sys.argv[2])
