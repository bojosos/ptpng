"""Check compressor seed coverage and persistent corpus integration."""
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import struct
import zlib

from prepare_fuzz_corpus import (DEFLATE_LIMIT, deflate_seeds,
                                 fragmented_stored_seeds, idat_stream)


class DeflateCorpusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.seeds = dict(deflate_seeds())

    def test_deterministic_and_bounded(self):
        self.assertEqual(self.seeds, dict(deflate_seeds()))
        self.assertTrue(all(len(raw) <= DEFLATE_LIMIT for raw in self.seeds.values()))
        self.assertEqual(self.seeds["zeros-0"], b"")
        self.assertEqual(len(self.seeds["noise-two-blocks"]), DEFLATE_LIMIT)
        self.assertEqual(len(self.seeds["block-plus-one"]), (1 << 20) + 1)

    def test_distances_and_maximum_match(self):
        for distance in (32767, 32768, 32769, 65535, 65536, 65537):
            raw = self.seeds[f"distance-{distance}"]
            self.assertEqual(raw[:258], raw[distance:distance + 258])
            self.assertEqual(len(raw), distance + 258 + 4)
        raw = self.seeds["max-match-258"]
        self.assertEqual(raw, raw[:258] * 513)
        raw = self.seeds["history-wraps"]
        self.assertEqual(raw, raw[:32768] * 5 + raw[:259])

    def test_entropy_distributions(self):
        self.assertEqual(set(self.seeds["alphabet4"]), set(range(4)))
        skew = self.seeds["entropy-skew"]
        self.assertGreater(skew.count(0), len(skew) * 0.9)
        self.assertEqual(len(set(skew)), 256)
        changing = self.seeds["entropy-changes"]
        self.assertEqual(changing[:1 << 18], bytes(1 << 18))
        self.assertEqual(len(set(changing[1 << 18:2 << 18])), 256)
        self.assertEqual(set(changing[2 << 18:3 << 18]), set(range(4)))

    def test_fragmented_stored_pixels(self):
        seeds = list(fragmented_stored_seeds())
        self.assertEqual(len(seeds), 7)
        for png in seeds:
            width, height = struct.unpack_from(">II", png, 16)
            expected = (b"\0" + bytes(i % 251 for i in range(width))) * height
            self.assertEqual(zlib.decompress(idat_stream(png)), expected)
            self.assertGreater(png.count(b"IDAT"), 3)
            self.assertIn(b"\0\0\0\0IDAT", png)

    def test_cli_adds_four_corpora_and_preserves_discoveries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            png_root = root / "png"
            png_root.mkdir()
            output = root / "corpus"
            deflate = output / "deflate"
            deflate.mkdir(parents=True)
            discovery = deflate / "existing-discovery"
            discovery.write_bytes(b"keep me")
            subprocess.run([sys.executable, str(Path(__file__).with_name("prepare_fuzz_corpus.py")),
                            "--output", str(output), "--png-root", str(png_root)], check=True,
                           capture_output=True, text=True)
            self.assertEqual(discovery.read_bytes(), b"keep me")
            for name in ("decode", "inflate", "encode", "deflate"):
                self.assertTrue(any((output / name).iterdir()), name)
            for raw in self.seeds.values():
                self.assertEqual((deflate / hashlib.sha256(raw).hexdigest()).read_bytes(), raw)
            for png in fragmented_stored_seeds():
                self.assertEqual((output / "decode" / hashlib.sha256(png).hexdigest()).read_bytes(), png)


if __name__ == "__main__":
    unittest.main()
