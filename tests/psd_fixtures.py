"""Synthetic PSD v1 fixtures and independent lossless image I/O for CLI tests."""
import random
import struct
import zlib


RAW, RLE, ZIP, PREDICTED_ZIP = range(4)
WIDTH, HEIGHT = 259, 11
LAYER_RGB = (19, 91, 217)


def padded(data, alignment):
    return data + bytes(-len(data) % alignment)


def section(data):
    return struct.pack(">I", len(data)) + data


def resource(number, data, name=b""):
    return (struct.pack(">4sH", b"8BIM", number)
            + padded(bytes((len(name),)) + name, 2) + padded(section(data), 2))


def version_info(has_merged):
    names = b"".join(struct.pack(">I", len(name)) + name.encode("utf-16-be")
                     for name in ("Fixture", "Fixture"))
    return resource(1057, struct.pack(">IB", 1, has_merged) + names + struct.pack(">I", 1))


def auxiliary_channel(spot):
    # DisplayInfo v1, RGB swatch, 100% opacity, spot (2) or saved alpha (0).
    return (resource(1006, b"\x04Spot" if spot else b"\x05Saved")
            + resource(1077, struct.pack(">I6HB", 1, 0, 65535, 0, 0, 0, 100, 2 if spot else 0)))


def samples(values, depth, endian=">"):
    return bytes(values) if depth == 8 else struct.pack(f"{endian}{len(values)}H", *values)


def layer_info(depth, count, tagged=False):
    # A 2x2 layer with RGB distinct from the merged RGB detects layer/composite mixups.
    channels = [b"\0\0" + samples([v * (257 if depth == 16 else 1)] * 4, depth) for v in LAYER_RGB]
    record = struct.pack(">4iH", 0, 0, 2, 2, len(channels))
    record += b"".join(struct.pack(">hI", i, len(data)) for i, data in enumerate(channels))
    record += b"8BIMnorm\xff\0\0\0" + section(bytes(8) + padded(b"\x05Layer", 4))
    info = padded(struct.pack(">h", count) + record + b"".join(channels), 2)
    if tagged:
        return bytes(8) + b"8BIMLr16" + padded(section(info), 4)
    return section(info) + bytes(4)


def prefix(width, height, depth, channels, resources=b"", layers=b""):
    # Adobe Photoshop File Formats Specification, Image Data Section.
    # https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
    # PSD fields, 16-bit samples and PackBits row lengths are big-endian.
    header = struct.pack(">4sH6xHIIHH", b"8BPS", 1, channels, height, width, depth, 3)
    return header + section(b"") + section(resources) + section(layers)


def packbits(row):
    """Pack sequences of three or more repeated bytes and cap packets at 128."""
    output, literals = bytearray(), bytearray()

    def flush():
        if literals:
            output.append(len(literals) - 1)
            output.extend(literals)
            literals.clear()

    pos = 0
    while pos < len(row):
        end = pos + 1
        while end < min(len(row), pos + 128) and row[end] == row[pos]:
            end += 1
        if end - pos >= 3:
            flush()
            output.extend((257 - (end - pos), row[pos]))
            pos = end
        else:
            literals.append(row[pos])
            pos += 1
            if len(literals) == 128:
                flush()
    flush()
    return bytes(output)


def predict(row, depth):
    # The predictor resets at each row. At 16 bits the delta is a whole word.
    mask = (1 << depth) - 1
    return samples([row[0]] + [(current - last) & mask for last, current in zip(row, row[1:])], depth)


def composite(planes, height, depth, compression):
    rows = [row for plane in planes for row in plane]
    assert len(rows) == len(planes) * height
    if compression == RLE:
        encoded = [packbits(samples(row, depth)) for row in rows]
        payload = struct.pack(f">{len(encoded)}H", *map(len, encoded)) + b"".join(encoded)
    else:
        encode = predict if compression == PREDICTED_ZIP else samples
        payload = b"".join(encode(row, depth) for row in rows)
        if compression != RAW:
            payload = zlib.compress(payload)
    return struct.pack(">H", compression) + payload


def pixels(depth, alpha=False):
    """Straight RGB(A) with repeated bytes, wrapped deltas and transparent edges."""
    maximum = (1 << depth) - 1
    scale = maximum // 255
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if x < 128:
                rgb = (0, maximum, 0) if y % 2 else (maximum, 0, maximum)
            else:
                # For 16-bit cases, low bytes deliberately differ from high bytes.
                rgb = tuple(((x * dx + y * dy) * scale + y * 31 + c * 79) & maximum
                            for c, (dx, dy) in enumerate(((27, 3), (83, 41), (9, 13))))
            if alpha:
                coverage = (0, 64, 128, 179, 255)[min(x, WIDTH - 1 - x, 4)] * scale
                if 12 < x < WIDTH - 12:
                    coverage = (64, 255, 128, 179, 255)[(x + y) % 5] * scale if x >= 128 else maximum
                yield (*rgb, coverage)
            else:
                yield rgb


def small_psd(depth=8, compression=RAW, channels="rgb", icc=None, layers=b"", resources=b""):
    alpha = channels == "rgba"
    maximum = (1 << depth) - 1
    pixels_on_disk = list(pixels(depth, alpha))
    if alpha:
        # The saved merged RGB is white-matted. TIFF and the numeric oracle
        # below use straight RGB and can catch failure to remove that matte.
        pixels_on_disk = [tuple(maximum - a + (v * a + maximum // 2) // maximum for v in rgb) + (a,)
                          for *rgb, a in pixels_on_disk]
        layers = layer_info(depth, -1, tagged=depth == 16)
    elif channels in ("spot", "aux"):
        pixels_on_disk = [(*rgb, (i * 59) & maximum) for i, rgb in enumerate(pixels_on_disk)]
        resources += auxiliary_channel(channels == "spot")
        layers = layer_info(depth, 1, tagged=depth == 16)
    channels = len(pixels_on_disk[0])
    if icc is not None:
        resources += resource(1039, icc, name=b"ICC name")
    planes = [[plane[y:y + WIDTH] for y in range(0, len(plane), WIDTH)] for plane in zip(*pixels_on_disk)]
    return prefix(WIDTH, HEIGHT, depth, channels, resources, layers) + composite(planes, HEIGHT, depth, compression)


def srgb_decode(value):
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def srgb_encode(value):
    return 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055


def rgb_profile(gamma=None):
    """D50-adapted Rec.709 colorants with an sRGB TRC or a synthetic power TRC."""
    def xyz(values):
        return b"XYZ " + bytes(4) + struct.pack(">3i", *(round(v * 65536) for v in values))

    name = b"sRGB\0" if gamma is None else b"Synthetic RGB calibration\0"
    white = xyz((0.9642, 1, 0.8249))
    curve = b"curv" + bytes(4)
    if gamma is None:
        curve += struct.pack(">I1024H", 1024, *(round(65535 * srgb_decode(i / 1023)) for i in range(1024)))
    else:
        curve += struct.pack(">IH", 1, round(gamma * 256))
    tags = [(b"desc", b"desc" + bytes(4) + section(name) + bytes(78)),
            (b"cprt", b"text" + bytes(4) + b"Synthetic fixture\0"),
            (b"wtpt", white),
            (b"rXYZ", xyz((0.4360747, 0.2225045, 0.0139322))),
            (b"gXYZ", xyz((0.3850649, 0.7168786, 0.0971045))),
            (b"bXYZ", xyz((0.1430804, 0.0606169, 0.7141733)))]
    body, table = bytearray(), bytearray(struct.pack(">I", len(tags) + 3))
    start = 132 + 12 * (len(tags) + 3)
    for tag, data in tags:
        table += struct.pack(">4sII", tag, start + len(body), len(data))
        body += padded(data, 4)
    for tag in (b"rTRC", b"gTRC", b"bTRC"):
        table += struct.pack(">4sII", tag, start + len(body), len(curve))
    body += padded(curve, 4)
    header = bytearray(128)
    struct.pack_into(">I", header, 0, 128 + len(table) + len(body))
    header[8:24] = b"\x02\x10\0\0mntrRGB XYZ "
    struct.pack_into(">6H", header, 24, 2026, 1, 1, 0, 0, 0)
    header[36:40] = b"acsp"
    header[68:80] = white[8:]
    return bytes(header + table + body)


def write_tiff(path, width, height, depth, values, endian, icc=None):
    """Classic TIFF, one uncompressed strip of chunky, unassociated samples."""
    values = list(values)
    channels = len(values[0])
    data = bytearray(b"II" if endian == "<" else b"MM")
    data += struct.pack(endian + "HI", 42, 0)
    payload = samples([sample for pixel in values for sample in pixel], depth, endian)
    data += padded(payload, 2)
    tags = [(256, 4, [width]), (257, 4, [height]), (258, 3, [depth] * channels),
            (259, 3, [1]), (262, 3, [2]), (273, 4, [8]), (277, 3, [channels]),
            (278, 4, [height]), (279, 4, [len(payload)]), (284, 3, [1])]
    if channels == 4:
        tags.append((338, 3, [2]))
    if icc is not None:
        tags.append((34675, 7, icc))
    entries = []
    for tag, kind, items in sorted(tags):
        value = items if kind == 7 else struct.pack(endian + {3: "H", 4: "I"}[kind] * len(items), *items)
        if len(value) > 4:
            offset = len(data)
            data += padded(value, 2)
            value = struct.pack(endian + "I", offset)
        entries.append(struct.pack(endian + "HHI", tag, kind, len(items)) + value.ljust(4, b"\0"))
    struct.pack_into(endian + "I", data, 4, len(data))
    path.write_bytes(data + struct.pack(endian + "H", len(entries)) + b"".join(entries) + bytes(4))


def read_png(path):
    """Decode ThumbGen's 8-bit noninterlaced RGB(A) PNG and check chunk CRCs."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    offset, compressed = 8, bytearray()
    while offset < len(data):
        length, kind = struct.unpack_from(">I4s", data, offset)
        tag = data[offset + 4:offset + 8 + length]
        assert zlib.crc32(tag) == struct.unpack_from(">I", data, offset + 8 + length)[0], path
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, compression, filtering, interlace = struct.unpack(">IIBBBBB", tag[4:])
        elif kind == b"IDAT":
            compressed += tag[4:]
        elif kind == b"IEND":
            assert offset == len(data)
            break
    assert kind == b"IEND" and depth == 8 and colour in (2, 6) and (compression, filtering, interlace) == (0, 0, 0)
    bands = 3 if colour == 2 else 4
    raw = zlib.decompress(compressed)
    stride = width * bands
    assert len(raw) == height * (stride + 1)
    previous, output = bytearray(stride), []
    for offset in range(0, len(raw), stride + 1):
        mode, row = raw[offset], bytearray(raw[offset + 1:offset + stride + 1])
        assert mode in range(5)
        for i in range(stride):
            left = row[i - bands] if i >= bands else 0
            up = previous[i]
            corner = previous[i - bands] if i >= bands else 0
            p = left + up - corner
            paeth = min((left, up, corner), key=lambda sample: abs(p - sample))
            row[i] = (row[i] + (0, left, up, (left + up) // 2, paeth)[mode]) & 255
        output.extend(tuple(row[i:i + bands]) for i in range(0, stride, bands))
        previous = row
    return width, height, output


def write_stress_psd(path, width, height, depth, compression, alpha):
    """Write one bounded fixture at a time, caching a few fixed-seed row patterns."""
    rng = random.Random(42021)
    maximum = (1 << depth) - 1
    channels = 4 if alpha else 3
    layers = layer_info(depth, -1, tagged=depth == 16) if alpha else b""
    # Long constant spans keep Raw/RLE and ZIP practical while still varying
    # channels, rows and columns. RGBA rows have white-matted transparent edges.
    rows = []
    encode = predict if compression == PREDICTED_ZIP else samples
    for c in range(channels):
        patterns = []
        for _ in range(4):
            values = []
            for block in range(8):
                a = (0, maximum // 2, maximum, maximum)[min(block, 7 - block, 3)] if alpha else maximum
                colour = rng.randrange(maximum + 1)
                value = a if c == 3 else maximum - a + colour * a // maximum
                values.extend([value] * ((block + 1) * width // 8 - block * width // 8))
            packed = encode(values, depth)
            patterns.append(packbits(packed) if compression == RLE else packed)
        rows.append(patterns)
    with path.open("wb") as stream:
        stream.write(prefix(width, height, depth, channels, layers=layers))
        stream.write(struct.pack(">H", compression))
        if compression == RLE:
            for plane in rows:
                stream.write(b"".join(struct.pack(">H", len(plane[y % 4])) for y in range(height)))
        compressor = zlib.compressobj(1) if compression in (ZIP, PREDICTED_ZIP) else None
        for plane in rows:
            for y in range(height):
                row = plane[y % 4]
                stream.write(compressor.compress(row) if compressor else row)
        if compressor:
            stream.write(compressor.flush())
