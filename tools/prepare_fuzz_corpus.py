"""Build persistent seed directories for the four libFuzzer targets.

Usage: python tools/prepare_fuzz_corpus.py --output build/fuzz-corpus
Additional --png-root paths replace the default tests/gen and third_party roots.
No third-party Python dependencies. Existing coverage discoveries are preserved.
"""
import argparse
import hashlib
from pathlib import Path
import random
import struct
import zlib


LIMIT = 1 << 20
DEFLATE_LIMIT = 2 << 20


def save(directory, data):
    (directory / hashlib.sha256(data).hexdigest()).write_bytes(data)


def idat_stream(data):
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return None
    pos, parts = 8, []
    while pos + 12 <= len(data):
        size = struct.unpack_from(">I", data, pos)[0]
        if size > len(data) - pos - 12:
            return None
        chunk = data[pos + 4:pos + 8]
        if chunk == b"IDAT":
            parts.append(data[pos + 8:pos + 8 + size])
        pos += size + 12
        if chunk == b"IEND":
            break
    return b"".join(parts) if parts else None


def save_inflate(directory, stream):
    if len(stream) + 4 > LIMIT:
        return
    try:
        decoder = zlib.decompressobj()
        raw = decoder.decompress(stream, LIMIT + 1)
    except zlib.error:
        return
    if len(raw) > LIMIT or not decoder.eof:
        return
    # Exact output capacity exercises normal decode and SIMD buffer ends.
    # Empty streams use capacity 1; the dynamic decoder can still succeed.
    save(directory, struct.pack("<I", max(1, len(raw))) + stream)


def png_chunk(kind, payload):
    return (struct.pack(">I", len(payload)) + kind + payload +
            struct.pack(">I", zlib.crc32(kind + payload)))


def gray_icc_profile():
    """Small ICC v2 gray display profile with D50 white and a gamma 2.2 curve."""
    description = b"Fuzz gray profile\0"
    white = struct.pack(">3I", 0xF6D6, 0x10000, 0xD32D)
    tags = (
        (b"desc", b"desc\0\0\0\0" + struct.pack(">I", len(description)) +
         description + bytes(78)),
        (b"cprt", b"text\0\0\0\0Public domain\0"),
        (b"wtpt", b"XYZ \0\0\0\0" + white),
        (b"kTRC", b"curv\0\0\0\0" + struct.pack(">IH", 1, 563)),
    )
    profile = bytearray(128 + 4 + 12 * len(tags))
    struct.pack_into(">4sI4s4s4s6H4s", profile, 4, b"ptpn", 0x02100000,
                     b"mntr", b"GRAY", b"XYZ ", 2026, 9, 25, 0, 0, 0, b"acsp")
    profile[68:80] = white
    struct.pack_into(">I", profile, 128, len(tags))
    for index, (kind, payload) in enumerate(tags):
        struct.pack_into(">4sII", profile, 132 + index * 12,
                         kind, len(profile), len(payload))
        profile.extend(payload)
        profile.extend(bytes((-len(profile)) % 4))
    struct.pack_into(">I", profile, 0, len(profile))
    return bytes(profile)


def metadata_seeds():
    """Exercise accepted metadata and the 64-allocation retention boundary."""
    def image(chunks, palette=False):
        header = struct.pack(">IIBBBBB", 1, 1, 8, 3 if palette else 0, 0, 0, 0)
        return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + chunks +
                png_chunk(b"IDAT", zlib.compress(b"\0\1")) + png_chunk(b"IEND", b""))

    splt8 = png_chunk(b"sPLT", b"eight\0\x08" + struct.pack(">4BH", 1, 2, 3, 4, 9))
    splt16 = png_chunk(b"sPLT", b"sixteen\0\x10" + struct.pack(">5H", 1, 2, 3, 4, 9))
    profile = png_chunk(b"iCCP", b"gray\0\0" + zlib.compress(gray_icc_profile()))
    compressed_text = png_chunk(b"zTXt", b"short\0\0" + zlib.compress(b"hello"))
    # 5001 bytes forces metadata inflate to grow beyond its initial 4096-byte buffer.
    itext = png_chunk(b"iTXt", b"long\0\1\0en\0title\0" + zlib.compress(b"x" * 5001))
    for chunks in (splt8, splt16, profile, splt8 + splt16 + profile + compressed_text + itext):
        yield image(chunks)
    yield image(png_chunk(b"PLTE", b"\0\0\0\xff\xff\xff") +
                png_chunk(b"hIST", struct.pack(">2H", 0, 1)), palette=True)
    for count in (31, 32, 33, 80):
        chunks = b"".join(png_chunk(b"tEXt", f"key{i}\0value".encode("ascii"))
                          for i in range(count))
        yield image(chunks + splt8 + profile)
    for count in (15, 16, 17):
        chunks = b"".join(png_chunk(b"iTXt", f"key{i}\0\0\0en\0title\0body".encode("ascii"))
                          for i in range(count))
        yield image(chunks + compressed_text + splt16)


def fragmented_stored_seeds():
    """Reach direct stored decoding with split headers, payloads and checksums."""
    for width, height, steps in ((257, 1, (1, 2, 3, 7, 31)),
                                 (65534, 2, (4093, 65535))):
        raw = (b"\0" + bytes(i % 251 for i in range(width))) * height
        stream = zlib.compress(raw, 0)
        header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
        for step in steps:
            parts = [b"\x89PNG\r\n\x1a\n", png_chunk(b"IHDR", header),
                     png_chunk(b"IDAT", b"")]
            for offset in range(0, len(stream), step):
                parts.append(png_chunk(b"IDAT", stream[offset:offset + step]))
                parts.append(png_chunk(b"IDAT", b""))
            yield b"".join(parts) + png_chunk(b"IEND", b"")


def deflate_seeds():
    """Raw compressor inputs, independent of the encoder's 64 KiB pixel cap."""
    rng = random.Random(0xDEF1A7E)
    for length in (0, 1, 2, 3, 4, 15, 16, 17, 257, 258, 259,
                   32767, 32768, 32769, 65535, 65536, 65537):
        yield f"zeros-{length}", bytes(length)
    yield "alphabet4", bytes(rng.randrange(4) for _ in range(65536))
    yield "entropy-skew", bytes(rng.randrange(256) if rng.randrange(16) == 0 else 0
                                for _ in range(131072))
    yield "literal-noise", rng.randbytes(131073)
    yield "all-literals", bytes(range(256)) * 259
    # The repeated random prefix can match only at the requested distance.
    # 32769 tests eviction rather than permitting an out-of-window match.
    for distance in (32767, 32768, 32769, 65535, 65536, 65537):
        prefix = rng.randbytes(distance)
        yield f"distance-{distance}", prefix + prefix[:258] + b"tail"
    pattern = rng.randbytes(258)
    yield "max-match-258", pattern * 513
    window = rng.randbytes(32768)
    yield "history-wraps", window * 5 + window[:259]
    # Alternate token distributions in one stream and across the 1 MiB raw
    # block boundary, including an exactly full and a one-byte trailing block.
    quarter = bytes(1 << 18)
    noise = rng.randbytes(1 << 18)
    alphabet = bytes(rng.randrange(4) for _ in range(1 << 18))
    changing = quarter + noise + alphabet + bytes(range(256)) * 1024
    yield "entropy-changes", changing
    yield "block-plus-one", changing + b"x"
    yield "two-blocks", changing + noise + quarter + alphabet + noise
    yield "zeros-two-blocks", bytes(DEFLATE_LIMIT)
    yield "noise-two-blocks", rng.randbytes(DEFLATE_LIMIT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--png-root", type=Path, action="append")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parent.parent
    roots = args.png_root or [repo / "tests/gen", repo / "third_party"]
    directories = {name: args.output / name for name in ("decode", "inflate", "encode", "deflate")}
    for directory in directories.values():
        directory.mkdir(parents=True, exist_ok=True)
    for data in metadata_seeds():
        save(directories["decode"], data)
    for data in fragmented_stored_seeds():
        save(directories["decode"], data)
    for _, data in deflate_seeds():
        save(directories["deflate"], data)
    for root in roots:
        for path in sorted(root.rglob("*.png")):
            if path.stat().st_size > LIMIT:
                continue
            data = path.read_bytes()
            if not data:
                continue
            save(directories["decode"], data)
            stream = idat_stream(data)
            if stream is not None:
                save_inflate(directories["inflate"], stream)

    # Stored, fixed and dynamic blocks; short output, overlapping matches,
    # 32 KiB history boundaries, and incompressible literal runs.
    for length in (0, 1, 2, 3, 7, 15, 16, 17, 255, 258, 4095, 4096, 32768, 65536):
        random_bytes = bytearray()
        state = 0x9E3779B9
        for _ in range(length):
            state ^= (state << 13) & 0xFFFFFFFF
            state ^= state >> 17
            state ^= (state << 5) & 0xFFFFFFFF
            random_bytes.append(state & 255)
        patterns = (bytes(length), bytes(i % 251 for i in range(length)), bytes(random_bytes))
        for raw in patterns:
            for level, strategy in ((0, zlib.Z_DEFAULT_STRATEGY),
                                    (1, zlib.Z_FIXED), (6, zlib.Z_DEFAULT_STRATEGY)):
                compressor = zlib.compressobj(level, zlib.DEFLATED, 15, 8, strategy)
                stream = compressor.compress(raw) + compressor.flush()
                save_inflate(directories["inflate"], stream)

    widths = (1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65, 127, 128)
    payload = bytes((i * 73 + (i // 19) * 17) & 255 for i in range(512))
    for fmt in range(8):
        for filt in range(7):
            for index, width in enumerate(widths):
                height = (1, 2, 17, 64)[index % 4]
                padding = (0, 1, 15, 31)[index % 4]
                header = bytes((width - 1, height - 1, fmt, filt, padding,
                                index % 3, index % 4, 0))
                save(directories["encode"], header + payload)
    # Full-sized random pixels reach long literal runs without a short repeating
    # payload. RGBA16 also spans the complete DEFLATE history window.
    noise = random.Random(572).randbytes(1 << 16)
    for fmt in (3, 7):
        for filt in range(7):
            header = bytes((127, 63, fmt, filt, 0, 0, 0, 0))
            save(directories["encode"], header + noise)
    for name, directory in directories.items():
        print(f"{name}: {sum(1 for path in directory.iterdir() if path.is_file())} seeds in {directory}")


if __name__ == "__main__":
    main()
