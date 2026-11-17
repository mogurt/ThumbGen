"""Regenerate checked-in app icons from the approved PNG (requires Pillow).

Run manually after changing the artwork; product builds do not need Pillow.
"""
from pathlib import Path

from PIL import Image


def main():
    repo = Path(__file__).resolve().parents[1]
    source = repo / 'docs/assets/branding/thumbgen-icon-concept-v1.png'
    destination = repo / 'resources/icons'
    sizes = (16, 24, 32, 48, 64, 128, 256, 512)
    with Image.open(source) as original:
        image = original.convert('RGBA')
        if image.width != image.height:
            raise ValueError('Application icon artwork must be square')
        destination.mkdir(parents=True, exist_ok=True)
        for size in sizes:
            folder = destination / f'{size}x{size}'
            folder.mkdir(parents=True, exist_ok=True)
            image.resize((size, size), Image.Resampling.LANCZOS).save(folder / 'thumbgen.png')
        image.save(destination / 'thumbgen.ico', sizes=[(s, s) for s in sizes if s <= 256])
    print('Generated Windows ICO and Linux PNG icons in', destination)


if __name__ == '__main__':
    main()
