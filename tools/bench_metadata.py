"""Record the runner visible to the benchmark, without assuming fixed hardware.

usage: bench_metadata.py output.json [cmake-build-dir]

With a build directory, the C compiler that CMake detected is recorded too:
runner image updates can change compiler versions, and with them both
sides of a benchmark comparison.
"""
import glob
import json
import os
import platform
import subprocess
import sys


def capture(command):
    try:
        return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT,
                                       timeout=15).strip()
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        return str(error)


system = platform.system()
if system == "Windows":
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,
                            r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            cpu = winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    except OSError:
        cpu = os.environ.get("PROCESSOR_IDENTIFIER", "unknown")
elif system == "Darwin":
    cpu = capture(["sysctl", "machdep.cpu.brand_string", "hw.ncpu", "hw.memsize"])
else:
    cpu = capture(["lscpu"])

def cmake_compiler(build_dir):
    found = {}
    for path in glob.glob(os.path.join(build_dir, "CMakeFiles", "*", "CMakeCCompiler.cmake")):
        with open(path, encoding="utf-8", errors="replace") as file:
            for line in file:
                for key in ("CMAKE_C_COMPILER_ID", "CMAKE_C_COMPILER_VERSION"):
                    if line.startswith("set(" + key + " "):
                        found[key] = line.split('"')[1] if '"' in line else ""
        if found:
            break
    return " ".join(found.get(key, "") for key in
                    ("CMAKE_C_COMPILER_ID", "CMAKE_C_COMPILER_VERSION")).strip() or "unknown"


metadata = {"os": platform.platform(), "machine": platform.machine(),
            "logical_cpus": os.cpu_count(), "cpu": cpu,
            "runner_image": os.environ.get("ImageOS", "unknown"),
            "runner_image_version": os.environ.get("ImageVersion", "unknown"),
            "run_id": os.environ.get("GITHUB_RUN_ID", "local"),
            "affinity": "OS scheduled; no fixed CPU affinity"}
if len(sys.argv) > 2:
    metadata["c_compiler"] = cmake_compiler(sys.argv[2])
with open(sys.argv[1], "w", encoding="utf-8") as output:
    json.dump(metadata, output, indent=2)
print(json.dumps(metadata, indent=2))
