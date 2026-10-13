# Performance work, September–October 2026

ptpng targets fast single-threaded PNG decoding on x86 AVX2 and ARM64
NEON. A kernel benchmark measures one operation; it does not establish
the speed of a complete PNG decode or a lead over every other decoder.

## 3 October nightly follow-up

[Nightly run 37111068455](https://github.com/bojosos/ptpng/actions/runs/37111068455)
tested `9a1dadf` on all five platforms. Within-run geometric mean speedups
against libpng + zlib-ng level 6 for encoding, and zlib-ng for decoding:

| Platform | Encode speedup | Faster encoder cases | Decode speedup | Faster decoder cases |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 1.486x | 6/9 | 1.791x | 27/27 |
| Windows x64 | 1.299x | 6/9 | 1.368x | 25/27 |
| Linux ARM64 | 1.094x | 3/9 | 1.573x | 27/27 |
| macOS ARM64 | 1.143x | 4/9 | 1.649x | 27/27 |
| Windows ARM64 | 1.221x | 5/9 | 1.462x | 26/27 |

RGB-photo encoding lost on every platform, at 0.670–0.929x the reference
throughput. Gray8 photos measured 0.705–0.949x, and RGBA16 photos measured
0.666–0.908x. These were the main targets. Windows noise-to-RGB8 decode
measured 0.959x on x64 and 0.978x on ARM64; Windows x64 RGB-graphics-to-RGBA8
decode measured 0.940x. Runner CPUs differ, so these ratios compare the
engines within each run, not raw times between nightlies.

Fresh local VTune software samples on the i7-1355U attributed 84.2% of
RGB-photo encoder CPU time to the long matcher. Gray8 encoding instead
spent 37.2% in short matching and 23.7% in long matching. The changes target
those loops while retaining hashes, chain limits, candidate ordering,
literal-cost estimates, filter choices and encoded bytes:

- Reject candidates with four bytes ending at the current best match's
  length before scanning their prefix. The six-byte prefix already proves
  an improvement over five, so those initial matches skip the extra probe.
- Return immediately for empty chains and compare three-byte prefixes
  together on known little-endian word-store targets. Calculate literal
  costs only for actual matches, with explicit sums for three to five bytes
  instead of a loop. Short input tails stay bounded.
- Expand eight RGB pixels per AVX2 iteration and compact RGBA to RGB with
  exact output stores. ARM Clang/MSVC use structured NEON loads and stores
  for alpha removal; GCC retains its faster auto-vectorized converter.

[Paired run 37142445201](https://github.com/bojosos/ptpng/actions/runs/37142445201)
compares `d2c3be7` with the nightly's `9a1dadf`, using nine alternating pairs
for each of 22 workloads. Timed loops run for at least 0.5 seconds. Linux
and Windows pin both versions to logical CPU 0; macOS remains OS scheduled.
The values below are median candidate/baseline throughput ratios. All 990
old/new output-size pairs match, including 405 encoder pairs.

| Platform | RGB photo encode | Gray8 photo encode | Gray16 photo encode | Noise-to-RGB8 decode |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 1.123x | 1.013x | 1.100x | 1.164x |
| Windows x64 | 1.139x | 1.056x | 1.111x | 0.988x |
| Linux ARM64 | 1.075x | 1.034x | 1.067x | 1.001x |
| macOS ARM64 | 1.158x | 1.155x | 1.112x | 1.044x |
| Windows ARM64 | 1.153x | 1.082x | 1.129x | 1.038x |

RGB-photo encoding improved in every pair on all five platforms. Gray16
improved in every pair except on Windows x64, whose range was 0.939–1.310x.
Gray8 gains are less certain on Linux and macOS; Windows ARM64's range was
1.047–1.129x. Nine-image encoder geometric means improved 2.0–4.9%.

This patch does not improve every case. RGBA16-photo encoder medians are
0.971–1.030x and retain wide ranges, so that reference gap remains. Noise
encoding measured 0.986x on Linux x64 and 0.988x on Linux ARM64. The x64
range was 0.977–1.001x, so a small slowdown remains plausible. Windows x64
RGB conversion gains did not reproduce reliably; noise-to-RGB8 pairs ranged
0.893–1.042x. Linux x64 noise-to-RGB8's 1.164x median also had outliers,
0.862–1.679x. These are observed ranges, not confidence intervals. macOS
has broad scheduling variation, including unchanged decoder controls.

The final code passed all eight jobs in
[CI run 37142293785](https://github.com/bojosos/ptpng/actions/runs/37142293785),
including Linux ASan/UBSan and all five platform correctness builds. Nine
local tests passed, including independent libpng/zlib parity. Differential
checks against `9a1dadf` verified 2,108 raw streams with independent zlib and
byte-identical PNG output for all eleven local fixtures, including two
interlaced input variants. Converter tests cover lengths 0–65 at all 32
byte alignments, exact input allocations and output guards; existing tests
retain multi-row RGB transparency coverage.

[Fuzz run 37142295688](https://github.com/bojosos/ptpng/actions/runs/37142295688)
passed 1,494,559 executions across decode, inflate, PNG encode and raw
DEFLATE on x64/ARM64, with ASan/UBSan and 60 seconds per target. This
includes 1,278 raw-compressor cases reaching inputs up to 2 MiB. These
bounded campaigns and synthetic fixtures do not prove correctness or
performance for every input.

## 3 October decoding follow-up

[Final paired run 37153416150](https://github.com/bojosos/ptpng/actions/runs/37153416150)
compares `a16ee14` with `51d5bc8`, which already includes the encoder and
initial converter improvements above. Each of 23 workloads has seven
alternating pairs with at least 0.5 seconds per timed loop. Linux and
Windows pin both versions to logical CPU 0; macOS remains OS scheduled.
All 805 old/new output-size pairs match, including 315 encoder pairs.
Values below are median candidate/baseline throughput ratios.

| Platform | RGB photo to RGBA8 | RGB graphics to RGBA8 | RGBA photo to RGB8 | Noise to RGB8 | Palette to RGB8 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Linux x64 | 1.003x | 1.117x | 0.997x | 1.006x | 1.007x |
| Windows x64 | 1.058x | 1.095x | 1.021x | 1.019x | 0.936x |
| Linux ARM64 | 1.000x | 0.997x | 0.999x | 0.999x | 1.000x |
| macOS ARM64 | 1.049x | 1.227x | 1.010x | 1.003x | 0.994x |
| Windows ARM64 | 1.049x | 1.113x | 1.033x | 1.034x | 0.999x |

Graphics expansion improved in all seven pairs on Linux x64, Windows x64
and macOS ARM64; Windows ARM64 improved in six, with one 0.756x outlier.
Windows ARM64 photo expansion ranged 1.046–1.101x and noise compaction
1.027–1.057x. Windows x64 photo and noise gains remain uncertain: their
ranges were 0.999–1.157x and 0.958–1.106x. macOS retains broad scheduling
variation, including native decoder controls.

The patch has remaining regressions. Windows x64 palette-to-RGB measured
0.936x, with six of seven pairs slower and a 0.850–1.017x range. Native
Gray16 decoding on Linux x64 measured 0.963x (0.960–0.997x), and native
RGBA16 graphics on Windows ARM64 measured 0.965x (0.962–0.968x); every pair
in those two cases was slower. Fourteen-case decoder geometric means
improved 0.8–1.6% on the enabled platforms and stayed at parity on Linux
ARM64. Those means weight each synthetic case equally, not a production
workload.

RGB8-to-RGBA8 and RGBA8-to-RGB8 output conversion previously allocated a
second pixel buffer after reconstructing the native image. Alpha removal
now compacts forward into that native allocation. For non-interlaced RGB8
expansion, the raw allocation reserves the larger output size, with both
sizes checked against the existing limit. Rows and pixels convert backward,
loading each complete AVX2/NEON input group before writing its expanded
output. Scalar conversion reads all three channels before storing and
preserves tRNS transparency. Interlaced RGB expansion retains extraction
and separate conversion; interlaced RGBA compaction reuses the extracted
native image.

RGBA-to-RGB reuse removes a temporary allocation of three bytes per pixel,
but the returned allocation retains the native capacity of approximately
four bytes per pixel. Its reported length and row stride remain three
bytes per pixel. RGB-to-RGBA expansion returns the four-byte output
allocation directly.

GCC ARM64 retains separate conversion allocations. The first nine-pair
[comparison](https://github.com/bojosos/ptpng/actions/runs/37146078240)
found no RGB-photo expansion gain, a 3.5% graphics expansion slowdown and a
2.4% RGBA-photo compaction slowdown on Linux ARM64. The same experiment
improved RGB graphics expansion by 5.2% on Linux x64, 8.1% on Windows x64,
13.0% on Windows ARM64 and 23.9% on macOS ARM64. macOS was OS scheduled
and had broad ranges, including unchanged native decoder controls.

Enabled builds align packed palette entries to four bytes, with byte and
word views of the same table. MSVC AVX2 palette-to-RGB conversion gathers
eight entries, removes alpha and writes exactly 24 bytes with two stores; its
scalar tail handles the remaining zero to seven pixels. Disabled GCC
ARM64 builds retain their original palette layout and structure size,
omit backward converter branches and retain their original cleanup path.
Enabled builds put the reverse flag in the alignment gap before the table
and also retain the original structure size. Converter regressions
cover lengths 0–65 at every byte alignment
modulo 32, same-buffer compaction and enabled backward expansion, output
guards, multi-row compaction and RGB tRNS transparency. The backward
overlap check failed before implementation and passed afterward.

A fresh local VTune software capture attributed 72.1% of noise-to-RGB8
decode CPU time to inflate and 3.8% to output conversion. Inflater
experiments did not show a reliable improvement, so the final follow-up
leaves that code unchanged. A local same-process palette comparison of
the `7da908b` experiment verified identical RGB bytes and measured a
15.6% median reduction in thread cycles per decoded image relative to
`51d5bc8`. Wall-clock pairs
had a 0.348–3.484x middle-90% range under other machine activity, so they
do not establish a reliable elapsed-time gain.

An initial all-compiler gather experiment in
[run 37152495720](https://github.com/bojosos/ptpng/actions/runs/37152495720)
regressed Linux x64 palette-to-RGB throughput to 0.561x, with all seven
pairs between 0.522 and 0.612x. Windows x64 measured 1.002x with a broad
0.711–1.160x range. The final code enables RGB gathering only for MSVC;
GCC and Clang retain their existing scalar packed-palette converter.

The final source, `a16ee14`, passed all eight jobs in
[CI run 37153322301](https://github.com/bojosos/ptpng/actions/runs/37153322301),
including independent libpng parity on all five platforms and Linux
ASan/UBSan checks. Local decoder and converter tests passed after adding
the palette batch. [Fuzz run 37153322298](https://github.com/bojosos/ptpng/actions/runs/37153322298)
passed 643,451 executions across decode, inflate, PNG encode and raw
DEFLATE on x64/ARM64 with ASan/UBSan, using 30 seconds per target. These
bounded campaigns do not establish correctness for every input.

## 1 October encoder compression

The default encoder now scores every byte in each row, using AVX2/NEON
filters and 64-bit residual scores. It selects dynamic, fixed or stored
DEFLATE blocks by bit cost, searches bounded match chains with lazy
lookahead, and accepts short matches only when they have an estimated
bit saving. It keeps the C/C++ interfaces and has no runtime dependency
on zlib. Compression state uses 896 KiB per call, plus image buffers.

The previous encoder used sampled filters, one match candidate and fixed
Huffman codes. On the same RGB fixture it wrote 9,739,624 bytes; the new
encoder writes 5,357,962, a 45.0% reduction. The graphics fixture drops
from 164,858 to 34,035 bytes, a 79.4% reduction. Pixels are unchanged.

Local Release measurements on the i7-1355U, pinned to P logical CPU 2,
use five rotated rounds after independently verified warm-up. All nine
cases are faster than stock zlib level 6. Against zlib-ng level 6,
five cases win and four lose; the sizes are at most 2.4% larger. Speed
ratios below use each reference's own run, not cross-run timings.
Positive size differences mean a larger ptpng PNG. Palette inputs
expand to RGBA8 for both encoders.

| Image | PNG bytes | Size vs zlib 6 | Size vs zlib-ng 6 | Speed vs zlib 6 | Speed vs zlib-ng 6 |
| --- | ---: | ---: | ---: | ---: | ---: |
| photo_rgb8 | 5,357,962 | -7.80% | +0.14% | 1.62x | 0.76x |
| photo_rgba8 | 11,738,487 | -8.84% | +0.87% | 4.73x | 1.09x |
| photo_gray8 | 3,131,667 | -0.12% | -6.72% | 7.84x | 0.83x |
| photo_gray16 | 3,832,479 | -1.48% | +1.83% | 1.52x | 0.87x |
| graphic_pal8 | 42,332 | +1.92% | +2.26% | 2.82x | 1.95x |
| graphic_rgb8 | 34,035 | +1.73% | +2.36% | 2.94x | 2.51x |
| photo_rgba16_paeth | 4,963,843 | -1.09% | +0.81% | 1.39x | 0.89x |
| graphic_rgba16_paeth | 9,661 | +1.51% | +1.93% | 2.90x | 1.85x |
| noise_rgba8 | 3,146,840 | -0.17% | -0.17% | 1.17x | 1.21x |

[Nightly run 36787300229](https://github.com/bojosos/ptpng/actions/runs/36787300229)
at [`835042d`](https://github.com/bojosos/ptpng/commit/835042df156c9d42f837729dfb22c4752a812141)
produced identical PNG sizes on all five platforms. Encoder speedups below
are geometric means across the nine fixtures; case counts show where ptpng
is faster. Each comparison uses medians from the same runner and run.

| Platform | vs zlib 6 | Faster cases | vs zlib-ng 6 | Faster cases |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 2.69x | 9/9 | 1.37x | 6/9 |
| Windows x64 | 2.69x | 9/9 | 1.36x | 5/9 |
| Linux ARM64 | 2.13x | 9/9 | 1.05x | 4/9 |
| macOS ARM64 | 2.59x | 8/9 | 1.12x | 3/9 |
| Windows ARM64 | 2.31x | 9/9 | 1.18x | 4/9 |

The x64/ARM64 ASan+UBSan campaigns passed 3,760,817 executions across
decode, inflate, PNG encode and raw DEFLATE, including 5,711 direct
compressor cases. These finite campaigns found no errors; they do not
prove correctness for every possible input.

[Linux profiling run 36787310317](https://github.com/bojosos/ptpng/actions/runs/36787310317)
places 84.76% of RGB-photo encoder samples in the long matcher on x64
and 86.21% on ARM64. This guided the matching and insertion changes below.
ARM64 exposed hardware counters on Neoverse-N2; x64
exposed only software task-clock sampling on EPYC 7763.

The size gap had two independently measured causes. For graphics,
stock zlib level 6 compressed the old sampled-filter data to 45,285
bytes, versus 33,331 with full-row filters. Applying the old ptpng
compressor to those same streams still took 164,801 and 156,097 bytes:
most of the remaining gap came from the match parser and fixed codes.
These raw comparisons used zlib's default strategy; the PNG reference
uses its filtered strategy. To reproduce the diagnostic:

```sh
./build/bench_encode --diagnose local diagnosis.json tests/bench/photo_rgb8.png
```

The raw compressor regression now compares flat, small-alphabet and
changing-entropy streams with zlib level 6. Boundary/random tests decode
1,528 streams with both ptpng and zlib; 628 ASan cases and 628 forced
portable-path cases passed locally. A focused 10,000-case screening
checked Huffman length limits, complete Kraft sums and prefix uniqueness.
The fourth coverage-guided fuzz target tests raw compression against
zlib on x64/ARM64, including 2 MiB inputs and entropy/block changes.

zlib-ng level 1 uses its quick strategy by default, while stock zlib
level 1 uses its fast strategy. The same level number therefore does
not imply the same parsing or file size. The benchmark builds enable
zlib-ng's new strategies; its level 6 uses medium parsing. See the
[vendored configuration](third_party/zlib-ng-2.2.4/deflate.c#L142).

### Encoder speed at unchanged sizes

Commit [`b87fc8a`](https://github.com/bojosos/ptpng/commit/b87fc8a08e0aa89956bc286c2da5744459796491)
shares one bounded word load between the three- and
six-byte insertion hashes, simplifies prefix checks, overlaps chain-link
loads, and avoids rechecking the byte already found by word mismatch
scanning. Hashes, candidate order, search limits and filter choices stay
unchanged. Local comparisons verified identical bytes for all nine PNGs
and 2,100 raw streams against `835042d`; ASan and portable-path tests passed.

[Paired run 36789896078](https://github.com/bojosos/ptpng/actions/runs/36789896078)
compares both revisions on each machine, pinned to logical CPU 0, with
nine alternating pairs per workload. Both use `-O3` with profiling symbols.
All 81 encoder size comparisons per architecture match. These ratios
measure the code change directly, rather than comparing different nightlies.

| Image | Linux x64 speedup | Linux ARM64 speedup |
| --- | ---: | ---: |
| photo_rgb8 | 1.059x | 1.086x |
| photo_rgba8 | 1.051x | 1.056x |
| photo_gray8 | 1.041x | 1.001x |
| photo_gray16 | 1.062x | 1.085x |
| graphic_pal8 | 1.363x | 1.139x |
| graphic_rgb8 | 1.359x | 1.137x |
| photo_rgba16_paeth | 1.025x | 1.022x |
| graphic_rgba16_paeth | 1.353x | 1.147x |
| noise_rgba8 | 1.003x | 0.978x |

Geometric mean encoder gains are 1.137x on EPYC 7763 and 1.071x on
Neoverse-N2. Unchanged decoder control medians range from 0.991x to
1.002x. A [focused repeat](https://github.com/bojosos/ptpng/actions/runs/36791128817)
uses 21 alternating pairs with three-second loops. ARM64 noise encoding
measures 0.988x, with 11/21 pairs below parity and a 0.944–1.055x range.
Run ordering affects the result, so a small regression remains possible.
The ARM64 decoder controls stay within 0.03% of parity. The x64 repeat measures
1.013x on a different runner CPU, EPYC 9V45. All 42 encoder size pairs
match. Eight CI jobs and another 2,796,780 x64/ARM64 fuzz executions
passed with no findings.

[Nightly run 36791257328](https://github.com/bojosos/ptpng/actions/runs/36791257328)
at [`df94a6d`](https://github.com/bojosos/ptpng/commit/df94a6d7851b5cba5d76279c4648effaa9da2c34)
passed on all five platforms and published the dashboard. All nine PNG
sizes match the preceding compression revision on every platform.
These geometric means compare ptpng with each reference within the same
run; hosted runner changes make comparisons between nightlies unreliable.

| Platform | vs zlib 6 | Faster cases | vs zlib-ng 6 | Faster cases |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 2.87x | 9/9 | 1.49x | 6/9 |
| Windows x64 | 2.98x | 9/9 | 1.52x | 6/9 |
| Linux ARM64 | 2.25x | 8/9 | 1.14x | 4/9 |
| macOS ARM64 | 2.59x | 9/9 | 1.18x | 4/9 |
| Windows ARM64 | 2.36x | 9/9 | 1.22x | 4/9 |

## 30 September benchmark snapshot

Release results from [nightly run 36753946386](https://github.com/bojosos/ptpng/actions/runs/36753946386),
commit [`86bcf29`](https://github.com/bojosos/ptpng/commit/86bcf29d098473fbe295995f5bb8a9ac1f215d98),
against libpng + zlib-ng 2.2.4 in ZLIB_COMPAT mode. The Windows x64 runner
used an AMD EPYC 7763, Windows Server 2025 and four logical CPUs, with OS
scheduling. Fixtures are generated gradients, graphics and random noise.
They are 3200x2400, except the two RGBA16 fixtures and `noise_rgba8`, which
are 1024x768. Checksums and allocations are included; file I/O and freeing
returned output are excluded.

### Decoder

Native-output timings use the median of 12 alternating rounds after
warm-up. Both decoders include setup and end-of-file processing; libpng
uses contiguous output storage. Milliseconds are derived from reported MPix/s.

| Image | ptpng | libpng + zlib-ng | Speedup |
| --- | ---: | ---: | ---: |
| photo_rgb8 | 34.04 ms | 46.37 ms | 1.36x |
| photo_rgba8 | 85.17 ms | 110.93 ms | 1.30x |
| photo_gray8 | 19.68 ms | 33.63 ms | 1.71x |
| photo_gray16 | 23.27 ms | 38.31 ms | 1.65x |
| graphic_pal8 | 2.13 ms | 7.37 ms | 3.46x |
| graphic_rgb8 | 11.60 ms | 13.19 ms | 1.14x |
| photo_rgba16_paeth | 28.54 ms | 42.26 ms | 1.48x |
| graphic_rgba16_paeth | 3.40 ms | 15.30 ms | 4.50x |
| noise_rgba8 | 11.61 ms | 11.89 ms | 1.02x |

### Encoder

This snapshot uses the previous speed-first encoder. From 1 October,
the default encoder uses full-row filters, dynamic Huffman blocks and
deeper match parsing; speed and file sizes are therefore different.

Encoding uses the median of five rotated rounds after verified warm-up.
Both encoders receive identical pixels; the palette fixture expands to
RGBA8 for both. The reference uses level 1. Size is ptpng's PNG byte count
divided by the reference's; below 100% means a smaller file.

| Image | ptpng | libpng + zlib-ng level 1 | Speedup | Size vs reference |
| --- | ---: | ---: | ---: | ---: |
| photo_rgb8 | 75.35 ms | 262.96 ms | 3.49x | 96.1% |
| photo_rgba8 | 172.36 ms | 418.91 ms | 2.43x | 100.7% |
| photo_gray8 | 42.77 ms | 121.95 ms | 2.85x | 98.5% |
| photo_gray16 | 54.37 ms | 188.50 ms | 3.47x | 96.3% |
| graphic_pal8 | 15.24 ms | 171.47 ms | 11.25x | 65.9% |
| graphic_rgb8 | 12.39 ms | 141.95 ms | 11.46x | 70.7% |
| photo_rgba16_paeth | 39.93 ms | 131.56 ms | 3.30x | 101.2% |
| graphic_rgba16_paeth | 3.47 ms | 34.13 ms | 9.84x | 66.8% |
| noise_rgba8 | 8.26 ms | 71.34 ms | 8.64x | 94.7% |

Level 6 can produce substantially smaller files. See the
[dashboard](https://bojosos.github.io/ptpng/bench/?mode=encode&platform=windows-x64&image=photo_rgb8&format=none&reference=libpng-zng-level6&metric=size&range=10)
for those sizes and timings, stock-zlib comparisons and newer measurements.
The five-platform summary remains in the [README](README.md#performance-30-september-2026).
Hosted machines can change between runs; these ratios compare engines
within one run rather than code revisions across different machines.

## Changes

- AVX2 Sub filtering reconstructs RGB and 16-bit RGB directly into the
  output row using prefix sums, without an intermediate copy.
- NEON Sub filtering covers all six PNG byte strides. NEON Adler-32
  replaces the scalar checksum on ARM64.
- Fixed DEFLATE blocks reuse cached Huffman roots. This removes 29 KiB
  of copies per fixed block and 24 KiB of unused fixed-table storage.
- The SSE2 fallback no longer contains SSSE3 instructions. The ARM scalar
  Sub fallback now uses the correct recurrence.
- Long x86 rows with 2-, 4- or 8-byte pixels retain the established
  blocked SSE2 path after local measurements showed possible regressions
  with the new fused path. RGB and 16-bit RGB use the new implementation.
- RGB16-to-RGBA8 conversion no longer loads beyond its source tail.

## Fixed-block measurement

Local Windows x64, MSVC `/O2`, old and new binaries run in alternating
order, best of ten decodes per invocation, Adler verification disabled
for this inflate-only measurement:

| Input | Before | After | Speedup |
| --- | ---: | ---: | ---: |
| 50,001 tiny fixed blocks, run 1 | 22.744 ms | 0.874 ms | 26.0x |
| Same input, run 2 | 23.144 ms | 0.930 ms | 24.9x |

The stream contains 500,011 compressed bytes and 1,600,032 output bytes.
This deliberately stresses block setup. It is not a representative PNG
corpus. The old executable used the static MSVC runtime and the new one
used the dynamic runtime. The ordinary 64x64 `fixedhuff.png` fixture
showed no clear improvement above timing noise.

Generate the stream with Python's standard library:

```python
import pathlib
import zlib

def compressor():
    return zlib.compressobj(6, zlib.DEFLATED, -15,
                            zlib.DEF_MEM_LEVEL, zlib.Z_FIXED)

c = compressor()
block = c.compress(b"a" * 32) + c.flush(zlib.Z_SYNC_FLUSH)
c = compressor()
end = c.compress(b"a" * 32) + c.flush()
data = b"a" * (32 * 50001)
stream = (b"\x78\x01" + block * 50000 + end
          + zlib.adler32(data).to_bytes(4, "big"))
pathlib.Path("build/many_fixed.z").write_bytes(stream)
```

After building Release with CMake, run `build/inf_bench build/many_fixed.z
1600032`. On Windows the executable has the `.exe` suffix.

## Whole-image and filter benchmarks

Native macOS ARM64 results from [the September 24 nightly run](https://github.com/bojosos/ptpng/actions/runs/36043349483),
commit `5079d0e`, generated 3200x2400 images, checksum verification on:

| Native output | ptpng, MPix/s | libpng + zlib-ng, MPix/s | ptpng speedup |
| --- | ---: | ---: | ---: |
| photo_rgb8 | 212.68 | 139.36 | 1.53x |
| photo_rgba8 | 63.64 | 59.09 | 1.08x |
| photo_gray8 | 330.11 | 213.85 | 1.54x |
| photo_gray16 | 256.28 | 170.37 | 1.50x |
| graphic_pal8 | 1982.45 | 1663.42 | 1.19x |
| graphic_rgb8 | 394.51 | 557.98 | 0.71x |

The same runner measured NEON Sub at 7.1–10.2 GB/s on 4096-byte rows,
5.7–19.4x the corrected scalar implementation depending on bytes per
pixel. RGB was 7414.9 versus 546.9 MB/s. This comparison isolates the
filter; it is not a before/after whole-image speedup. RGBA8 output also
lost to libpng+zlib-ng on palette graphics and RGB graphics, so conversion
and match-heavy images remain useful optimization targets.

The Linux x64 AVX2 runner measured 4096-byte RGB Sub at 17.33 GB/s
versus 2.19 GB/s in the scalar kernel, and 16-bit RGB at 17.33 versus
3.00 GB/s. Whole-image performance still varied: native photo RGB was
178.06 versus 170.16 MPix/s against libpng+zlib-ng, while photo RGBA was
59.93 versus 75.93 and RGB graphics were 376.33 versus 947.40 MPix/s.
These losses matter more than the isolated filter wins when deciding
what to optimize next.

`tests/gen_bench.py` creates a deterministic synthetic image corpus with
Pillow. `bench_compare` compares ptpng with libpng and stock zlib;
`bench_zlibng` compares against libpng with zlib-ng. Both report MPix/s,
not decoded MB/s. They include allocation, parsing and decompression,
use contiguous pixel storage, and leave freeing the returned pixels
outside the timed region. Measurements are best of twelve decodes.

```text
python tests/gen_bench.py
build/bench_compare local build/stock.json tests/bench/photo_rgb8.png
build/bench_zlibng local build/zng.json tests/bench/photo_rgb8.png
build/filters_test --bench
```

The nightly workflow runs these comparisons on Linux x64 and ARM64,
macOS ARM64, and Windows x64 and ARM64. It uploads JSON results and
scalar-versus-dispatched filter timings as artifacts, then updates the
benchmark history. Shared runner timings vary with host load, CPU model,
frequency and compiler; compare repeated runs on the same hardware.

## Correctness checks

The suite compares 309 images in three output formats against libpng,
checks every Paeth predictor input, and tests filters across alignments,
tails and overlapping buffers. Decoder regressions cover compressed
metadata, metadata limits, invalid chunks, mixed DEFLATE block types,
truncation and dynamic output capacity. CI includes ASan/UBSan corpus
decoding and mutation fuzzing.

## Remaining performance work

Average and the remaining scalar Paeth strides are candidates for SIMD
on ARM and x86. Four-byte Paeth pixels now use SIMD on AVX2 CPUs.
CRC-32 now has runtime-detected PCLMUL and AArch64 CRC paths;
parallel ARM CRC chains remain a candidate. Any replacement
needs corpus measurements with checksum verification enabled and tests
for short rows, tails and fallback CPUs. A broader comparison should
include other specialized PNG decoders and a real-image corpus before
making a fastest-decoder claim.

## VTune-guided follow-up

An elevated VTune hardware sampling run on an Intel Core i7-1355U
identified decompression, Paeth filtering and SSE2 Adler-32 as the main
costs of decoding `graphic_rgb8.png`. Branch misprediction accounted for
only 0.6% of P-core pipeline slots in that recording.

The follow-up changes target those operations:

- Distance-one DEFLATE matches fill the output directly. Previously,
  matches longer than 32 bytes built a 64-byte periodic scratch buffer
  before copying it to the output.
- AVX2 Adler-32 processes 32 bytes per iteration with fixed byte weights
  and prefix sums. The SSE2 and scalar fallbacks remain available on
  other x86 CPUs; ARM uses the existing NEON implementation.
- Multichannel Paeth uses an equivalent threshold calculation with fewer
  arithmetic operations. The one-byte path retains its previous predictor
  because the proposed replacement had no repeatable advantage.
  Exhaustive predictor tests verify the PNG tie rules.

The decoder tests cover every distance-one match length from 3 to 258
bytes, including exact output capacity and insufficient capacity. The
checksum tests cover unaligned inputs, vector and chunk boundaries, and
all-255 data to exercise the accumulator bounds. `filters_test
--bench-paeth` measures Paeth separately; nightly filter artifacts now
include these timings alongside Sub.

Local Windows measurements against `c1ed216`, identical MSVC
`/O2 /Ob2 /Zi` builds, pinned to logical CPU 4, an E-core, at AboveNormal
priority:

| Native decode | Median paired speedup | Range across three pairs |
| --- | ---: | ---: |
| graphic_rgb8 | 1.73x | 1.71–1.80x |
| graphic_pal8 | 1.56x | 1.40–1.74x |
| photo_rgba8 | 1.19x | 1.14–1.21x |
| photo_rgb8 | 0.98x | 0.93–1.07x |
| photo_gray8 | 1.03x | 0.96–1.11x |
| photo_gray16 | 1.03x | 0.97–1.11x |

Each pair ran the old and new `ptpng_tool --native --bench 16` binaries
consecutively, alternating order between rounds, with CRC and Adler
verification enabled. Ratios use each invocation's best decode time.
Another project was compiling on this laptop, so absolute times drifted.
The graphics and RGBA gains were consistent; these measurements do not
establish a change for RGB photos or grayscale images.

Three additional paired `inf_bench` runs on the extracted graphics IDAT
stream measured inflate without Adler at 10.29–11.06 ms before and
3.12–3.57 ms after. Adler alone on the 23,042,400 decoded bytes measured
4.60–4.85 ms before and 3.64–3.90 ms after. These are component timings,
not whole-image results. ARM performance must be measured on ARM hardware.

### Separate P-core and E-core timings

Windows CPU-set topology confirms that logical CPUs 0–3 are hardware
threads on two P-cores, and logical CPUs 4–11 are eight E-cores. The
original VTune recording sampled work on all twelve logical CPUs.

A subsequent timing-only run used logical CPU 2 for the P-core test and
logical CPU 4 for the E-core test, with the same three-pair methodology
and no profiler. Values below are median speedup followed by the full
range across pairs:

| Native decode | P-core | E-core |
| --- | ---: | ---: |
| graphic_rgb8 | 1.77x, 1.71–1.95x | 1.77x, 1.61–1.91x |
| graphic_pal8 | 2.09x, 1.70–2.14x | 1.83x, 1.65–2.06x |
| photo_rgb8 | 0.96x, 0.85–1.10x | 0.78x, 0.50–1.06x |
| photo_rgba8 | 1.16x, 0.88–1.44x | 1.11x, 1.00–1.13x |
| photo_gray8 | 0.99x, 0.94–1.12x | 0.97x, 0.76–1.05x |
| photo_gray16 | 1.07x, 0.94–1.27x | 1.27x, 0.98–1.29x |

The graphics gains persisted on both core types. Background compilation
continued during this run, and photo timings varied too much to establish
reliable improvements or regressions. In particular, the E-core RGB photo
slowdown needs an idle-machine repeat. Pinning prevents core migration;
it does not isolate shared resources or hold CPU frequency constant.

## Further x64 optimizations

Four-byte Paeth pixels now use 128-bit SIMD in the AVX2 translation unit.
The predictor operates on four 16-bit lanes, then wraps reconstructed
bytes modulo 256. This covers RGBA8 and other four-byte pixel layouts.
Other strides retain scalar prediction, including RGB, whose experimental
SIMD implementation regressed on the E-core.

Short DEFLATE matches with length at most 16 and distance at least 16
use one fixed-size copy when the output allocation has 16 bytes left.
The source lies entirely in decoded history and cannot overlap that
copy. The logical output position advances by the actual match length.
This avoids the larger copy loop for common photo back-references.

Local measurements against `368c6b4`, the preceding code plus its timing
documentation, used identical MSVC optimization settings. A single
process loaded old and new libraries, verified equal decoded pixels,
and alternated their order for 21 paired native decodes per image.
CRC and Adler verification remained enabled, allocation was timed, and
freeing the returned image was outside the timed interval.

| Native decode | P-core CPU 2, median paired speedup | E-core CPU 4, median paired speedup |
| --- | ---: | ---: |
| photo_rgba8 | 1.12x | 1.10x |
| photo_rgb8 | 1.02x | 1.08x |
| photo_gray8 | 1.06x | 1.09x |
| photo_gray16 | 1.05x | 1.08x |
| graphic_rgb8 | 1.02x | 0.99x |
| graphic_pal8 | 0.98x | 0.98x |

Background compilation continued, so absolute times varied and small
differences around 1.00x remain inconclusive. The RGBA P-core paired
ratios had a middle-90% range of 1.08–1.23x; the E-core range was
0.94–1.24x. Paired [Windows thread-cycle counters](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-querythreadcycletime)
gave corresponding RGBA medians of 1.12x and 1.11x. Those counters were
compared directly, without converting them to elapsed time.

The isolated four-byte Paeth kernel measured 2.1–2.7x scalar throughput
on the P-core and 1.2–1.3x on the E-core for 4096-byte rows. Whole-image
gains are smaller because decompression and other work remain.

Tests cover all 16.7 million predictor triples through the dispatched
four-byte kernel, unaligned and overlapping rows, and exact input
allocations. Match-copy tests cover every distance from 1 to 64 and
length from 3 to 258, with and without following literals: 32,768 cases.
Each case also checks insufficient capacity and its output boundary.

## First encoder release

Commit `e6f083c` adds encoding for gray, gray+alpha, RGB and RGBA at 8 or
16 bits. Output is non-interlaced and contains no ancillary metadata.
AVX2 filters handle 32 bytes per iteration; NEON handles 16. Adaptive
filtering scores up to 192 sampled bytes per row and runs the selected
filter once over the full row. The compressor uses a 32 KiB hash table,
bounded LZ77 search and fixed Huffman codes. If that stream exceeds the
stored-block bound, it emits stored blocks instead. No runtime dependency
was added. Format references are the [PNG specification](https://www.w3.org/TR/png-3/)
and [DEFLATE specification](https://www.rfc-editor.org/rfc/rfc1951).

Local Windows x64, i7-1355U, Release MSVC build. These are two generated
3200x2400 photo-like images, not a real-world photo corpus. Each encoder
ran once for warm-up and pixel verification, then five times with order
rotated each round. The table uses median elapsed times. Allocation,
filtering, compression and checksums are timed; freeing the returned PNG
is excluded for every encoder. Runs were pinned to P logical CPU 2 and
E logical CPU 4. System load and frequency were not fixed, so compare
encoders within each run rather than absolute P/E times.

| Image | Core | ptpng | libpng+zlib-ng level 1 | Speedup | ptpng bytes / reference bytes |
| --- | --- | ---: | ---: | ---: | ---: |
| photo_rgb8 | P | 243.36 ms | 841.58 ms | 3.46x | 9,739,624 / 10,136,572 |
| photo_rgb8 | E | 264.24 ms | 1,018.55 ms | 3.85x | 9,739,624 / 10,136,572 |
| photo_rgba8 | P | 532.10 ms | 1,393.55 ms | 2.62x | 19,392,753 / 19,251,365 |
| photo_rgba8 | E | 501.43 ms | 1,195.84 ms | 2.38x | 19,392,753 / 19,251,365 |

The RGB file is 3.9% smaller than the zlib-ng level-1 reference; RGBA is
0.7% larger. Against stronger compression, the tradeoff changes: zlib-ng
level 6 produced 5,350,341 RGB bytes and 11,637,017 RGBA bytes, making our
output 82% and 67% larger. Stock-zlib level 1 produced 7,116,353 RGB bytes;
our P-core encode was 5.37x faster but 37% larger. This release is a fast
compression mode, with no claim to lead all encoders or all images.

Reproduce the stronger-reference comparison on Windows:

```powershell
./build/bench_encode_zlibng --cpu 2 local-p encode_p.json tests/bench/photo_rgb8.png tests/bench/photo_rgba8.png
./build/bench_encode_zlibng --cpu 4 local-e encode_e.json tests/bench/photo_rgb8.png tests/bench/photo_rgba8.png
```

Tests add 2,307 complete PNG round trips, independently read by libpng,
and 1,496 raw compression streams, independently inflated by zlib in
reference builds. Filters are compared with scalar reference predictors
across alignments, exact buffer ends and every Paeth input triple. All
seven platform/sanitizer CI jobs passed for `e6f083c`. Further compression
work should compare specialized encoders and a wider image corpus, while
reporting file size alongside speed.

## Reading the nightly graphs

[GitHub-hosted jobs use fresh virtual machines](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
The workflow does not guarantee an identical physical CPU or load between
runs. Between `77cbb8b` and `28971d9`, Linux RGB decode throughput changed
from 242.00 to 189.40 MPix/s, while unchanged libpng changed from 149.60 to
107.35. Within-run speedup changed from 1.62x to 1.76x. The shared slowdown
suggests runner variation; those records lack CPU model information and
cannot establish its exact cause.

The dark chart page now derives same-run ratios for historical decoder
points. From `e6f083c`, artifacts also record CPU, OS and runner image.
Decoder timing changes to medians of 12 alternating rounds after warm-up;
older points used separate best-of-12 samples. This method change is
visible on the page and in new data tooltips. Encoder charts report both
time and bytes against libpng levels 1 and 6, using zlib and zlib-ng.

A warm-buffer 64 MiB memory-copy benchmark provides throughput context.
Its 128 MiB working set may interact differently with each CPU's cache.
It reports payload MB/s, not aggregate read/write traffic. This is a
measured reference rather than a theoretical PNG limit: decoding reads
compressed data, writes raw pixels, and performs input-dependent work.
Kernel throughput and complete-image throughput answer different questions.

## Further x64 encoder and decoder work, September 25

Three changes survived comparison against `98e53b0`:

- RGB8-to-RGB8 and RGBA8-to-RGBA8 decoding now returns the reconstructed
  buffer directly. This removes one allocation and a full-image copy.
  RGB8 still ignores tRNS; RGB-to-RGBA still expands transparency.
- AVX2 dispatch now handles the eight byte chains of RGBA16 Paeth with
  exact eight-byte loads and stores. Its existing four-byte loop stays
  separate. This uses 128-bit vectors in the AVX2 translation unit.
- The x64 encoder writes completed DEFLATE bytes with an unaligned
  64-bit store, retaining fewer than eight pending bits. The store stays
  inside the output limit; a byte tail handles the end of the allocation.
  Compression decisions, emitted bytes and output sizes are unchanged.

Whole-image measurements used baseline and candidate DLLs loaded into
one process on the i7-1355U. Each pair alternated order, with 21 pairs
for the cases below, pinned to P logical CPU 2 or E logical CPU 4. Tests
include allocations and checksums, excluding output freeing. Every warm-up
compared complete output bytes. Thread-cycle ratios broadly agreed with
elapsed-time ratios; frequency and background load were not controlled.

| Operation / image | P-core paired speedup | E-core paired speedup |
| --- | ---: | ---: |
| Decode graphic_rgb8 to RGB8 | 1.56x | 1.44x |
| Decode photo_rgba8 to RGBA8 | 1.11x | 1.07x |
| Decode photo_rgba16_paeth, native | 1.16x | 1.18x |
| Decode graphic_rgba16_paeth, native | 1.93x | 2.12x |
| Encode photo_rgb8 | 1.03x | 1.12x |
| Encode photo_rgba8 | 1.05x | 1.07x |

The encoder's RGB P-core screening run measured 1.07x; the longer repeat
above measured 1.03x. Gray and graphics encoding changes were smaller,
with some indistinguishable from noise. Native RGBA8 decoding, which
does not benefit from the removed output copy, remained near 0.99x in
both core tests. The RGBA16 photo paired ranges were 1.08–1.20x on P and
1.15–1.20x on E. The flat-color RGBA16 case spends a much larger fraction
of its time filtering, explaining its larger whole-image gain.

The new RGBA16 fixtures are synthetic 1024x768 images with Paeth on every
row and varying low sample bytes. Reproduce them with
`python tests/gen_bench16.py`. The nightly workflow now covers those two
images plus the previous six, in native, RGBA8 and RGB8 decode modes.
It independently compares benchmark pixels with libpng before timing.

Validation includes all seven local CTest suites, libpng parity on the
large benchmark images in all three output modes, and all 16.7 million
Paeth triples through both four-byte and eight-byte dispatch. A separate
comparison checked 2,096 raw compression streams against the old encoder
byte-for-byte and then inflated each with zlib. This covers every length
from 0 through 512, DEFLATE block/window boundaries and multiple patterns.

Bit-scan match-length detection, a four-byte bit-writer store and moving
writer state into a local struct did not show convincing overall gains
in the screening tests and were not included. No new VTune collection
was needed for these changes.

## Literal batching and ARM profiling, September 25

An x64 encoder change packs three already-selected literal codes into one
bit-writer call. Only runs of at least eight literals use the separate helper;
match probes, compression choices and PNG bytes remain unchanged. An earlier
version slowed RGBA photos by 5% on the E-core and was discarded.

Against `f6172e4`, 21 alternating pairs on the i7-1355U measured 1.41x P-core
and 1.32x E-core encoding speed on a 1024x768 random RGBA image. Thread-cycle
ratios were 1.43x and 1.29x. RGB/RGBA photo ratios were 1.00–1.02x. Gray and
palette cases were close to parity. The small flat RGBA16 image was noisy:
an E-core repeat measured 0.94x elapsed throughput and 0.97x by thread cycles;
that case does not call the new helper. Background load and code layout remain
possible factors, so these measurements do not establish a universal win.
A later 51-pair E-core repeat put flat RGBA16 at 0.999x elapsed throughput
and 0.997x by thread cycles, while random RGBA remained 1.302x and 1.299x.
All seven local tests and 2,096 byte-identical streams checked with zlib passed.
Nightly benchmarks now include the random RGBA image to track literal-heavy
workloads alongside the eight existing fixtures.

The first [Linux profiling run](https://github.com/bojosos/ptpng/actions/runs/36060188672)
used a Neoverse-N2 ARM64 runner and exposed user hardware counters. Scalar Paeth
accounted for 44% of RGBA16-photo decode samples and 22% of RGBA8-photo samples;
inflate took 47% and 63%. Photo encoding spent 75–78% in fixed-Huffman DEFLATE.
These are sampled self costs, including brief process setup, not exact stage
timings. The x64 VM supported software samples only: perf silently changed
the requested cycles event to task-clock. The collection script now reads the
recorded event to label that fallback correctly.

The profiling workflow accepts an optional baseline revision. It builds both
versions with identical flags on the same VM, pins each to one CPU, and runs
nine alternating timing pairs. Artifacts retain every result, the CPU/compiler
details, sampled stacks and annotated instructions.

The [paired ARM run](https://github.com/bojosos/ptpng/actions/runs/36061201763)
compared the new NEON Paeth implementation against `f6172e4` on one
Neoverse-N2 CPU, nine alternating one-second pairs per case:

| Native decode | Median speedup | Observed pair range |
| --- | ---: | ---: |
| RGBA8 photo | 1.004x | 1.002–1.006x |
| RGBA16 Paeth photo | 1.553x | 1.550–1.555x |
| RGBA16 Paeth graphics | 1.935x | 1.932–1.937x |

NEON processes the independent four- or eight-byte Paeth chains in 16-bit
lanes, using exact-width loads/stores and scalar partial-pixel tails. The
four-byte path did not materially improve this ARM photo workload; the
eight-byte path did. These Linux ARM results do not predict Apple Silicon
performance. macOS ARM and Windows ARM correctness checks passed separately.

The initial GCC build improved random RGBA encoding 1.278x but slowed the
RGBA8 photo to 0.974x. Its generated code placed the rare batching call on
the fall-through path and moved short literals behind extra jumps. Marking
long runs unlikely under GCC/Clang restored the photo case to 0.999x
(0.997–1.004x range), with random RGBA still 1.248x faster
(1.101–1.483x range). Graphics encoding was 1.000x. This
[final paired run](https://github.com/bojosos/ptpng/actions/runs/36062061909)
also repeated the ARM gains at 1.555x for RGBA16 photos and 1.944x for
RGBA16 graphics. Windows keeps the previously measured MSVC code path.

The [expanded differential fuzz campaign](https://github.com/bojosos/ptpng/actions/runs/36061508310)
completed 2,641,561 executions with ASan and UBSan across Linux x64 and ARM64,
with no reported failures. Each of three targets ran for 120 seconds per
architecture. Native decoder output is compared with libpng when both accept
the input; encoder output is decoded by libpng, and decompression is checked
against zlib. The seed corpus includes valid compressed profiles, suggested
palettes, text-allocation boundaries, and full-sized random encoder inputs.
Pixel/input limits and short campaign budgets remain deliberate restrictions;
execution counts are not coverage percentages or proof of correctness.

## Nightly graph audit, September 27

The [September 24 baseline](https://github.com/bojosos/ptpng/actions/runs/36062576710)
and the scheduled runs on [September 25](https://github.com/bojosos/ptpng/actions/runs/36113326363),
[September 26](https://github.com/bojosos/ptpng/actions/runs/36229352660), and
[September 27](https://github.com/bojosos/ptpng/actions/runs/36307712066)
all tested unchanged commit `c1c133f`. Each run's 550 decoder and memory-copy
measurements and 540 encoder measurements exactly match the published graph
data. Changes between these four points cannot be attributed to code changes.

Windows x64 alternated between EPYC 7763 on September 24/26 and EPYC 9V74 on
September 25/27. Against September 24, the latest ptpng decode throughput rose
14.5% at the median across cases, while libpng with zlib-ng rose 22.2%.
macOS kept the same Apple M1 Virtual description but varied substantially:
ptpng's median increase was 12.1%, with individual cases up to 60.3%; the
reference median increased 17.5%. Within-run ratios help, but do not remove
differences in how implementations respond to hardware and VM scheduling.

The latest run gives these geometric means, weighting each case equally:

| Platform | Decode speed vs libpng + zlib-ng | Encode speed vs libpng + zlib-ng level 1 |
| --- | ---: | ---: |
| Linux x64 | 1.533x | 5.487x |
| macOS ARM64 | 1.324x | 4.897x |
| Windows x64 | 1.433x | 4.907x |
| Windows ARM64 | 1.253x | 4.242x |
| Linux ARM64 | 1.250x | 4.435x |

Decode covers nine synthetic images in three output formats; encode covers
the nine images. These averages are not a claim about every workload.
Random RGBA decoding remains slower than the reference on every platform.
Linux ARM64 native/RGBA output runs at about 0.50x, and Linux x64 at 0.59x.
Palette-to-RGB conversion on ARM runs at 0.715–0.784x. The encoder wins all
45 platform/image comparisons against level 1, but its graphics files are
4.54–4.96x larger than level 6. Encoded sizes remained identical across all
four runs and all five platforms.

The filter artifacts also show four-byte NEON Paeth behind scalar on their
4096-byte rows: 0.605–0.670x on macOS, 0.798–0.808x on Windows ARM64 and
0.965–0.973x on Linux ARM64. This microbenchmark alone is not grounds to
disable the path. Its row pattern differs from whole-image random pixels,
and GCC's scalar predictor branches can respond differently to that data.
Native, paired whole-image measurements are needed to resolve the choice;
the microbenchmark does not establish a Linux whole-image regression.

The [performance graphs](https://bojosos.github.io/ptpng/bench/) now
label measurement dates, show compact CPU details, and link points to their
Actions runs. Encoder comparisons include speed and file-size ratios against
both zlib-ng level 1 and level 6, alongside the existing raw charts. Above
1x means faster on speed charts; below 1x means smaller on size charts.

## Hardware checksums and palette expansion, September 27

PNG CRC now uses four PCLMUL folding chains on capable x86 CPUs and IEEE
CRC instructions on capable AArch64 CPUs. Runtime checks retain the portable
implementation on CPUs without those instructions. The x86 implementation
derives from zlib-ng, with its license retained in the source. Exact loads and
scalar tails avoid reading beyond the input. Checksum tests compare dispatched
results with the scalar reference across lengths, alignments and buffer ends.

The ARM64 encoder now uses the bounded 64-bit writer and three-literal batching
previously used on x64. The writer alone made photos faster but slowed noisy
encoding by 16–19% on Linux/Windows ARM64. Batching targets those long literal
runs; compression decisions and encoded bytes are unchanged.

Paeth selection also needs compiler-specific whole-image evidence. GCC retains
four-byte NEON because its scalar fourth channel has a data-dependent branch
that slows random alpha. Clang selects scalar for four-byte pixels, with the
NEON kernel separate from that selection. MSVC retains its original direct
NEON dispatch: scalar selection made RGBA8 photos faster but slowed the flat
RGBA16 case, even after restoring the original kernel body behind a wrapper.

The [Linux profile](https://github.com/bojosos/ptpng/actions/runs/36335673907)
at `dd1c78a` exposed hardware counters on Neoverse-N2 ARM64 and software timer
samples on EPYC 7763 x64. Palette-to-RGB conversion accounted for 83.0% and
84.9% of samples respectively. The revised converter uses the existing packed
palette table, one four-byte copy per pixel, and an exact three-byte final copy.
Every store stays within the output row. It covers all four palette depths.
On the i7-1355U, 21 alternating same-process pairs against the hardware-CRC build
measured 1.231x P-core and 1.313x E-core whole-image RGB decode throughput;
thread-cycle ratios were 1.214x and 1.312x. Pixels matched, and independent
libpng parity tests passed.

Noise still has room for improvement. ARM decode samples attributed 40.1% to
Paeth, 17.1% to Adler and 13.3% to CRC. Encoder literal batching plus fixed-code
compression took about 60–63% on both Linux architectures before stored-block
fallback. These are sampled self costs, not exact stage durations. An early
stored-block decision needs compression-size testing on mixed content before
it can replace that work.

The Linux CI noise PNG was 3,147,615 bytes; local, macOS and Windows generated
PNGs were 3,319,878 bytes.
They exercise different compressed streams despite matching image dimensions.
Each paired comparison uses one shared input file, but results across those
fixtures should not be equated. Future Linux profile artifacts retain the PNGs
as well as the executable, stacks and annotated instructions.

The final [sanitizer fuzz campaign](https://github.com/bojosos/ptpng/actions/runs/36336235745)
completed 4,415,223 executions across decode, inflate and encode targets on
x64/ARM64, 120 seconds per target, with no reported failures. The final
[CI matrix](https://github.com/bojosos/ptpng/actions/runs/36336218991)
and [five-platform nightly](https://github.com/bojosos/ptpng/actions/runs/36336232524)
also passed. These remain bounded campaigns, not proof of correctness.

The [full paired comparison](https://github.com/bojosos/ptpng/actions/runs/36336395808)
tested `8c6940f` against `c1c133f` with nine alternating one-second pairs per
workload. Each old/new pair ran on the same machine and input; Linux and Windows
were pinned to one logical CPU, while macOS was OS scheduled. Ratios below are
median throughput ratios. Every paired output size matched.

| Platform | Noise decode | Noise encode | Palette-to-RGB decode |
| --- | ---: | ---: | ---: |
| Linux x64 | 1.502x | 1.144x | 1.718x |
| Windows x64 | 1.122x | 1.180x | 1.326x |
| macOS ARM64 | 1.203x | 1.594x | 1.467x |
| Linux ARM64 | 1.464x | 1.413x | 1.912x |
| Windows ARM64 | 1.143x | 1.681x | 1.400x |

These results exposed remaining flat RGBA16 decode regressions: Linux ARM64
was 0.966x (0.962–0.969x pair range), and Windows ARM64 was 0.947x
(0.943–0.952x). Restoring the old kernel body behind a wrapper did not fix the
Windows case. They prompted a focused follow-up on compiler-specific dispatch
and loop layout; the broad improvements do not cancel out these losses.

The [focused follow-up](https://github.com/bojosos/ptpng/actions/runs/36337098143)
tested `252da3d` against the same original baseline, nine alternating half-second
pairs. Retaining GCC's measured four-byte-first loop layout restored flat
RGBA16 decode to 1.001x (0.995–1.011x). Restoring MSVC's original direct dispatch
put Windows ARM64 flat RGBA16 at 1.032x (1.026–1.036x), with RGBA8 photo decode
still 1.057x faster. Linux/Windows ARM64 noise encoding remained 1.378x/1.704x.
This final choice gives up some Windows RGBA8 photo gain to remove the flat-image
regression. All platform CI checks and the push-triggered sanitizer fuzzing
passed after this adjustment.

The final nightly changed x64 runner models again (Linux EPYC 9V45 and Windows
EPYC 7763) and showed a Linux ARM RGB-graphics dip. A
[same-machine graphics check](https://github.com/bojosos/ptpng/actions/runs/36337884642)
at `03dd5f9` found Linux ARM RGB decode at 1.003x (0.981–1.024x), with palette
RGB conversion still 1.917x faster. macOS palette RGB conversion measured
1.586x (1.349–1.674x) against the original code, despite its latest nightly
remaining below the zlib-ng reference. This check does not reproduce a graphics
code regression. Current-code versus reference and new-code versus old-code
are different comparisons; hosted-runner nightly swings require paired checks.

## 30 September profiling and literal batching

This round compares `86bcf29` with `5deaece`, the code used by the previous
nightlies. A fresh VTune software profile on the i7-1355U, pinned to P-core
logical CPU 2, attributed 80.4% of noise-decode samples to `inflate_impl` and
3.9% to Paeth. Hardware counter collection still requires an elevated VTune
process on that Windows machine. Profiling timings are not benchmark timings.

The [fresh Linux profiles](https://github.com/bojosos/ptpng/actions/runs/36749760593)
used EPYC 7763 software samples and Neoverse-N2 hardware cycles. The ARM noise
decode attributed 38.1% of samples to Paeth, 18.2% to Adler and 16.0% to CRC.
Noise encoding attributed 49.1%/53.3% to literal emission on x64/ARM64.
The artifacts retain inputs, binaries, annotated instructions and counter reports.
Linux noise again used stored blocks; Windows/macOS noise used a fixed-Huffman
stream. Each old/new pair shares its input, but these workloads differ across OSes.

The changes preserve pixels, compression decisions and encoded bytes:

- Decode up to four root-table literals per refill. Refill before processing
  a following match, and keep exact output and input boundaries.
- Classify valid root literals with one comparison. Reserved symbols 286/287
  have zero-length entries and remain errors.
- Emit six encoder literals per bounded 64-bit store, keeping writer state in
  registers during long runs. Short tails retain the existing bounded writer.
- Expand RGB8 to RGBA8 using NEON structured loads and stores, with exact vector
  and scalar tails. Images with RGB tRNS retain the scalar transparency conversion.
- Accumulate ARM Adler byte columns over 64-byte blocks, applying position
  weights once per 2,048-byte chunk. Inputs below 512 bytes retain the old kernel.

The [first full comparison](https://github.com/bojosos/ptpng/actions/runs/36750793304)
found a 5.2% Linux x64 gray-decode loss and a 3.1% Windows ARM flat-RGBA16 loss.
Simplifying literal classification restored GCC's literal-table pointer to a
register, removed repeated stack loads, placed all four literal steps together,
and reduced the inflater by 60 bytes. The final gray-decode ratio was 1.021x;
Windows ARM flat RGBA16 was 1.021x. These measurements do not isolate the separate
effects of register allocation, branch count and code placement.

The [isolated ARM checksum comparison](https://github.com/bojosos/ptpng/actions/runs/36751990697)
tested `b9588ea` against `b3ce4f8`. Linux ARM noise decode improved 1.069x,
RGB graphics 1.186x and native palette graphics 1.446x. x64 controls remained
near parity. The new checksum uses baseline NEON instructions.

The [final full comparison](https://github.com/bojosos/ptpng/actions/runs/36752507940)
uses nine alternating one-second pairs for 22 workloads on each of five
platforms. Linux and Windows use one pinned logical CPU; macOS remains OS
scheduled. The table shows median candidate/baseline throughput ratios.
Every pair has matching input, decoded and encoded sizes.

| Platform | Noise decode | Noise encode | RGB graphics to RGBA8 | RGBA16 photo decode |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 1.000x | 1.074x | 1.011x | 1.119x |
| Windows x64 | 1.159x | 1.086x | 1.009x | 1.139x |
| Linux ARM64 | 1.066x | 1.178x | 1.648x | 1.115x |
| macOS ARM64 | 1.142x | 1.213x | 1.516x | 1.144x |
| Windows ARM64 | 1.115x | 1.467x | 1.463x | 1.131x |

macOS ratios vary more: its RGB-graphics conversion pairs ranged 1.371–1.758x.
Linux x64 noise encoding ranged 0.988–1.316x, so its 7.4% median gain is less
precise than Windows x64's 8.6% gain, whose pairs ranged 1.077–1.125x.

A [focused repeat](https://github.com/bojosos/ptpng/actions/runs/36753863588)
checked RGB/RGBA photos and noise. Linux x64 noise encode measured 1.157x,
while RGB photo encode retained a small loss: 0.987x, with all nine pairs
between 0.981–0.997x. The full run had measured 0.975x. Windows RGB photo encode
was 1.002x, Linux ARM 1.019x and Windows ARM 1.022x. GCC's photo compressor loop
matches the old loop instruction-for-instruction, and these photo inputs never
call the modified literal-run helper. The cause of the small Linux loss remains
unresolved; the patch does not improve every workload. Raising the helper's
threshold would not address a path these images do not execute.

Six decoder literals offered only a small local gain beyond four, and a cached
root-entry experiment slowed RGB graphics by 3–4% despite helping one photo.
Both remain excluded from the retained implementation.

Tests cover transparency at vector/tail/row boundaries, literal lengths 0–33,
both reserved fixed symbols, every truncated prefix, undersized outputs, and
checksum boundaries across 32 byte alignments. Independent zlib tests passed
1,496 streams, and local old/new encoder comparisons matched 2,096 streams
byte-for-byte. All seven [CI jobs](https://github.com/bojosos/ptpng/actions/runs/36752505886)
passed. The [final sanitizer fuzz campaign](https://github.com/bojosos/ptpng/actions/runs/36752973103)
completed 3,117,250 executions across decode, inflate and encode on x64/ARM64,
120 seconds per target, without a reported failure. Three longer campaigns in
this round completed 8,883,527 executions in total. These bounded campaigns
and synthetic benchmark images do not establish a fastest-in-the-world claim.

Chart verification matched all 2,180 values from the two new nightly artifacts.
It also found that the first run's commit label had advanced from `b3ce4f8` to
`b9588ea` while the run was in progress. The benchmark action now receives the
immutable workflow SHA for both histories, and that point's commit metadata was
corrected. Measurement values and the dark chart page were unchanged.
