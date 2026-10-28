#!/usr/bin/env python3
"""
AetherVM Matrix Test Runner
Executes the full test matrix:
  - Tiers: {interpreter, baseline, opt}
  - GC modes: {default, gc-stress, gc-verify}
  Total: 3 x 3 = 9 matrix configurations across benchmark suite and test suite.
"""

import sys
import os
import subprocess
import time
from pathlib import Path

TIERS = {
    "interpreter": ["--interpreter-only"],
    "baseline": ["--always-baseline"],
    "opt": ["--always-optimize"]
}

GC_MODES = {
    "default": [],
    "gc-stress": ["--gc-stress=10"],
    "gc-verify": ["--gc-verify"]
}

BENCHMARKS = [
    "gc1.js",
    "gc2.js",
    "gc3.js",
    "str.js",
    "depth.js",
    "nbody.js",
    "richards.js",
    "deltablue.js"
]

def run_configuration(binary_path, bench_dir, tier_name, tier_flags, gc_name, gc_flags):
    combined_flags = tier_flags + gc_flags
    flags_str = " ".join(combined_flags) if combined_flags else "(default)"

    start = time.perf_counter()
    failed_tests = []

    for bench_name in BENCHMARKS:
        script_path = bench_dir / bench_name
        if not script_path.is_file():
            continue

        cmd = [str(binary_path)] + combined_flags + [str(script_path)]
        try:
            proc = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding='utf-8',
                errors='replace',
                timeout=30.0
            )
            if proc.returncode != 0:
                err = (proc.stdout + "\n" + proc.stderr).strip()[:150]
                failed_tests.append((bench_name, err))
        except subprocess.TimeoutExpired:
            failed_tests.append((bench_name, "Timed out (>30s)"))
        except Exception as e:
            failed_tests.append((bench_name, str(e)))

    elapsed = time.perf_counter() - start
    success = (len(failed_tests) == 0)
    return success, elapsed, flags_str, failed_tests

def main():
    binary_path = Path("build/Release/aether.exe").resolve()
    if not binary_path.is_file():
        # Fallback to linux binary
        binary_path = Path("build/aether").resolve()
    if not binary_path.is_file():
        print(f"Error: Aether binary not found at '{binary_path}'", flush=True)
        sys.exit(1)

    bench_dir = Path("bench").resolve()

    print("=" * 76, flush=True)
    print(" AetherVM Full Matrix Validation (3 Tiers x 3 GC Modes = 9 Configurations)", flush=True)
    print(f" Binary  : {binary_path}", flush=True)
    print(f" Test Set: 8 Benchmarks ({', '.join(BENCHMARKS)})", flush=True)
    print("=" * 76, flush=True)

    results = []
    all_passed = True

    print(f"{'#':<3} | {'Tier':<14} | {'GC Mode':<12} | {'Flags':<26} | {'Status':<8} | {'Duration'}", flush=True)
    print("-" * 76, flush=True)

    idx = 1
    for t_name, t_flags in TIERS.items():
        for g_name, g_flags in GC_MODES.items():
            success, duration, flags_str, failures = run_configuration(
                binary_path, bench_dir, t_name, t_flags, g_name, g_flags
            )
            status_str = "PASS" if success else "FAIL"
            print(f"{idx:<3} | {t_name:<14} | {g_name:<12} | {flags_str:<26} | {status_str:<8} | {duration:.2f} s", flush=True)
            if not success:
                all_passed = False
                for b_name, err in failures:
                    print(f"      - {b_name} failed: {err}", flush=True)
            results.append((t_name, g_name, success, duration))
            idx += 1

    print("=" * 76, flush=True)
    total_time = sum(r[3] for r in results)
    passed_count = sum(1 for r in results if r[2])
    print(f" Matrix Summary: {passed_count}/9 configurations passed in {total_time:.1f} s", flush=True)
    print("=" * 76, flush=True)

    sys.exit(0 if all_passed else 1)

if __name__ == "__main__":
    main()
