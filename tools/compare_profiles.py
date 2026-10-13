"""Alternate baseline/candidate process timings on the same machine.

Each driver validates pixels before timing. These repeated same-run comparisons
avoid comparing raw throughput between different hosted VMs.
"""
import argparse
import json
import os
import platform
from pathlib import Path
import statistics
import subprocess


IMAGES = ("photo_rgb8", "photo_rgba8", "photo_gray8", "photo_gray16",
          "graphic_pal8", "graphic_rgb8", "photo_rgba16_paeth",
          "graphic_rgba16_paeth", "noise_rgba8")


def set_affinity():
    if hasattr(os, "sched_getaffinity"):
        cpu = min(os.sched_getaffinity(0))
        os.sched_setaffinity(0, {cpu})
        return f"pinned logical CPU {cpu}"
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = wintypes.HANDLE
        kernel.GetProcessAffinityMask.argtypes = [wintypes.HANDLE,
            ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        kernel.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
        process = kernel.GetCurrentProcess()
        allowed, system = ctypes.c_size_t(), ctypes.c_size_t()
        if not kernel.GetProcessAffinityMask(process, ctypes.byref(allowed), ctypes.byref(system)):
            raise ctypes.WinError(ctypes.get_last_error())
        mask = allowed.value & -allowed.value
        if not mask or not kernel.SetProcessAffinityMask(process, mask):
            raise ctypes.WinError(ctypes.get_last_error())
        return f"pinned logical CPU {mask.bit_length() - 1}"
    # macOS has affinity hints, not the strict logical-CPU pinning used above.
    return "OS scheduled; strict CPU affinity unavailable"


def measure(driver, operation, name, seconds, fmt):
    command = [str(driver), operation, f"tests/bench/{name}.png", str(seconds), fmt]
    output = subprocess.check_output(command, text=True, timeout=90)
    fields = dict(token.split("=", 1) for token in output.split() if "=" in token)
    return float(fields["MPix_per_second"]), output.strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--pairs", type=int, default=9)
    parser.add_argument("--seconds", type=float, default=1)
    parser.add_argument("--images", nargs="+", choices=IMAGES, default=IMAGES)
    args = parser.parse_args()
    if not 3 <= args.pairs <= 21 or not 0.1 <= args.seconds <= 5:
        parser.error("pairs must be 3..21 and seconds 0.1..5")
    drivers = [args.baseline.resolve(strict=True), args.candidate.resolve(strict=True)]
    affinity = set_affinity()
    args.output.mkdir(parents=True, exist_ok=True)
    results = []
    lines = [f"## Paired comparison, {affinity}", "",
             f"{args.pairs} alternating pairs per workload; {args.seconds:g} seconds per timed loop. "
             "Speedup is candidate/baseline throughput. The ranges are observed "
             "paired minima/maxima, not confidence intervals.", "",
             "| Workload | Median speedup | Pair range |",
             "|---|---:|---:|"]
    workloads = [(operation, name,
                  "rgba8" if operation == "encode" and name == "graphic_pal8" else "native")
                 for operation in ("decode", "encode") for name in args.images]
    if "graphic_pal8" in args.images:
        workloads.append(("decode", "graphic_pal8", "rgb8"))
    for name in ("photo_rgb8", "graphic_rgb8"):
        if name in args.images:
            workloads.append(("decode", name, "rgba8"))
    for name in ("photo_rgba8", "noise_rgba8"):
        if name in args.images:
            workloads.append(("decode", name, "rgb8"))
    for operation, name, fmt in workloads:
        pairs = []
        for iteration in range(args.pairs):
            values = [None, None]
            for index in (iteration % 2, 1 - iteration % 2):
                values[index] = measure(drivers[index], operation, name, args.seconds, fmt)
            pairs.append({"baseline": values[0][0], "candidate": values[1][0],
                          "ratio": values[1][0] / values[0][0],
                          "logs": [value[1] for value in values]})
        ratios = [pair["ratio"] for pair in pairs]
        workload = f"{operation}-{name}"
        if operation == "decode" and fmt != "native":
            workload += f"-{fmt}"
        results.append({"workload": workload, "affinity": affinity,
                        "platform": platform.platform(), "pairs": pairs})
        lines.append(f"| {workload} | {statistics.median(ratios):.3f}x | "
                     f"{min(ratios):.3f}–{max(ratios):.3f}x |")
        # Save completed cases even if a later workload fails.
        (args.output / "paired.json").write_text(json.dumps(results, indent=2))
    summary = "\n".join(lines) + "\n"
    (args.output / "paired.md").write_text(summary, encoding="utf-8")
    print(summary)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as file:
            file.write(summary)


if __name__ == "__main__":
    main()
