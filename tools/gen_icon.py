"""Draw setup/echovrmusic.ico: a speaker with sound waves on a purple-to-cyan rounded square."""
import os
from PIL import Image, ImageDraw

S = 1024


def make():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # gradient square
    grad = Image.new("RGBA", (S, S))
    gp = grad.load()
    for y in range(S):
        for x in range(S):
            t = (x + y) / (2 * S)
            gp[x, y] = (int(124 + (0 - 124) * t), int(92 + (200 - 92) * t), 255, 255)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((40, 40, S - 40, S - 40), radius=220, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)
    # speaker
    d.rectangle((230, 410, 360, 614), fill=white)
    d.polygon([(360, 410), (520, 270), (520, 754), (360, 614)], fill=white)
    # waves
    for i, r in enumerate((150, 250, 350)):
        box = (520 - r, 512 - r, 520 + r, 512 + r)
        d.arc(box, start=-48, end=48, fill=white, width=56 - i * 6)
    return img


def main():
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "setup", "echovrmusic.ico")
    make().save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("wrote", out)


if __name__ == "__main__":
    main()
