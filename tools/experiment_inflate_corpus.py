"""Generate independent zlib cases for the temporary paired sanitizer driver."""
from pathlib import Path
import random
import struct
import sys
import zlib

rng = random.Random(17102026)
count = valid = 0
with Path(sys.argv[1]).open("wb") as output:
    def add(stream, capacity, expected=None):
        global count, valid
        output.write(struct.pack("<III", len(stream), capacity,
            0xFFFFFFFF if expected is None else len(expected)))
        output.write(stream)
        if expected is not None:
            assert zlib.decompress(stream) == expected
            output.write(expected)
            valid += 1
        count += 1

    for size in list(range(1, 130)) + [255, 256, 257, 511, 512, 513, 1023, 1024,
            4095, 4096, 4097, 32767, 32768, 32769, 65535, 65536, 65537]:
        for mode in range(4):
            raw = (rng.randbytes(size) if mode == 0 else bytes(size) if mode == 1 else
                bytes(rng.randrange(4) for _ in range(size)) if mode == 2 else
                (bytes(range(251)) * ((size + 250) // 251))[:size])
            for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY):
                encoder = zlib.compressobj(6, zlib.DEFLATED, 15, 8, strategy)
                stream = encoder.compress(raw) + encoder.flush()
                add(stream, size, raw)
                for capacity in (max(1, size - 1), size + 1):
                    add(stream, capacity)
                prefixes = range(len(stream)) if size < 40 else sorted(set(
                    [0, 1, 2, 3, 4, len(stream) - 1, len(stream) - 2, len(stream) - 4] +
                    [rng.randrange(len(stream)) for _ in range(8)]))
                for prefix in prefixes:
                    add(stream[:prefix], size)
                for _ in range(5):
                    altered = bytearray(stream)
                    altered[rng.randrange(len(altered))] ^= 1 << rng.randrange(8)
                    add(bytes(altered), size)
                add(stream + b"\0", size)

print(count, "paired cases generated;", valid, "have independent zlib pixels")
