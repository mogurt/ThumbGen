"""Inspect encoded bytes independently of requested CLI options and image metadata."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib


LUMA_ZIGZAG = (16,11,12,14,12,10,16,14,13,14,18,17,16,19,24,40,
               26,24,22,22,24,49,35,37,29,40,58,51,61,60,57,51,56,55,64,72,92,78,64,68,
               87,69,55,56,80,109,81,87,95,98,103,104,103,62,77,113,121,112,100,120,92,101,103,99)


def inspect_encoding(path):
    path = Path(path)
    data = path.read_bytes()
    result = dict(path=str(path), sha256=hashlib.sha256(data).hexdigest(), bytes=len(data))
    if data.startswith(b'\xff\xd8'):
        position = 2
        tables = []
        while position + 4 <= len(data):
            if data[position] != 255:
                raise ValueError('Invalid JPEG marker boundary: ' + str(path))
            marker = data[position + 1]
            if marker in (0xda, 0xd9):
                break
            length = struct.unpack_from('>H', data, position + 2)[0]
            end = position + 2 + length
            if length < 2 or end > len(data):
                raise ValueError('Invalid JPEG segment: ' + str(path))
            if marker == 0xdb:
                start = position + 4
                while start < end:
                    info = data[start]
                    start += 1
                    precision, identity = info >> 4, info & 15
                    size = 64 * (precision + 1)
                    if precision > 1 or start + size > end:
                        raise ValueError('Invalid DQT bounds: ' + str(path))
                    if identity == 0:
                        if precision != 0:
                            raise ValueError('Expected eight-bit luminance DQT: ' + str(path))
                        tables.append(list(data[start:start + size]))
                    start += size
            position = end
        expected = [max(1, min(255, (value * 20 + 50) // 100)) for value in LUMA_ZIGZAG]
        if not tables or any(table != expected for table in tables):
            raise ValueError('JPEG luminance DQT differs from standard Q90: ' + str(path))
        result.update(format='jpeg', luminance_dqt=tables[0], standard_q90_luminance=True)
    elif data.startswith(b'RIFF') and data[8:12] == b'WEBP':
        if struct.unpack_from('<I', data, 4)[0] + 8 != len(data):
            raise ValueError('Invalid WebP RIFF length: ' + str(path))
        position = 12
        chunks = []
        while position + 8 <= len(data):
            tag = data[position:position + 4].decode('ascii')
            size = struct.unpack_from('<I', data, position + 4)[0]
            position += 8 + size + (size & 1)
            if position > len(data):
                raise ValueError('Invalid WebP chunk: ' + str(path))
            chunks.append(tag)
        if position != len(data) or 'VP8 ' not in chunks or 'VP8L' in chunks:
            raise ValueError('Expected lossy VP8 payload, without VP8L: ' + str(path))
        result.update(format='webp', riff_chunks=chunks, lossy_vp8=True)
    elif data.startswith(b'\x89PNG\r\n\x1a\n'):
        if data[8:16] != b'\x00\x00\x00\rIHDR' or len(data) < 33:
            raise ValueError('Invalid PNG IHDR: ' + str(path))
        width, height, depth, colour, compression, filtering, interlace = struct.unpack('>IIBBBBB', data[16:29])
        if zlib.crc32(data[12:29]) != struct.unpack_from('>I', data, 29)[0]:
            raise ValueError('PNG IHDR CRC mismatch: ' + str(path))
        if not width or not height or depth != 8 or compression != 0 or filtering != 0 or interlace not in (0, 1):
            raise ValueError('Expected valid eight-bit PNG: ' + str(path))
        result.update(format='png', bit_depth=depth, colour_type=colour, width=width, height=height)
    else:
        raise ValueError('Unsupported preview encoding: ' + str(path))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('paths', nargs='+', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        raise FileExistsError(args.report)
    records = [inspect_encoding(path) for path in args.paths]
    args.report.write_text(json.dumps(dict(checked=len(records), records=records), indent=2) + '\n')


if __name__ == '__main__':
    main()
