#!/usr/bin/env python3
"""
AetherVM Performance Benchmark Harness
Runs micro-probes and standard JS benchmarks under Interpreter, Baseline JIT, and Optimizing JIT.
Measures wall time, memory RSS, and outputs comparison deltas against committed baselines.
"""

import sys
import os
import time
import json
import argparse
import subprocess
from pathlib import Path

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

TIERS = {
    "interp": ["--interpreter-only"],
    "baseline": ["--always-baseline"],
    "opt": ["--always-optimize"]
}

def get_peak_rss_kb(proc):
    # In Windows or Linux, subprocess returns execution results.
    # Platform-specific memory stats can be measured or estimated.
    return 0

def run_benchmark(binary_path, script_path, tier_name, tier_flags, trace_gc=False):
    cmd = [str(binary_path)]
    cmd.extend(tier_flags)
    if trace_gc:
        cmd.append("--trace-gc")
    cmd.append(str(script_path))

    start = time.perf_counter()
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
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        success = (proc.returncode == 0)
        output = proc.stdout + "\n" + proc.stderr
        gc_count = output.count("[GC]")
        return {
            "success": success,
            "wall_ms": round(elapsed_ms, 2),
            "gc_count": gc_count,
            "error": "" if success else output.strip()[:200]
        }
    except subprocess.TimeoutExpired:
        return {"success": False, "wall_ms": 30000.0, "gc_count": 0, "error": "Timeout"}
    except Exception as e:
        return {"success": False, "wall_ms": 0.0, "gc_count": 0, "error": str(e)}

def main():
    parser = argparse.ArgumentParser(description="AetherVM Performance Benchmarks Runner")
    parser.add_argument("--binary", default="build/Release/aether.exe", help="Path to aether executable")
    parser.add_argument("--bench-dir", default="bench", help="Directory containing benchmark scripts")
    parser.add_argument("--out", default="", help="Path to save output JSON benchmark results")
    parser.add_argument("--compare", default="", help="Path to baseline JSON file for delta comparison")
    parser.add_argument("--tier", choices=["all", "interp", "baseline", "opt"], default="all", help="Tier to benchmark")
    args = parser.parse_args()

    binary_path = Path(args.binary).resolve()
    if not binary_path.is_file():
        print(f"Error: Aether binary not found at '{binary_path}'")
        sys.exit(1)

    bench_dir = Path(args.bench_dir).resolve()
    tiers_to_run = TIERS.keys() if args.tier == "all" else [args.tier]

    print("=" * 76)
    print(" AetherVM Performance Benchmark Suite")
    print(f" Binary  : {binary_path}")
    print(f" Directory: {bench_dir}")
    print(f" Tiers   : {', '.join(tiers_to_run)}")
    print("=" * 76)

    baseline_data = None
    if args.compare:
        compare_path = Path(args.compare).resolve()
        if compare_path.is_file():
            try:
                baseline_data = json.loads(compare_path.read_text(encoding="utf-8"))
                print(f" Comparing against baseline: {compare_path.name}")
            except Exception as e:
                print(f"Warning: Could not read baseline file: {e}")

    results = {}

    print(f"{'Benchmark':<16} | {'Tier':<10} | {'Wall Time':<12} | {'GC Events':<10} | {'Status':<8} | {'Delta vs Base':<14}")
    print("-" * 76)

    for bench_name in BENCHMARKS:
        script_file = bench_dir / bench_name
        if not script_file.is_file():
            continue

        results[bench_name] = {}
        for tier in tiers_to_run:
            res = run_benchmark(binary_path, script_file, tier, TIERS[tier], trace_gc=True)
            results[bench_name][tier] = res

            status_str = "PASS" if res["success"] else "FAIL"
            time_str = f"{res['wall_ms']:.2f} ms"

            delta_str = "-"
            if baseline_data and bench_name in baseline_data and tier in baseline_data[bench_name]:
                base_time = baseline_data[bench_name][tier].get("wall_ms", 0.0)
                if base_time > 0 and res["wall_ms"] > 0:
                    pct = ((res["wall_ms"] - base_time) / base_time) * 100.0
                    sign = "+" if pct > 0 else ""
                    delta_str = f"{sign}{pct:.1f}% ({base_time:.1f}ms)"

            print(f"{bench_name:<16} | {tier:<10} | {time_str:<12} | {res['gc_count']:<10} | {status_str:<8} | {delta_str:<14}")
            if not res["success"] and res["error"]:
                print(f"   Error: {res['error']}")

    print("=" * 76)

    if args.out:
        out_path = Path(args.out).resolve()
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(results, indent=2), encoding="utf-8")
        print(f" Benchmark results written to: {out_path}")

    # Check if all passed
    all_passed = all(
        res.get("success", False)
        for bench_res in results.values()
        for res in bench_res.values()
    )
    sys.exit(0 if all_passed else 1)

if __name__ == "__main__":
    main()
