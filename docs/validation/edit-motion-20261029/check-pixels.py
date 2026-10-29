from pathlib import Path
from PIL import Image, ImageChops

out = Path(__file__).resolve().parent
lines = []
for theme in ('light', 'dark'):
    for dpi in (100, 150):
        scale = dpi / 100
        before = Image.open(out / 'snapshots' / f'{theme}-{dpi}-initial.png').convert('RGB')
        middle = Image.open(out / 'snapshots' / f'{theme}-{dpi}-insert-middle.png').convert('RGB')
        prefix = (int(2 * scale), 0, int(24 * scale), before.height)
        assert ImageChops.difference(before.crop(prefix), middle.crop(prefix)).getbbox() is None, \
            f'{theme}-{dpi}: unchanged prefix moved or faded'
        lines.append(f'[PASS] {theme}-{dpi}: unchanged prefix stays pixel-identical during insertion')
(out / 'pixel-checks.log').write_text('\n'.join(lines), encoding='utf-8')
print('\n'.join(lines))
