# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
"""Generate the isolated input directory for pulse_jpeg_orientation_test (Pillow)."""

import argparse
from pathlib import Path
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
args = parser.parse_args()
args.directory.mkdir(parents=True, exist_ok=False)
image = Image.new("RGB", (80, 48))
colours = [(240, 16, 16), (16, 240, 16), (16, 16, 240), (240, 240, 16)]
for y in range(48):
    for x in range(80):
        image.putpixel((x, y), colours[(y >= 24) * 2 + (x >= 40)])
image.save(args.directory / "no-exif.jpg", quality=100, subsampling=0)
image.save(args.directory / "fixture.png")
for orientation in (*range(1, 9), 0, 9, 65535):
    exif = Image.Exif()
    exif[274] = orientation
    filename = f"orientation-{orientation}.jpg" if 1 <= orientation <= 8 else f"invalid-{orientation}.jpg"
    image.save(args.directory / filename, quality=100, subsampling=0, exif=exif)
