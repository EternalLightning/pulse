from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parent
lines = []
for theme in ('light', 'dark'):
    for dpi in (100, 150):
        scale = dpi / 100
        for frame in ('category-middle', 'category-reverse'):
            name = f'{theme}-{dpi}-{frame}.png'
            image = Image.open(root / 'snapshots' / name).convert('RGB')
            points = [(x, y) for x in range(int(20 * scale))
                      for y in range(int(45 * scale), int(600 * scale))
                      if (lambda rgb: rgb[0] < 80 and rgb[2] > rgb[0] + 40
                          and rgb[1] > rgb[0] + 20)(image.getpixel((x, y)))]
            assert points, f'{name}: no accent marker found'
            top = min(y for _, y in points)
            bottom = max(y for _, y in points)
            sample_x = max(x for x, _ in points) + int(5 * scale)
            center = (top + bottom) // 2
            fill = image.getpixel((sample_x, center))
            fill_top = fill_bottom = center
            while fill_top > 0 and image.getpixel((sample_x, fill_top - 1)) == fill:
                fill_top -= 1
            while fill_bottom + 1 < image.height and image.getpixel((sample_x, fill_bottom + 1)) == fill:
                fill_bottom += 1
            assert 0 < fill_bottom - fill_top < 50 * scale, f'{name}: marker has no selected pill beside it'
            assert top - fill_top >= 5 * scale and fill_bottom - bottom >= 5 * scale, \
                f'{name}: marker extends beyond the selected pill padding'
            lines.append(f'[PASS] {name}: accent marker stays inside moving selection')
(root / 'marker-checks.log').write_text('\n'.join(lines), encoding='utf-8')
print('\n'.join(lines))
