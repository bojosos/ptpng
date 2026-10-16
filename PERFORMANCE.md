# Performance work, September–October 2026

ptpng targets fast single-threaded PNG decoding on x86 AVX2 and ARM64
NEON. A kernel benchmark measures one operation; it does not establish
the speed of a complete PNG decode or a lead over every other decoder.

## 16 October follow-up experiments

This round compares against main `db34161`, which already includes the
fragmented stored-stream shortcut below. Local measurements screen ideas;
acceptance uses alternating baseline/candidate measurements on GitHub
machines. Earlier laptop measurements made on battery are not acceptance
evidence.

The retained change copies zero-residual Paeth rows on Linux GCC ARM64.
The Windows fixed-literal experiment is held because its final comparison
repeats a substantial graphics encoding regression on EPYC 7763.

### Windows x64 fixed-literal experiment (held)

The Windows noise fixture contains two fixed-Huffman blocks. Its
3,319,878 compressed bytes and Linux's 3,147,615-byte stored-block input
inflate independently with zlib to the same 3,146,496 filtered bytes
(SHA-256 `e3da8405bdc2fcf1874758c4d99d9390e21d400064c28c5dac59d213e080a63b`).
The Windows bottleneck is therefore different from Linux's stored-stream
copying cost.

Fixed literal widths can be derived from five low bits before the decode
table returns the literal byte. The new four-literal batch retains the
table's literal classification, requires 36 available bits for four
nine-bit codes, and preserves the existing output-capacity check. A
nonliteral returns to checked decoding without consuming its bits;
previous literals in the batch have already been committed. Dynamic
blocks and short tails retain the established decoder. The trial selects
only native MSVC x64.

The [five-pair screen](https://github.com/bojosos/ptpng/actions/runs/38936495592)
on EPYC 7763 confirms native noise at 1.401x (1.352–1.417x) and noise to
RGB8 at 1.401x (1.360–1.419x). The
[nine-pair half-second repeat](https://github.com/bojosos/ptpng/actions/runs/38937169874)
on EPYC 9V45 confirms 1.492x (1.441–1.518x) and 1.490x
(1.372–1.567x), respectively. Every pair improves in both runs, and each
version's executable text is identical across the two machines.

There is a repeated tradeoff: Gray16-photo decoding is 0.979x in the
screen and 0.988x in the repeat, with every pair slower. That input has
61 dynamic blocks and no fixed blocks. Palette and RGBA16-graphics
encoding losses around 11% in the first screen do not repeat on the
second CPU; the repeat's graphics encoding ranges cross parity. These
controls still belong in the full combined comparison.

The [final seven-pair half-second comparison](https://github.com/bojosos/ptpng/actions/runs/38939461640)
returns to EPYC 7763 with the same executable text. Native noise improves
1.405x (1.302–1.542x), and noise to RGB8 improves 1.397x
(1.240–1.510x), but palette/RGB/RGBA16 graphics encoding falls to
0.906x/0.893x/0.886x with every pair slower. Gray8-photo decoding also
loses 2.1% in every pair. The 14-case decode mean is 1.026x, while the
nine-image encode mean is 0.962x. The encoding losses therefore repeat
on the original CPU and cannot be dismissed as timing noise. The
fixed-literal change and its compiler-selection overrides are excluded
from the retained ARM patch.

An additional section-placement trial keeps native MSVC x64 encoder
functions in `.text$deflate`, before the inflater's ordinary code section.
Hosted PE inspection verifies identical operational instructions in all
ten encoder functions and restores each entry's baseline offset modulo
64. The original fixed-width trial moved every later function by 304
bytes; the section contract prevents inflater growth from moving this
encoder block.

The [five-pair screen](https://github.com/bojosos/ptpng/actions/runs/38943228616)
on Xeon 8370C confirms native/RGB noise at 1.339x/1.313x. The integrated
[nine-pair half-second repeat](https://github.com/bojosos/ptpng/actions/runs/38944022718)
tests `099174c` against merged ARM main `c7dfd64` on all five platforms.
EPYC 7763 confirms noise at 1.390x/1.405x, with every pair faster.
Graphics encoding recovers to 1.001x/0.984x/1.003x for palette/RGB/RGBA16,
with all ranges crossing parity; the encode mean is 1.0005x.

Keep this rescue provisional: photo RGB-to-RGBA decoding is 0.967x
(0.899–0.990x), slower in all nine pairs. Gray16 photo is 0.952x
(0.903–1.020x), slower in eight pairs. The 14-case decode mean improves
1.034x, but these remaining photo costs prevent retaining the trial.
All 1,035 paired size tuples match, all four disabled-platform executable
code comparisons are identical, and the Windows executable text matches
the isolated screen. All eleven integrated
[CI](https://github.com/bojosos/ptpng/actions/runs/38943390752) and
[sanitizer fuzz jobs](https://github.com/bojosos/ptpng/actions/runs/38943395622)
pass. This branch preserves the experiment; it is not merged.

### Copy zero Paeth residual rows on Linux ARM64

For an all-zero Paeth residual row, reconstruction equals the previous
row: the first pixel selects above, then decoded-left equals upper-left,
so subsequent predictions select above as well. The RGBA16 graphics
fixture has 734 qualifying rows among its 768 Paeth rows. A bounded NEON
scan proves that the complete residual row is zero before copying the
previous row. A nonzero first vector rejects ordinary photo rows quickly.
The scan handles exact tails and overlapping source/output safely because
it completes before writing.

The shortcut selects only Linux GCC AArch64 and eight-byte pixels. A
separate dispatch wrapper keeps the existing predictor body and measured
placement intact. Windows, macOS and other compiler paths retain their
existing code. The first version placed the scan inside the kernel and
slowed ordinary rows slightly; the isolated wrapper removes those losses.

The [full nine-pair half-second confirmation](https://github.com/bojosos/ptpng/actions/runs/38937520542)
matches all 1,035 input/decoded/output size tuples. RGBA16 graphics
decoding improves 4.014x (3.980–4.034x), with every pair faster. RGBA8 and
RGBA16 photo decoding are 1.016x and 1.003x, Gray8 is 1.000x, and noise
is 1.002x (0.997–1.004x). No ARM decode workload loses in every pair;
the 14-case decode geometric mean is 1.107x. The four disabled platforms
have identical baseline/candidate executable code, despite timing noise.

The final seven-pair comparison above repeats the ARM graphics gain at
4.041x (4.022–4.070x). RGBA8 and RGBA16 photo decode improve 1.017x
and 1.004x; the decode mean is 1.109x and the encode mean is 0.999x.
No ARM workload loses in every pair. All 805 paired size tuples across
five platforms match. This cross-check includes the subsequently held
Windows experiment; its Linux ARM64 executable is identical to the
ARM-only confirmation. The retained source keeps Windows unchanged.

### Rejected alternatives

These experiments remain outside the retained source. Initial full
screens use five alternating 0.3-second pairs on all five platforms;
confirmations use nine half-second pairs.

| Trial | Hosted evidence | Decision |
| --- | --- | --- |
| Pack four Windows literal stores into one word | [38936041157](https://github.com/bojosos/ptpng/actions/runs/38936041157) | Reject. Native noise is 1.005x with a 0.907–1.007x range; Gray16 decode loses 1.1% in every pair. Added packing instructions do not establish a reliable gain. |
| Reassociate NEON Paeth arithmetic | [38935767854](https://github.com/bojosos/ptpng/actions/runs/38935767854) | Reject. Linux ARM64 RGBA16 graphics decoding falls to 0.918x; the compiler adds a serial arithmetic operation. |
| Isolate Linux IDAT materialization in a separate helper | [38936179431](https://github.com/bojosos/ptpng/actions/runs/38936179431) | Reject. Linux x64 target photo cases stay near parity; Linux ARM64 Gray8/Gray16 photos develop repeatable losses. |
| Align the Linux GCC x64 inflater to 64 bytes | [38936941309](https://github.com/bojosos/ptpng/actions/runs/38936941309) | Reject. Photo gains of 0.9–1.3% on EPYC 7763 accompany graphics encoding ratios of 0.847–0.855x. |
| Also align Linux GCC x64 compression to 32 bytes | [screen](https://github.com/bojosos/ptpng/actions/runs/38937803489), [confirmation](https://github.com/bojosos/ptpng/actions/runs/38938564459) | Reject. The Xeon 8573C screen recovers encoding, but EPYC 9V45 repeats RGB/Gray16-photo decode at 0.970x/0.961x with every pair slower. Each executable is byte-identical across runs, so the result depends on the measured CPU rather than build drift. |

The remaining small Linux x64 photo and Gray16-encoding losses recorded
below are unresolved. Restoring a previous instruction alignment is not
sufficient evidence of a portable speedup.

### Correctness coverage

The fixed-code change passes 22,572 valid, truncated and mutated stream
comparisons against the baseline and independent zlib checks, including
the actual Windows noise fixture. Regression tests independently encode
all 256 literal values at each batch position. Existing tests exercise
partial-batch EOB, reserved symbols, matches, exact output capacities and
every truncated input prefix. Native MSVC AddressSanitizer also passes
10,000 additional mutations of a fixed-code PNG.

Paeth regressions compare the selected kernel with scalar reconstruction
at row-length boundaries from zero through 4,096 bytes, all 16 alignments,
six source/output overlap gaps and exact allocations. They include zero
rows, late nonzero tails, and a nonzero in each of the four vectors after
the initial zero prefix. The new Linux GCC ARM64 ASan/UBSan CI job exercises
the actual production shortcut. During the Windows experiment, Clang x64
sanitizer and fuzz jobs explicitly enabled the fixed-code batch so the
compiler guard could not hide that algorithm from those checks. Those
overrides are held with the Windows trial.

The [combined-source sanitizer campaigns](https://github.com/bojosos/ptpng/actions/runs/38938683343)
complete 1,304,062 executions across eight campaigns without failure.
Eight local C/C++ tests also pass after the late-vector regression cases
are added. The retained ARM library source matches the ARM-only
confirmation; the later additions only strengthen its tests and CI.
The final ARM-only checks are recorded in the
[nine-job CI run](https://github.com/bojosos/ptpng/actions/runs/38940484598)
and [sanitizer campaigns](https://github.com/bojosos/ptpng/actions/runs/38940552379).

## 16 October nightly investigation

The [nightly on `f698c88`](https://github.com/bojosos/ptpng/actions/runs/38869992861)
passes all five platforms. All 135 decode comparisons beat libpng with
zlib-ng. The within-run geometric means are:

| Platform | Decode, 27 cases | Encode against level 6, nine images |
| --- | ---: | ---: |
| Linux x64 | 1.664x | 1.943x |
| Linux ARM64 | 1.607x | 1.445x |
| macOS ARM64 | 1.633x | 1.443x |
| Windows x64 | 1.738x | 1.723x |
| Windows ARM64 | 1.491x | 1.675x |

Windows x64 noise remains a narrow lead: native/RGBA8/RGB8 decode is
1.046x/1.047x/1.069x. Windows ARM64 native noise is 1.027x. RGBA16-photo
encoding is another weak spot, at 1.004x on Windows x64 and 0.861x on macOS.
These ratios compare implementations within one run; they do not identify
code regressions between different nightly machines.

The [fresh profiles](https://github.com/bojosos/ptpng/actions/runs/38929309789)
retain the Linux inputs and executable files. Input compression differs by
platform: Linux noise is a 3,147,615-byte stored stream, while the Windows
noise input is 3,319,878 bytes and uses Huffman compression. The local Windows
fixture matches the latter size. A Windows VTune profile of that fixture
attributes about 80% of samples to inflation; the Linux x64 profile instead
attributes 32% to a libc memory routine, 30% to Paeth, 22% to CRC and 10% to
Adler. Optimizations for one stream must not be attributed to the other.

### Read fragmented stored streams directly

Linux's noise input has 49 IDAT chunks and 96 stored blocks. Previously the
decoder allocated a contiguous compressed buffer, copied all IDAT payloads
into it, then copied stored payloads into the raw image buffer. The new
internal reader copies stored payloads directly from the parsed chunk spans.
It retains the zero-copy input path for a single IDAT chunk.

Success requires a valid zlib header, complementary stored-block lengths,
exact output size, the final block, the Adler checksum unless verification
is disabled, and no trailing data. Empty chunks and splits within any header,
payload or checksum are accepted. Unsupported or malformed streams return
to the existing inflater; any partially written raw output is disposable.
The general inflater retains responsibility for the normal error result.

The [first full comparison](https://github.com/bojosos/ptpng/actions/runs/38930195879)
tests `93948c7` against `f698c88`, five alternating 0.3-second pairs for all
23 workloads on five platforms. All 575 input/decoded/output size tuples
match. Linux ARM64 native noise improves 2.263x (2.261–2.277x), and Linux x64
2.673x (2.611–2.736x). Windows and macOS do not establish a noise gain.
The [seven-pair half-second repeat](https://github.com/bojosos/ptpng/actions/runs/38931036409)
confirms native noise at 2.270x on Linux ARM64 and 2.627x on Linux x64,
but also repeats a 1.5% Linux ARM64 RGBA8/Gray8-photo loss.

ELF inspection finds unchanged inflater instructions apart from relocations,
but its address moves by 112 bytes. The first literal lookup changes from
offset 44 to offset 60 within a 64-byte cache line, crossing the boundary
immediately. This is a plausible placement effect, not a proven cause of
the entire loss. A revised caller branches directly to unfiltering after
stored success and preserves the original general-inflate block verbatim.
Its [full comparison](https://github.com/bojosos/ptpng/actions/runs/38932126120)
restores Linux ARM64 Gray8 to 1.000x and improves RGBA8 to 0.994x, while
native noise remains 2.278x. Some other small losses remain. This universal
caller also regresses Windows x64 graphics encoding on the next runner,
so the retained stored shortcut is selected only on Linux. The disabled
Windows caller and inflater restore byte-identical local executable text.

Eight local C/C++ tests pass. New public-decoder regressions cover every
two-way split, empty IDAT chunks, nonzero stored padding, mixed stored/fixed
blocks, truncation, invalid complements, output-size mismatches, preset
dictionaries, bad Adler checksums and trailing bytes. A separate 2,942-case
differential experiment matches baseline status, size and pixels, including
860 valid stored cases checked against independent expected pixels and
maximum 65,535-byte blocks. The matching
[CI](https://github.com/bojosos/ptpng/actions/runs/38930201372) and
[sanitizer fuzzing](https://github.com/bojosos/ptpng/actions/runs/38930206944)
pass; the latter completes 1,799,984 executions across eight campaigns.
Seven additional stored PNG seeds keep fragmented decoding reachable in
future fuzz campaigns, with independent zlib checks on their raw pixels.
The [seeded campaign](https://github.com/bojosos/ptpng/actions/runs/38931237302)
passes another 1,248,123 executions.

### Hoist the literal-batch bit guard on Linux ARM64

The existing batch decodes up to four root-table literals. A single guard
requiring at least four times the ten-bit root width proves that all four
literal code lengths fit. Three subsequent bit-count comparisons become
redundant. The batch width, Huffman tables, output checks, refill before
matches and checked short-tail path are preserved.

The [universal trial](https://github.com/bojosos/ptpng/actions/runs/38931650137)
has Linux x64 and Windows ARM64 regressions. Windows x64 noise-to-RGB
initially improves 1.014x, but the
[nine-pair repeat](https://github.com/bojosos/ptpng/actions/runs/38932584216)
does not confirm it: native noise becomes 0.996x with every pair slower.
The two Windows runs use EPYC 9V45 and 9V74 with identical respective
baseline/candidate executable text. Windows therefore retains the original
loop. Linux GCC AArch64 repeats RGBA8-photo decode at 1.013x
(1.012–1.014x), RGBA16 photo at 1.016x (1.013–1.018x), and RGBA8-to-RGB
at 1.013x (1.010–1.015x); every pair improves in all three cases.

The retained guard selects only Linux GCC AArch64. Other compilers retain
the original conditions verbatim. The loop passes six core tests, 22,572
valid/truncated/mutated stream comparisons and independent zlib checks.
The universally enabled loop also passes
[707,423 sanitizer executions](https://github.com/bojosos/ptpng/actions/runs/38931655076),
including Clang builds before the compiler selection was narrowed.

### Combined final-source validation

Source `fdc6187` combines the Linux stored shortcut and Linux GCC AArch64
batch guard on top of `f698c88`. The complete comparison uses seven
alternating half-second pairs for all 23 workloads on each platform:
[paired results](https://github.com/bojosos/ptpng/actions/runs/38933419955).
Matching source validation is recorded in the
[CI run](https://github.com/bojosos/ptpng/actions/runs/38933427114) and
[seeded sanitizer campaigns](https://github.com/bojosos/ptpng/actions/runs/38933433085).
All eight CI jobs and both sanitizer jobs pass; the campaigns complete
1,456,927 executions. All 805 paired input/decoded/output size tuples match.
Median candidate/baseline throughput ratios are:

| Workload | Linux x64 | Linux ARM64 |
| --- | ---: | ---: |
| Noise native decode | 3.187x | 2.266x |
| Noise to RGB8 | 2.988x | 2.196x |
| Gray8 photo decode | 0.990x | 1.020x |
| RGBA8 photo decode | 0.989x | 1.007x |
| RGBA16 photo decode | 0.989x | 1.011x |
| RGBA16 graphics decode | 1.002x | 0.985x |
| Decode geometric mean, 14 paired workloads | 1.171x | 1.123x |
| Encode geometric mean, nine images | 0.997x | 1.006x |

The remaining tradeoffs are explicit: Linux x64 RGBA8/Gray8/RGBA16 photo
decode and RGBA8-to-RGB lose about 1.0–1.1% in every pair, and Gray16-photo
encoding loses 1.4%. Linux ARM64 RGBA16 graphics decode loses 1.5% in every
pair. The large noise gain outweighs these losses in the measured decode
geometric means; the patch does not improve every workload.

Windows and macOS retain their original decoder code paths. The final
macOS executable text matches baseline byte-for-byte. Linux x64 text also
matches the previous caller-only source, confirming that the disabled
ARM literal guard introduces no x64 code or placement change. These results
cover the measured fixtures and hosted CPUs, rather than arbitrary inputs
or every compiler and processor.

### Other isolated trials

Each initial comparison uses five alternating 0.3-second pairs for all
23 workloads on all five platforms, against `f698c88`.

| Trial | Paired run | Decision |
| --- | --- | --- |
| Reassociate the AVX2 Paeth arithmetic | [38929563943](https://github.com/bojosos/ptpng/actions/runs/38929563943), [repeat](https://github.com/bojosos/ptpng/actions/runs/38930313008) | Reject. Windows x64 noise is 0.996x in the nine-pair half-second repeat; the earlier RGBA8-to-RGB gain does not repeat. |
| Replace dependent Paeth blends with independent masks | [38929773119](https://github.com/bojosos/ptpng/actions/runs/38929773119) | Reject. RGBA16 graphics decode falls to 0.886x on Windows x64 and 0.850x on Linux x64. |
| Process 64 bytes per AVX2 Adler iteration | [38929990231](https://github.com/bojosos/ptpng/actions/runs/38929990231) | Leave experimental. Windows x64 noise is 1.003x (0.998–1.131x); Linux x64 is 1.005x (0.995–1.007x). No stable target gain. |
| Pair adjacent literal-code writer updates | [38930119513](https://github.com/bojosos/ptpng/actions/runs/38930119513), [MSVC x64 repeat](https://github.com/bojosos/ptpng/actions/runs/38931650041) | Reject. Initial Windows photo gains fail to repeat: RGBA16 photo becomes 0.973x and graphics 0.888–0.913x. |
| Pair match length and distance writer updates | [38930510964](https://github.com/bojosos/ptpng/actions/runs/38930510964) | Leave experimental. Windows x64 RGB-photo encoding improves 2.1%, but Linux x64 RGBA16 and macOS Gray16 regress. |

The checksum trial passes 31,728 independent checksum cases and
[730,834 sanitizer fuzz executions](https://github.com/bojosos/ptpng/actions/runs/38929995754).
The CRC helper already inlines in MSVC; its apparent VTune helper calls are
inline-frame attribution. Both encoder trials preserve exact bytes for
2,108 raw streams and eleven PNGs, including the portable bit writer.
The literal repeat's disabled-platform executables or text sections match
baseline exactly. Its Windows x64 baseline and candidate code also match
their respective original-run code. The first run uses EPYC 9V74, while
the repeat uses EPYC 7763: hardware-dependent behavior is plausible, and
the first apparent gain is insufficient for a general MSVC x64 selection.

## 15 October continued investigation

The [refreshed nightly](https://github.com/bojosos/ptpng/actions/runs/38757881206)
tests merged `90e0c93`. All five platforms pass correctness checks. Its
within-run geometric mean ratios against libpng with zlib-ng are:

| Platform | Decode speedup, 27 cases | Encode speedup against level 6, nine images |
| --- | ---: | ---: |
| Linux x64 | 1.890x | 1.888x |
| Linux ARM64 | 1.577x | 1.368x |
| macOS ARM64 | 1.636x | 1.448x |
| Windows x64 | 1.615x | 1.680x |
| Windows ARM64 | 1.477x | 1.498x |

133 of 135 decode comparisons beat the reference. Windows x64 noise is
0.979x in native output and 0.981x in RGBA8; RGB8 is 1.002x. Windows ARM64
noise remains close at 1.025x. These within-run ratios do not establish a
code regression against a previous nightly on another virtual machine.

The [new profiles](https://github.com/bojosos/ptpng/actions/runs/38757887735)
use this same revision and retain their actual PNG inputs. Linux ARM64
attributes 79.2% of palette-to-RGB self samples to scalar conversion and
53.8% of RGB-graphics decode to scalar Paeth. RGB-photo encoding still
attributes about 80% to long matching on both Linux architectures.

### ARM palette conversion

The retained palette converter reads four independent packed colors,
removes alpha with a NEON byte lookup, and writes exactly twelve bytes.
Its tail writes exactly three bytes per remaining pixel. It accepts an
unaligned palette and introduces no source or destination overread.
Decoded pixels and encoder output remain identical to the baseline.

The [first comparison](https://github.com/bojosos/ptpng/actions/runs/38758143415)
finds palette-to-RGB median gains of 9.0% on Linux ARM64, 10.8% on Windows
ARM64 and 16.6% on macOS. However, Linux ARM64 RGBA8 decode loses about 1.1%.
The [nine-pair repeat](https://github.com/bojosos/ptpng/actions/runs/38759001922)
reproduces the RGBA8 loss in every pair, despite its unchanged filter body.

The new static kernel had moved GCC's existing Paeth function from
`0x12cb0` to `0x12d80`. Giving only the new kernel its own GCC text
subsection restores Paeth to `0x12cb0`, with identical size and bytes.
The [seven-pair confirmation](https://github.com/bojosos/ptpng/actions/runs/38759942686)
then measures Linux ARM64 palette conversion at 1.095x, with RGBA8 native
at 1.000x and RGB8 at 0.999x. Windows ARM64 palette conversion is 1.111x,
with every pair between 1.092 and 1.127x; its RGBA8 paths are 1.004x and
1.001x. macOS remains noisier. x64 retains its original implementation.

The complete final-source comparison is
[38760482803](https://github.com/bojosos/ptpng/actions/runs/38760482803).
All eight jobs in the final-source
[CI run](https://github.com/bojosos/ptpng/actions/runs/38760487793) pass.
Existing conversion tests cover independent expected palette colors,
short lengths, all alignments and output canaries; platform CI also
checks independent libpng parity. The original kernel's
[fuzz run](https://github.com/bojosos/ptpng/actions/runs/38758977459)
passes 930,135 executions across four targets on x64/ARM64 without a
reported sanitizer finding. The final source changes only GCC placement
from that kernel; PR checks exercise the complete final revision again.

### Additional isolated trials

The following full comparisons use `90e0c93`, five alternating 0.3-second
pairs for each of 23 workloads, and all five platforms. Correct output is
necessary but does not make an optimization useful.

| Candidate | Paired run | Decision |
| --- | --- | --- |
| NEON Paeth for three- and six-byte pixels | [38758398415](https://github.com/bojosos/ptpng/actions/runs/38758398415) | Reject. RGB-graphics native decode falls to 0.396x on Linux ARM64, 0.474x on Windows ARM64 and 0.659x on macOS. |
| Three independent ARM CRC chains | [38758485783](https://github.com/bojosos/ptpng/actions/runs/38758485783) | Reject. Joining the chains does not improve noise decode; Linux ARM64 noise is 0.991x with every pair slower. |
| Nine-bit inflater literal root | [38758281304](https://github.com/bojosos/ptpng/actions/runs/38758281304) | Reject. RGBA16-photo decode falls to 0.881x on Linux ARM64, 0.837x on Linux x64 and 0.860x on Windows x64. |
| Eleven-bit inflater literal root | [38759372387](https://github.com/bojosos/ptpng/actions/runs/38759372387) | Leave experimental. Linux RGBA8 improves 5.0–6.7%, but RGB/Gray16/RGBA16 losses remain and macOS graphics lose 5.4%. |
| Fixed-width Huffman bit reversal | [38760619492](https://github.com/bojosos/ptpng/actions/runs/38760619492) | Reject. Many Linux ARM64 decode workloads lose 0.4–1.0% with every pair slower. |
| Long-chain deltas and prefix tags | [38759138215](https://github.com/bojosos/ptpng/actions/runs/38759138215) | Reject. Linux x64 graphics encode falls to 0.454–0.465x; Linux ARM64 graphics to 0.860–0.863x. |
| Gate the exact Kraft stored-block bound | [38758231989](https://github.com/bojosos/ptpng/actions/runs/38758231989) | Leave experimental. Linux noise encode improves 3.3–4.5% in every pair, but overall encoder means are near parity and Linux ARM64 Gray16 loses 0.3%. |
| Replace the short predecessor ring with one link | [38758316218](https://github.com/bojosos/ptpng/actions/runs/38758316218) | Refine. Linux improves, but Windows ARM64 graphics lose about 4%. |
| Preserve the old state layout with one active link | [38759917748](https://github.com/bojosos/ptpng/actions/runs/38759917748) | Reject as a fix. Windows ARM64 graphics still lose about 4%; moving tokens or shrinking allocation does not explain that loss. |
| Also suppress unused interior-link updates | [38761179159](https://github.com/bojosos/ptpng/actions/runs/38761179159) | Retain the mechanism with platform selection below. Windows ARM64 graphics improve 23–30%; Linux graphics improve 6–14%. |

The CRC candidate passes native checksum comparisons and independent raw-state
composition checks, but adds work without a measured speedup. The Kraft gate
only decides whether to attempt an exact lower-bound proof; it never chooses
a stored block heuristically. Its raw streams, PNGs, mixed inputs and bound
checks remain correct. Neither candidate is included in the retained source.

### Eliminate unused single-candidate links

With a one-candidate short search, only the latest search-position insertion's
predecessor can be read. Replace the 32,768-entry predecessor ring with one
link, saving 131,068 bytes of internal state. Interior insertions still update
both hash heads and the long-match chain, but skip the unused old-head read
and scalar predecessor store. Search and lazy-lookahead insertions save the
link before any short search. Window limits, token choices, search budgets
and encoded bytes are preserved.

The [nine-pair graphics repeat](https://github.com/bojosos/ptpng/actions/runs/38761823294)
confirms the Linux and Windows ARM64 gains. macOS does not establish a gain,
so it retains the original ring implementation. Budgets above one also keep
the ring. These baseline paths retain the original four-argument insertion
declaration and calls: an earlier unused extra inline argument had changed
MSVC x64's generated code. Their normalized preprocessor output matches the
merged baseline exactly.

Final source `70bb0e3` starts from `cc7b3fc`, including the merged palette
converter. Its [complete comparison](https://github.com/bojosos/ptpng/actions/runs/38762816430)
uses seven alternating half-second pairs on all five platforms.
The matching [CI](https://github.com/bojosos/ptpng/actions/runs/38762822821)
and [coverage fuzzing](https://github.com/bojosos/ptpng/actions/runs/38762828278)
test that source. Local checks pass six core tests in each of three
configurations: MSVC's default budget two, optimized budget one, and Apple's
ring path at budget one. Each also independently passes 2,108 exact raw-stream
comparisons and eleven exact PNG comparisons against the corresponding
updated-main build. These measured fixtures do not constrain arbitrary
inputs or every compiler's performance.

## 15 October parallel experiments

This round starts from `56bed94`, including the two-candidate short matcher
from the earlier 15 October work. Nine isolated candidates each receive
five alternating 0.3-second pairs for 23 workloads on five platforms.
All 5,175 pairs have matching input and decoded sizes. Eight candidates
preserve encoder sizes; the one-candidate search intentionally changes them.

| Experiment | Paired run | Outcome |
| --- | --- | --- |
| Rotate output/scratch buffers after a filter improves | [38700992169](https://github.com/bojosos/ptpng/actions/runs/38700992169) | Encoder mean improves 0.8–2.7%; retained outside MSVC x64 after the combined checks. |
| Fuse filtering and scoring | [38701277897](https://github.com/bojosos/ptpng/actions/runs/38701277897) | Excluded. Linux x64 graphics slow 6.8–8.0%, with every pair slower. |
| Reuse short-match prefix loads and prune searches | [38701153785](https://github.com/bojosos/ptpng/actions/runs/38701153785) | Excluded. Mixed photo gains and consistent Linux ARM graphics losses. |
| Skip long-match bytes already proved equal | [38701549965](https://github.com/bojosos/ptpng/actions/runs/38701549965) | Excluded. Windows x64 RGB-photo encoding loses 5%, with every pair slower. |
| Keep only short-search pruning | [38701902018](https://github.com/bojosos/ptpng/actions/runs/38701902018) | Excluded. Linux ARM graphics remain 0.7–0.9% slower. |
| Reuse Huffman heap order instead of sorting leaves again | [38701144117](https://github.com/bojosos/ptpng/actions/runs/38701144117) | Excluded. Linux x64 graphics slow 7.4–8.8%. |
| Prove stored blocks cheaper using a Kraft lower bound | [38701953703](https://github.com/bojosos/ptpng/actions/runs/38701953703) | Left experimental. Linux/Windows noise improves 2.2–5.8%, but some photos lose 0.6–1.1%. |
| Decode six literals per refill | [38701108394](https://github.com/bojosos/ptpng/actions/runs/38701108394) | Excluded. Linux x64 RGBA16-photo decode loses 2.4%, with all five pairs at 0.974–0.976x. |
| Search one short-match candidate instead of two | [38702129642](https://github.com/bojosos/ptpng/actions/runs/38702129642) | Encoder mean improves 3.3–5.8%; retained outside MSVC x64 after the combined checks. |

All comparisons use immutable baseline/candidate revisions and shared inputs.
Linux and Windows pin both versions to one logical CPU; macOS remains OS
scheduled and has substantially wider pair ranges. Removing source-level
work does not guarantee faster generated code; the rejected changes above
are not included in the combined candidate.

The [fresh baseline profiles](https://github.com/bojosos/ptpng/actions/runs/38700687772)
use hardware cycles on ARM64 and software timer samples on x64. RGB-photo
encoding still attributes 79.1%/84.0% of self samples to long matching on
ARM64/x64. Noise attributes 25.4%/27.2% to long matching and 20.1%/27.7% to
short matching. Inlined compression work accounts for another 49.1%/40.0%.
ARM assembly annotations point to dictionary insertion stores as a large
part of that inlined work. These are sampled costs, including setup and
warm-up, not stage timings.

### Combined measurements and platform selection

The first combined candidate sets `DEFLATE_SHORT_CHAIN` to one and alternates the
existing output and scratch rows when a filter improves. At most one final
row copy is needed. Filter scores, tie choices and first-row rules stay the
same. A separate byte comparison confirms that buffer rotation alone leaves
all 2,108 raw test streams and eleven complete PNGs identical. The combined
encoder is also byte-identical to the isolated one-candidate encoder on
those inputs. Long matching retains its 64-candidate cap.

The [combined comparison](https://github.com/bojosos/ptpng/actions/runs/38702421393)
tests `fb3cafd` against `56bed94`, with seven alternating half-second pairs
per workload. Median candidate/baseline throughput ratios are:

| Platform | Gray8 photo encode | RGBA16 photo encode | Noise encode | Nine-image encode geometric mean |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 1.147x | 1.156x | 1.103x | 1.072x |
| Windows x64 | 1.136x | 1.130x | 1.049x | 1.049x |
| Linux ARM64 | 1.161x | 1.168x | 1.151x | 1.068x |
| macOS ARM64 | 1.125x | 1.034x | 1.067x | 1.043x |
| Windows ARM64 | 1.138x | 1.201x | 1.086x | 1.051x |

Every pair improves for Gray8-photo and noise encoding on all five platforms.
RGBA16-photo encoding improves in every Linux/Windows pair. macOS RGBA16
varies from 0.446–1.261x, and RGBA8-photo encoding is near parity at 0.998x;
its timing variation limits conclusions about those cases.

The full run also has losses. On the Windows x64 Xeon Platinum 8573C,
RGB-native decode measures 0.980x and Gray8-native decode 0.967x, with every
pair slower. Linux ARM palette-to-RGB decode is 0.983x, with every pair
slower. Windows ARM RGB-graphics encoding is 0.997x, with all seven pairs
0.992–0.999x. Across platforms, decoder geometric means range 0.994–1.014x.

The [focused repeat](https://github.com/bojosos/ptpng/actions/runs/38703309747)
uses nine half-second pairs for RGB photo, Gray8 photo and palette graphics.
On Windows x64 EPYC 7763, Gray8 decode remains slower at 0.975x, with every
pair between 0.957–0.984x. Palette-graphics encoding is 0.926x, with every
pair between 0.858–0.950x. Linux ARM palette-to-RGB decode returns to 1.002x;
that earlier loss is not reproduced. These results rule out applying the
combination universally.

The retained source `7b6fdc3` keeps MSVC x64's original row selection and
two-candidate short search. Other targets use buffer rotation and one short
candidate. Local MSVC validation confirms that all 2,108 raw streams and
eleven complete PNGs match the original two-candidate baseline byte-for-byte.
The decoder sources retain their original implementation on every target.

The [guarded confirmation](https://github.com/bojosos/ptpng/actions/runs/38704163237)
compares `7b6fdc3` with `56bed94`, again using seven half-second pairs.
The four optimized targets measure:

| Platform | Gray8 photo encode | RGBA16 photo encode | Noise encode | Nine-image encode geometric mean |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 1.126x | 1.144x | 1.082x | 1.047x |
| Linux ARM64 | 1.130x | 1.174x | 1.156x | 1.062x |
| macOS ARM64 | 1.129x | 1.138x | 1.070x | 1.056x |
| Windows ARM64 | 1.119x | 1.155x | 1.065x | 1.043x |

Some small losses remain: Windows ARM64 RGBA16-graphics encoding is 0.995x,
with every pair between 0.992–1.000x and below parity before rounding.
The four optimized decoder means are 0.999–1.005x. MSVC x64 is a control
with its baseline code path, measuring 1.008x encoder and 0.993x decoder
means. Its Gray8 decode is 1.017x with a 0.932–1.071x pair range, and
palette-graphics encoding is 0.965x with a 0.942–1.059x range. The earlier
every-pair losses are not reproduced, but the control still shows timing
variation. No new MSVC x64 speedup is claimed.

All 805 guarded pairs have matching input and decoded sizes. Encoder sizes
are consistent across repetitions and within each of the two default paths.
MSVC x64 matches all baseline sizes; the other four platforms agree with the
one-candidate sizes below.

Relative to the two-candidate baseline, the Gray8 PNG shrinks by 314 bytes,
RGB grows by 486 bytes, RGBA16 grows by 9,864 bytes and RGBA8 grows by 91,750
bytes on the optimized targets. RGBA8's 0.779% is the largest relative growth;
the other five fixtures retain their sizes. MSVC x64 retains its baseline
sizes, so encoded bytes now differ between these default build paths.
Independent zlib decoding verifies identical filtered pixels for all eleven
benchmark PNG inputs, including interlaced sources.
All nine fixtures remain within 2.1% of the previous nightly's zlib-ng
level-6 sizes. That fixture bound does not constrain arbitrary images.

All 805 unguarded timing pairs retain matching input and decoded sizes, and
all 315 encoder sizes in that run agree across platforms. The one-candidate
build passes nine local CTest checks, including libpng parity and 1,660 raw
streams independently decoded with zlib. All eight jobs in the combined
[CI run](https://github.com/bojosos/ptpng/actions/runs/38702424522) pass.
The combined [fuzz run](https://github.com/bojosos/ptpng/actions/runs/38702428184)
passes 1,832,608 executions across four targets on x64/ARM64, 60 seconds per
target, without a reported sanitizer finding. It exercises the Linux paths
retained by the MSVC guard. All eight jobs in the guarded build's
[CI run](https://github.com/bojosos/ptpng/actions/runs/38704168748) also pass.
These bounded checks and
synthetic fixtures do not establish performance or correctness on every input.

The stored-block lower bound remains a possible follow-up if a cheap
frequency check can avoid evaluating it on photo blocks where it fails.
Its exact version skips tree construction for 63 of 96 noise blocks and
passes 4,000 checks against actual Huffman costs. Repeated random patterns
and mixed binary/noise inputs remain necessary quality checks before any
heuristic early stored-block decision.

## 15 October follow-up

The [14 October nightly](https://github.com/bojosos/ptpng/actions/runs/38534614560)
tests merged `4640b81`. All five platform jobs passed. Its geometric mean
speedups against libpng + zlib-ng are:

| Platform | Decode speedup | Encode speedup vs level 6 |
| --- | ---: | ---: |
| Linux x64 | 1.847x | 1.644x |
| Windows x64 | 1.713x | 1.639x |
| Linux ARM64 | 1.570x | 1.183x |
| macOS ARM64 | 1.649x | 1.252x |
| Windows ARM64 | 1.470x | 1.350x |

All 135 decoder comparisons beat the reference in this run. The narrowest
margins remain Windows noise decoding, at 1.034x on x64 and 1.025x on ARM64.
These are within-run reference comparisons; runner changes prevent attributing
raw timing differences from the previous nightly to code changes.

Gray8 and RGBA16 photo encoding still lose on every platform. Gray8 runs at
0.754–0.917x the reference throughput but produces 6.7% smaller files.
RGBA16 runs at 0.612–0.875x and produces 0.8% larger files. Noise encoding
also trails the reference on Linux ARM64 and macOS, at 0.919x and 0.766x.
These remain useful targets after the earlier RGB-photo improvements.

Exploratory local VTune software profiles attribute 37.6% of Gray8 encoder
CPU samples and 38.8% of RGBA16 samples to `deflate_short_match`. The profiles
include setup and warm-up. They identify sampled function costs, not isolated
stage durations. Early local timing experiments ran with the laptop unplugged;
their speed ratios are excluded from the retained measurements.

### Short-match search budget

The candidate caps three-to-five-byte match searches at two candidates instead
of four. Long matching retains its 64-candidate cap, and filter scoring and
block-cost selection retain their existing behavior. `DEFLATE_SHORT_CHAIN=4`
can restore the previous short-search budget at build time. This changes some
match choices and encoded bytes while preserving decoded pixels.

After connecting AC power, seven alternating half-second pairs on the local
i7-1355U, pinned to logical CPU 2, measure 1.221x Gray8-photo encoding,
1.117x RGBA16-photo encoding and 1.089x RGBA8-photo encoding. Every pair
improves in those three cases. The nine-image encoder geometric mean is
1.058x; RGB-photo encoding is 1.002x. The fourteen decoder controls average
1.001x, with individual conversion pairs still varying broadly. Local
noise-to-RGB decode has a 0.960x median and 0.916–1.092x pair range.

The [five-platform comparison](https://github.com/bojosos/ptpng/actions/runs/38699602784)
tests `2c3c676` against merged `4640b81`. Each of 23 workloads has seven
alternating pairs with at least 0.5 seconds per timed loop. Linux and Windows
pin both versions to one logical CPU; macOS is OS scheduled. Median
candidate/baseline throughput ratios are:

| Platform | Gray8 photo encode | RGBA16 photo encode | RGBA8 photo encode | Nine-image encode geometric mean |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 1.338x | 1.126x | 1.104x | 1.059x |
| Windows x64 | 1.265x | 1.123x | 1.088x | 1.052x |
| Linux ARM64 | 1.241x | 1.102x | 1.078x | 1.048x |
| macOS ARM64 | 1.274x | 1.243x | 1.083x | 1.080x |
| Windows ARM64 | 1.211x | 1.075x | 1.078x | 1.042x |

Every pair improves in these three photo cases on Linux and Windows. macOS
has wider variation, with Gray8 pairs ranging 0.834–1.414x and RGBA16 pairs
0.887–1.307x. Its RGBA8 pairs also include a loss. These macOS medians are
less precise than the pinned-platform results.

There are small losses elsewhere. Linux ARM64 palette-graphics encoding is
0.996x, with all seven pairs between 0.994–1.000x and below parity before
rounding. Windows x64's RGB native decode and RGB-graphics-to-RGBA decode
measure 0.954x and 0.965x, with ranges of 0.779–1.033x and 0.948–1.126x.
Those broad decoder ranges include improvements; the decoder geometric means
across platforms range 0.994–1.006x. These measurements support the targeted
photo-encoder gains; other workloads have smaller changes or losses.

All 805 pairs have matching input and decoded sizes, and all 315 candidate
encoder-size observations agree across platforms. Gray8 output shrinks by 108
bytes, RGB grows by 548 bytes, RGBA8 grows by 36,866 bytes and RGBA16 grows by 8,006
bytes. The largest relative growth is RGBA8's 0.314%; the three graphics
fixtures, Gray16 photo and noise retain their sizes. All nine remain at most
2.055% larger than the nightly's zlib-ng level-6 reference. That bound applies
to these synthetic fixtures, not arbitrary images.

Local validation passes all nine CTest tests, including libpng parity and
independent zlib checks. Raw compression tests now include nearby three- and
five-byte repeats among random bytes, with short inputs, window wraps and
block boundaries. All 1,660 streams pass both decoders. Additional differential
checks verify 2,108 raw streams with independent zlib and eleven complete PNG
fixtures with identical decompressed IDAT bytes. Those raw samples retain
identical compressed bytes; the photo PNG changes above are intentional.

All eight jobs in [CI run 38699606928](https://github.com/bojosos/ptpng/actions/runs/38699606928)
pass, including the six native compiler/platform jobs and Linux ASan/UBSan.
[Fuzz run 38699610855](https://github.com/bojosos/ptpng/actions/runs/38699610855)
completes 1,756,089 executions across decode, inflate, PNG encode and raw
DEFLATE on x64/ARM64, 60 seconds per target, without a reported sanitizer
finding. These remain bounded checks on synthetic and generated inputs.

Cost hoisting, explicit short-match byte checks, search pruning, forced
inlining and tagged short links remain experimental. Their early unplugged
timings do not establish a gain, and tagged links add 384 KiB of per-call
state. They are excluded from this change. Further work should profile
current ARM noise encoding before trying an early stored-block decision;
mixed compressible/noisy inputs need size checks before such a decision can
replace matching. Real application images also need validation before
generalizing the short-search tradeoff beyond these fixtures.

## 14 October follow-up

The [forced nightly](https://github.com/bojosos/ptpng/actions/runs/38465890719)
tested the merged `6b2bcfb` source. The earlier 13 October scheduled run
tested `9a1dadf`, before PR #1 merged. Both nightlies and the merge's CI
and fuzzing passed. The new nightly's within-run geometric mean speedups
against libpng + zlib-ng are:

| Platform | Decode speedup | Encode speedup vs level 6 |
| --- | ---: | ---: |
| Linux x64 | 1.812x | 1.522x |
| Windows x64 | 1.613x | 1.479x |
| Linux ARM64 | 1.611x | 1.147x |
| macOS ARM64 | 1.649x | 1.192x |
| Windows ARM64 | 1.464x | 1.256x |

These use 27 decoder cases and nine encoder cases. Photo encoders still
lose some comparisons, and Windows noise decoding remains slightly behind
the reference. Different nightly CPUs prevent attributing raw timing
changes to the merge.

The [same-machine merge comparison](https://github.com/bojosos/ptpng/actions/runs/38465894004)
tests `6b2bcfb` against its `9a1dadf` first parent. Each of 23 workloads has
seven alternating pairs with at least 0.5 seconds per timed loop. Linux
and Windows pin both versions to one logical CPU; macOS is OS scheduled.
All 805 output-size pairs match, including 315 encoder pairs. The values
below are median candidate/baseline throughput ratios:

| Platform | RGB photo encode | Gray16 photo encode | RGB graphics to RGBA8 | Palette to RGB8 |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 1.111x | 1.079x | 1.071x | 0.995x |
| Windows x64 | 1.160x | 1.148x | 1.058x | 0.930x |
| Linux ARM64 | 1.076x | 1.067x | 1.004x | 1.003x |
| macOS ARM64 | 1.139x | 1.153x | 1.215x | 0.935x |
| Windows ARM64 | 1.153x | 1.125x | 1.127x | 1.001x |

Every RGB-photo encoder pair improves on all five platforms. The merge
also has losses: the three graphics encoder cases measure 0.881–0.902x
on Linux x64 and 0.871–0.907x on Windows x64. Windows x64 palette-to-RGB
decode measures 0.930x, with six of seven pairs slower. Linux x64 Gray16
native decode is 0.961x, and Windows ARM64 RGBA16 graphics native decode
is 0.970x. These targeted gains do not establish a gain on every image.

A fresh local VTune software profile of the merged source attributes
81.5% of RGB-photo encoder CPU samples to `deflate_match`, 6.6% to one
`deflate_insert` site and 2.5% to short matching. It uses the i7-1355U,
logical CPU 4, with a five-second timed loop plus setup and warm-up.
Hardware counter collection requires elevation on this machine; the
software profile is sufficient to identify functions, but its timing is
not a performance comparison. SIMD filter and checksum kernels leave
substantial scalar match-search and branch work for compiler tuning.

### Retained save and load changes

The original prefix-load reuse experiment improved graphics saving on
EPYC 7763 but stayed near parity on EPYC 9V74 and retained small photo
losses. The [full comparison](https://github.com/bojosos/ptpng/actions/runs/38467663609)
and [focused repeat](https://github.com/bojosos/ptpng/actions/runs/38468982176)
did not support keeping that matcher change. It has been removed.

The retained encoder caps long match searches at 64 candidates rather
than 128. Lookahead searches starting with an eight-byte match retain
the existing quarter-budget rule. Hashes, short matching, filter scoring
and DEFLATE block-cost selection remain unchanged. This changes some
compression decisions and encoded bytes while preserving filtered pixels.
The existing `DEFLATE_CHAIN` build definition can still select another cap.

MSVC x64 palette-to-RGB conversion replaces AVX2 gathering with eight
independent scalar table lookups, then retains the existing vector packing
and exact 24-byte stores. Scalar tails handle the last zero to seven pixels.
Other compilers and ARM retain their existing converters.

The [final comparison](https://github.com/bojosos/ptpng/actions/runs/38470200992)
tests `787b592` against merged `6b2bcfb`, five alternating pairs per
workload with at least 0.3 seconds per timed loop. The table reports median
candidate/baseline throughput ratios:

| Platform | RGB photo encode | Gray16 photo encode | Nine-image encode geometric mean | Palette to RGB8 decode |
| --- | ---: | ---: | ---: | ---: |
| Linux x64 | 1.501x | 1.239x | 1.079x | 1.000x |
| Windows x64 | 1.496x | 1.238x | 1.107x | 1.181x |
| Linux ARM64 | 1.488x | 1.228x | 1.064x | 1.003x |
| macOS ARM64 | 1.619x | 1.103x | 1.080x | 0.940x |
| Windows ARM64 | 1.495x | 1.230x | 1.082x | 1.000x |

Every RGB-photo encoder pair improves on every platform. All Gray16
pairs improve on Linux and Windows. macOS Gray16 ranges 0.908–1.338x;
its other controls also have broad OS scheduling variation. Windows
palette-to-RGB pairs range 1.147–1.187x. The two earlier checks measured
1.179x and 1.138x, with every pair faster. Local i7-1355U pairs measured
1.607x, so the gain depends on the CPU.

The change does not improve every workload. Linux ARM64 RGBA16 photo
encoding measures 0.976x, with all five pairs between 0.945–0.996x.
Its noise encoding measures 0.985x, with a 0.981–1.001x range. macOS noise
encode measures 0.929x with a 0.792–1.072x range, and its decoder controls
also swing. These results support the photo and Windows palette gains,
not a universal speedup.

RGB-photo PNG size grows from 5,357,962 to 5,459,744 bytes, or 1.900%.
Gray16 grows from 3,832,479 to 3,835,322 bytes, or 0.074%. The other seven
encoder fixtures stay the same size or shrink, including graphics shrinking
0.31–0.48%. All 225 candidate encoder-size observations agree across the
five platforms. Each fixture remains at most 2.045% larger than the
zlib-ng level-6 reference, within the previous 2.4% fixture bound. This
size bound describes these fixtures, not arbitrary images.

All 575 pairs retain matching input and decoded sizes. Local differential
validation verifies 2,108 raw streams with independent zlib; those raw
streams happen to retain identical compressed bytes. Eleven complete PNG
fixtures, including interlaced inputs, retain identical filtered pixels
when their IDAT streams are decompressed with independent zlib.
Nine local tests and all eight jobs in
[CI run 38470210983](https://github.com/bojosos/ptpng/actions/runs/38470210983)
passed. Palette tests cover lengths 0–65, all 32 alignments, exact input
allocations and output guards for RGB and RGBA conversion. Compression
quality checks additionally cover binary residuals with deep hash chains
and binary/noise mixtures against zlib level 6.

[Fuzz run 38470211035](https://github.com/bojosos/ptpng/actions/runs/38470211035)
completed 734,167 executions across decode, inflate, PNG encode and raw
DEFLATE on x64/ARM64 without a reported sanitizer finding. These are bounded
checks; the raw compressor corpus includes inputs up to 2 MiB.

### Compiler measurements

ClangCL previously failed to build the x86 checksum translation unit
because its PCLMUL target flag was missing. The build now enables that
instruction only for that runtime-dispatched translation unit. Ordinary
MSVC builds retain their compiler flags.

The [Windows compiler workflow](.github/workflows/compiler.yml) builds
MSVC, ClangCL, an instrumented ClangCL trainer and a ClangCL PGO version.
Training uses all nine benchmark images in native/RGB8/RGBA8 output and
the unit tests. Alternating comparisons measure Clang/MSVC, PGO/Clang and
PGO/MSVC on the same runner. Training and measurement share synthetic
fixtures, so any gains need validation on the consuming application's
images before choosing a compiler for release builds. This workflow
keeps compiler tuning separate from source changes and the nightly
dashboard; it does not enable PGO or change the default compiler.

Local Clang 19.1.5 and frontend-instrumented PGO builds passed six unit
tests. Independent differential checks against merged-source MSVC match
2,108 raw compressed streams and eleven complete PNGs byte-for-byte for
both compilers. Local paired timings had severe CPU scheduling variation,
including RGB-photo PGO ratios spanning 0.263–2.560x, so they do not
support a compiler recommendation. ICX is not installed on this machine
and has not been measured.

The [controlled compiler run](https://github.com/bojosos/ptpng/actions/runs/38467669393)
used MSVC 19.51.36260 and ClangCL 22.1.3 on a Xeon Platinum 8573C.
It tested `d10be1b`, the earlier 128-candidate prefix-reuse experiment,
before the final search-budget change. Each comparison has seven
alternating half-second pairs for the same 23 workloads. Encoder and
decoder means weight their nine and fourteen cases equally:

| Comparison | Encode geometric mean | Decode geometric mean |
| --- | ---: | ---: |
| Clang / MSVC | 0.986x | 0.988x |
| Clang PGO / Clang | 1.055x | 1.008x |
| Clang PGO / MSVC | 1.042x | 0.990x |

PGO improves Clang's Gray8 photo encoding 8.2%, RGBA8 8.0%, RGBA16 10.8%
and noise 16.3%. Every pair improves in these four cases. It also retains
losses: RGB graphics decode is 0.980x versus Clang, with every pair slower.
Clang PGO's RGBA16 graphics decode is 0.902x versus MSVC, while RGB-photo
encode is 0.978x with all seven pairs slower. Some comparisons have large
outliers, including unchanged graphics encoder controls. All output-size
pairs match and all four builds pass their six unit tests. These results
show useful PGO gains despite explicit SIMD, but do not justify replacing
MSVC as the default Windows compiler or predict performance with the final
64-candidate encoder. Profiles need regeneration when source or workloads
change.

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
