# ptpng

A fast, single-threaded PNG decoder and encoder in C11, with a header-only
C++17/20 wrapper and no external runtime dependencies.

| Feature | Support |
| --- | --- |
| Decode | All PNG color types, 1/2/4/8/16-bit samples, Adam7 and tRNS |
| Encode | Gray, gray+alpha, RGB and RGBA at 8 or 16 bits |
| Output | Native pixels, RGB8 or RGBA8 |
| Acceleration | Runtime-dispatched AVX2/SSE2 on x86; NEON on ARM64 |

The encoder uses full-row adaptive filters and dynamic DEFLATE blocks.
It writes non-interlaced PNGs without palette, packed-sample or metadata support.
Library calls must be serialized across threads because internal tables
and dispatch state are shared.

[Nightly dashboard](https://bojosos.github.io/ptpng/bench/) ·
[C interface](include/ptpng.h) · [C++ interface](include/ptpng.hpp) ·
[Performance notes](PERFORMANCE.md)

## Build

CMake builds the library, tools and vendored libpng references on Linux,
macOS and Windows:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Optional settings:

- `-DPTPNG_WITH_LIBPNG=OFF`: skip libpng references and comparisons.
- `-DPTPNG_WITH_ZLIB_NG=OFF`: skip the second reference using zlib-ng.
- `-DPTPNG_BUILD_CPP_TESTS=ON`: test the C++17/20 wrapper.

CMake consumers can use `add_subdirectory(ptpng)` and link `ptpng::ptpng`.
C-only builds require no C++ compiler. For MSVC x64, `build.bat` also builds
the tools; its Visual Studio path may need adjusting.

## Use from C++

Include `ptpng.hpp` and link the C library. Images and encoded buffers
own their memory, support moves and add no pixel copies.

```cpp
#include <ptpng.hpp>

// png_bytes contains a complete PNG.
auto image = ptpng::decode(png_bytes.data(), png_bytes.size(),
                           {ptpng::output_format::rgba8});
auto png = ptpng::encode(image);
// Access pixels with image.data()/image.size(), and PNG bytes with png.data()/png.size().
// Both buffers and image metadata are freed automatically.
```

`image.layout()` describes the returned pixels; `image.source_info()` gives
the original IHDR and ancillary metadata. Encoding writes pixels only.
Decode palette or packed samples to RGB8/RGBA8 before re-encoding.
Failures throw `ptpng::error`, with `what()` and the original C `code()`.

For borrowed RGB8 pixels, use:

```cpp
auto png = ptpng::encode(ptpng::pixel_view{
    data, size, width, height, ptpng::color_type::rgb});
```

`pixel_view` also accepts a bit depth and row stride. C++20 adds
`std::span` inputs and `bytes()` spans on owned buffers.

## Use from C

### Decode

```c
#include "ptpng.h"

void *pixels = NULL;
size_t pixels_size = 0;
ptpng_info info = {0};
ptpng_opts opts = {0, PTPNG_OUT_RGBA8, PTPNG_DEFAULT_MAX_BYTES};

int rc = ptpng_decode(data, data_size, &opts, &pixels, &pixels_size, &info);
if (rc == PTPNG_OK) {
    /* Use pixels and info.width/info.height here. */
    ptpng_free(pixels);
    ptpng_info_free(&info);
}
```

`NULL` options select native output with checksums enabled. Native output
keeps packed samples and palette indices; RGB8/RGBA8 output converts them
to bytes and takes the high byte of 16-bit samples. RGBA8 expands tRNS to
alpha. `opts.max_bytes` limits decompressed output.

`ptpng_info` owns ancillary metadata, including text and ICC profiles.
Release it separately from the pixels. Metadata allocations are bounded;
excess metadata is skipped while pixels still decode.

### Encode

For a borrowed, tightly packed RGBA8 buffer:

```c
void *png = NULL;
size_t png_size = 0;
int rc = ptpng_encode(rgba, rgba_size, width, height, 0,
                      6, 8, NULL, &png, &png_size);
if (rc == PTPNG_OK) {
    /* Write png_size bytes from png. */
}
ptpng_free(png);
```

Pass a row stride for padded input. All native 16-bit pixels, including
encoder input, use PNG big-endian byte order. `NULL` encoder options select
adaptive filtering; zero-initialized options select the None filter.
See [ptpng.h](include/ptpng.h) for formats, filters, limits and error codes.

## Performance

Decoder results from 30 September 2026: Release-build speedups against
libpng + zlib-ng 2.2.4 from
[run 36753946386](https://github.com/bojosos/ptpng/actions/runs/36753946386),
commit [`86bcf29`](https://github.com/bojosos/ptpng/commit/86bcf29d098473fbe295995f5bb8a9ac1f215d98):

| Platform | Decoder speedup | Faster decoder cases |
| --- | ---: | ---: |
| Linux x64 | 1.80x | 27/27 |
| Windows x64 | 1.64x | 26/27 |
| Linux ARM64 | 1.59x | 27/27 |
| macOS ARM64 | 1.62x | 27/27 |
| Windows ARM64 | 1.46x | 26/27 |

Speedups are geometric means across 27 decoder cases in native/RGB8/RGBA8
output. Checksums and allocations are included. Timings use medians of
12 alternating rounds after warm-up. The decoder loses on noise-to-RGB8
on the two Windows platforms.

The encoder changed on 1 October to target level-6 file sizes. The RGB
photo drops from 9.29 to 5.11 MiB, versus zlib-ng's 5.10 MiB. All five
platforms produce identical PNG sizes; the nine fixtures are at most
2.4% larger than zlib-ng level 6. The 1 October encoder averaged 2.25–2.98x
stock zlib level 6 and 1.14–1.52x zlib-ng across the five platforms, with
some slower cases. Same-machine comparisons show another 14% gain on
Linux x64 and 7% on Linux ARM64, with byte-identical output.
See [encoder sizes](PERFORMANCE.md#1-october-encoder-compression) and
[speed measurements](PERFORMANCE.md#encoder-speed-at-unchanged-sizes).
Earlier dashboard points use the previous speed-first compressor.

The [3 October follow-up](PERFORMANCE.md#3-october-nightly-follow-up) improves
RGB-photo encoding by 7.5–15.8% and Gray16-photo encoding by 6.7–12.9% in
same-machine comparisons across all five platforms, with unchanged PNG
bytes. Some workloads retain small losses; see the full measurements.

See [per-image times and file sizes](PERFORMANCE.md#30-september-benchmark-snapshot)
or the [live dashboard](https://bojosos.github.io/ptpng/bench/) for all
platforms, output formats and reference levels. The dashboard keeps full
history, records machines and lets you share a selected view.

Hosted runners can change CPUs, so compare speed ratios within each run.
For code changes, use alternating old/new measurements on the same machine.
These synthetic fixtures do not establish a fastest-in-the-world claim.

## Tests and profiling

[CI](.github/workflows/ci.yml) checks all five platforms for byte parity
with libpng, C++ ownership, SIMD kernels and malformed streams. Linux jobs
also run ASan/UBSan. The corpus includes 104 libpng images and 205 generated
PNGs covering color types, depths, interlacing, filters and metadata.

- [Coverage fuzzing](.github/workflows/fuzz.yml) checks decode against libpng,
  inflate and raw DEFLATE against zlib, and PNG encoder round trips. Raw
  compressor inputs cover up to 2 MiB. It caches discoveries and
  uploads logs, corpora and crashes from x64/ARM64 runs.
- [Paired performance](.github/workflows/compare.yml) compares a baseline
  commit with the current revision on all five platforms.
- [Linux profiling](.github/workflows/profile.yml) saves `perf` reports and
  annotated assembly; it labels software sampling when hardware counters
  are unavailable.

The [nightly workflow](.github/workflows/nightly.yml) tests and benchmarks
all five platforms, then publishes the dashboard. Implementation details
and measured optimization results are in [PERFORMANCE.md](PERFORMANCE.md).
