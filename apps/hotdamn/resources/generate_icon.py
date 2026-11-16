# Generates hotdamn.ico (exe + file-association icon) and hotdamn.png (window
# icon) for Hot DAMN NFO Viewer. Requires Pillow: python generate_icon.py
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
SIZE = 1024


def lerp(a, b, t):
	return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


def render():
	top, bottom = (255, 176, 32), (214, 40, 24)
	gradient = Image.new("RGB", (SIZE, SIZE))
	draw = ImageDraw.Draw(gradient)
	for y in range(SIZE):
		draw.line([(0, y), (SIZE, y)], fill=lerp(top, bottom, y / (SIZE - 1)))

	mask = Image.new("L", (SIZE, SIZE), 0)
	ImageDraw.Draw(mask).rounded_rectangle([40, 40, SIZE - 40, SIZE - 40], radius=190, fill=255)
	icon = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
	icon.paste(gradient, (0, 0), mask)
	draw = ImageDraw.Draw(icon)

	# Dark "screen" panel holding the text.
	draw.rounded_rectangle([130, 200, SIZE - 130, 640], radius=60, fill=(24, 20, 22, 255))
	try:
		font = ImageFont.truetype("consolab.ttf", 300)
	except OSError:
		font = ImageFont.load_default()
	box = draw.textbbox((0, 0), "NFO", font=font)
	draw.text(((SIZE - (box[2] - box[0])) / 2 - box[0], 420 - (box[3] - box[1]) / 2 - box[1]),
	          "NFO", font=font, fill=(255, 214, 120, 255))

	# CP437 shade ramp (light, medium, dark, full) as plain blocks.
	x, y, w, gap = 150, 720, 160, 28
	for i, alpha in enumerate((70, 130, 195, 255)):
		left = x + i * (w + gap)
		draw.rectangle([left, y, left + w, y + w], fill=(24, 20, 22, alpha))
	return icon


icon = render()
icon.resize((256, 256), Image.LANCZOS).save(os.path.join(HERE, "hotdamn.png"))
icon.save(os.path.join(HERE, "hotdamn.ico"),
          sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
