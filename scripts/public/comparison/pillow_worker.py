"""Standard Pillow candidate; one invocation, all outputs read from original."""
import json
import math
from pathlib import Path
import sys
from PIL import Image, ImageOps


def main():
    plan = json.loads(Path(sys.argv[1]).read_text(encoding='utf-8'))
    for job in plan.get('jobs', [plan]):
        process(job)


def process(plan):
    with Image.open(plan['input']) as source:
        oriented = ImageOps.exif_transpose(source)
        try:
            # Preserve alpha; Pillow resize works in its encoded colour path.
            image = oriented.convert('RGBA')
            try:
                for box, output in zip(plan['boxes'], plan['outputs']):
                    scale = min(1, box[0] / image.width, box[1] / image.height)
                    size = tuple(max(1, math.floor(v * scale + .5)) for v in image.size)
                    result = image.resize(size, Image.Resampling.BILINEAR)
                    try:
                        if plan.get('format', 'png') == 'jpeg':
                            background = Image.new('RGB', result.size, (0, 0, 0))
                            try:
                                background.paste(result, mask=result.getchannel('A'))
                                background.save(output, 'JPEG', quality=90)
                            finally:
                                background.close()
                        else:
                            result.save(output, 'PNG', compress_level=3)
                    finally:
                        result.close()
            finally:
                image.close()
        finally:
            oriented.close()


if __name__ == '__main__':
    main()
