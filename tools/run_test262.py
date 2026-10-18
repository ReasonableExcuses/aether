#!/usr/bin/env python3
"""
AetherVM Test262 Compliance Test Suite Runner
Executes official ECMA-262 Test262 conformance test suites against AetherVM.
Supports multi-worker parallel execution, harness caching, feature filtering, and detailed reporting.
"""

import sys
import os
import re
import time
import json
import argparse
import subprocess
import threading
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

# Global in-memory harness cache
_HARNESS_CACHE = {}
_HARNESS_CACHE_LOCK = threading.Lock()

def parse_frontmatter(content):
    """Parses YAML frontmatter block between /*--- and ---*/."""
    meta = {
        "description": "",
        "features": [],
        "flags": [],
        "includes": [],
        "is_negative": False,
        "negative_phase": "",
        "negative_type": ""
    }
    match = re.search(r'/\*---(.*?)---\*/', content, re.DOTALL)
    if not match:
        return meta

    yaml_block = match.group(1)
    current_key = None

    for raw_line in yaml_block.splitlines():
        line = raw_line.strip()
        if not line:
            continue

        if line.startswith("negative:"):
            meta["is_negative"] = True
            rest = line[9:].strip()
            if rest.startswith("{") and rest.endswith("}"):
                inner = rest[1:-1]
                p_match = re.search(r'phase:\s*([a-zA-Z0-9_-]+)', inner)
                t_match = re.search(r'type:\s*([a-zA-Z0-9_-]+)', inner)
                if p_match: meta["negative_phase"] = p_match.group(1).strip()
                if t_match: meta["negative_type"] = t_match.group(1).strip()
                current_key = None
            else:
                current_key = "negative"
            continue

        if current_key == "negative":
            if line.startswith("phase:"):
                meta["negative_phase"] = line[6:].strip()
                continue
            elif line.startswith("type:"):
                meta["negative_type"] = line[5:].strip()
                continue
            elif not raw_line.startswith(" ") and not raw_line.startswith("\t"):
                current_key = None

        if line.startswith("includes:"):
            val = line[9:].strip()
            if val.startswith("[") and val.endswith("]"):
                meta["includes"].extend([x.strip() for x in val[1:-1].split(",") if x.strip()])
                current_key = None
            else:
                current_key = "includes"
            continue
        elif current_key == "includes":
            if line.startswith("- "):
                meta["includes"].append(line[2:].strip())
                continue
            elif not raw_line.startswith(" ") and not raw_line.startswith("\t"):
                current_key = None

        if line.startswith("flags:"):
            val = line[6:].strip()
            if val.startswith("[") and val.endswith("]"):
                meta["flags"].extend([x.strip() for x in val[1:-1].split(",") if x.strip()])
                current_key = None
            else:
                current_key = "flags"
            continue
        elif current_key == "flags":
            if line.startswith("- "):
                meta["flags"].append(line[2:].strip())
                continue
            elif not raw_line.startswith(" ") and not raw_line.startswith("\t"):
                current_key = None

        if line.startswith("features:"):
            val = line[9:].strip()
            if val.startswith("[") and val.endswith("]"):
                meta["features"].extend([x.strip() for x in val[1:-1].split(",") if x.strip()])
                current_key = None
            else:
                current_key = "features"
            continue
        elif current_key == "features":
            if line.startswith("- "):
                meta["features"].append(line[2:].strip())
                continue
            elif not raw_line.startswith(" ") and not raw_line.startswith("\t"):
                current_key = None

        if line.startswith("description:"):
            meta["description"] = line[12:].strip()
            current_key = None

    return meta

def load_harness(harness_dir, filename):
    """Loads a harness file with thread-safe caching."""
    cache_key = (str(harness_dir), filename)
    with _HARNESS_CACHE_LOCK:
        if cache_key in _HARNESS_CACHE:
            return _HARNESS_CACHE[cache_key]

    p = harness_dir / filename
    content = ""
    if p.exists():
        try:
            content = p.read_text(encoding="utf-8", errors="replace")
        except Exception:
            content = ""

    with _HARNESS_CACHE_LOCK:
        _HARNESS_CACHE[cache_key] = content
    return content

def bundle_test(source, meta, harness_dir):
    """Bundles harness scripts and prepends strict mode directives."""
    parts = []

    if "onlyStrict" in meta["flags"]:
        parts.append('"use strict";\n')

    if "raw" not in meta["flags"]:
        assert_js = load_harness(harness_dir, "assert.js")
        if assert_js:
            parts.append(assert_js)

        sta_js = load_harness(harness_dir, "sta.js")
        if sta_js:
            parts.append(sta_js)

        if "async" in meta["flags"]:
            parts.append('if (typeof print === "undefined") { if (typeof console !== "undefined" && console.log) { globalThis.print = function(msg) { console.log(msg); }; } }\n')
            done_js = load_harness(harness_dir, "doneprintHandle.js")
            if done_js:
                parts.append(done_js)
                parts.append('\nglobalThis.$DONE = $DONE;\n')

        for inc in meta["includes"]:
            if inc not in ("assert.js", "sta.js", "doneprintHandle.js"):
                inc_text = load_harness(harness_dir, inc)
                if inc_text:
                    parts.append(inc_text)

    parts.append(source)
    return "\n".join(parts)

def run_single_test_file(binary_path, test_path, target_root, harness_dir, timeout_s, worker_id, unsupported_features=None):
    """Runs a single test file in an isolated temporary file for the worker."""
    try:
        source = test_path.read_text(encoding="utf-8", errors="replace")
    except Exception as e:
        return {
            "path": str(test_path),
            "passed": False,
            "skipped": False,
            "reason": f"Failed to read file: {e}",
            "duration_ms": 0.0,
            "negative": False,
            "timed_out": False
        }

    meta = parse_frontmatter(source)

    if unsupported_features:
        for f in meta["features"]:
            if f in unsupported_features:
                return {
                    "path": str(test_path),
                    "passed": False,
                    "skipped": True,
                    "reason": f"Skipped (unsupported feature: '{f}')",
                    "duration_ms": 0.0,
                    "negative": meta["is_negative"],
                    "timed_out": False
                }

    if "raw" not in meta["flags"]:
        required_harness = ["assert.js", "sta.js"]
        if "async" in meta["flags"]:
            required_harness.append("doneprintHandle.js")
        required_harness.extend(
            inc for inc in meta["includes"] if inc not in required_harness
        )
        missing = [name for name in required_harness if not (harness_dir / name).is_file()]
        if missing:
            return {
                "path": str(test_path),
                "passed": False,
                "skipped": False,
                "reason": f"Missing required harness file(s): {', '.join(missing)}",
                "duration_ms": 0.0,
                "negative": meta["is_negative"],
                "timed_out": False
            }

    bundled = bundle_test(source, meta, harness_dir)

    temp_file = Path(f".tmp_test262_w{worker_id}_{threading.get_ident()}.js").resolve()
    try:
        temp_file.write_text(bundled, encoding="utf-8", errors="replace")

        start_time = time.perf_counter()
        try:
            proc = subprocess.run(
                [str(binary_path), str(temp_file)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                timeout=timeout_s
            )
            elapsed_ms = (time.perf_counter() - start_time) * 1000.0
            timed_out = False
        except subprocess.TimeoutExpired:
            return {
                "path": str(test_path),
                "passed": False,
                "skipped": False,
                "reason": f"Execution timed out (> {timeout_s}s)",
                "duration_ms": timeout_s * 1000.0,
                "negative": meta["is_negative"],
                "timed_out": True
            }
        except Exception as e:
            return {
                "path": str(test_path),
                "passed": False,
                "skipped": False,
                "reason": f"Subprocess launch failed: {e}",
                "duration_ms": 0.0,
                "negative": meta["is_negative"],
                "timed_out": False
            }

        combined_output = (proc.stdout + "\n" + proc.stderr).strip()

        if meta["is_negative"]:
            neg_type = meta["negative_type"]
            matched_err = False
            if neg_type in combined_output:
                matched_err = True

            if proc.returncode != 0 and matched_err:
                return {
                    "path": str(test_path),
                    "passed": True,
                    "skipped": False,
                    "reason": f"Passed (Expected negative error '{neg_type}' caught)",
                    "duration_ms": elapsed_ms,
                    "negative": True,
                    "timed_out": False
                }
            elif proc.returncode == 0:
                return {
                    "path": str(test_path),
                    "passed": False,
                    "skipped": False,
                    "reason": f"Expected negative {neg_type} but script completed with 0",
                    "duration_ms": elapsed_ms,
                    "negative": True,
                    "timed_out": False
                }
            else:
                return {
                    "path": str(test_path),
                    "passed": False,
                    "skipped": False,
                    "reason": f"Wrong negative error (expected {neg_type}): {combined_output[:140]}",
                    "duration_ms": elapsed_ms,
                    "negative": True,
                    "timed_out": False
                }
        else:
            if "async" in meta["flags"]:
                if "Test262:AsyncTestFailure" in combined_output:
                    return {
                        "path": str(test_path),
                        "passed": False,
                        "skipped": False,
                        "reason": f"Async failure: {combined_output[:160]}",
                        "duration_ms": elapsed_ms,
                        "negative": False,
                        "timed_out": False
                    }
                elif proc.returncode == 0 and "Test262:AsyncTestComplete" in combined_output:
                    return {
                        "path": str(test_path),
                        "passed": True,
                        "skipped": False,
                        "reason": "Passed (AsyncTestComplete)",
                        "duration_ms": elapsed_ms,
                        "negative": False,
                        "timed_out": False
                    }
                elif proc.returncode != 0:
                    return {
                        "path": str(test_path),
                        "passed": False,
                        "skipped": False,
                        "reason": f"Uncaught exception (exit {proc.returncode}): {combined_output[:160]}",
                        "duration_ms": elapsed_ms,
                        "negative": False,
                        "timed_out": False
                    }
                else:
                    return {
                        "path": str(test_path),
                        "passed": False,
                        "skipped": False,
                        "reason": "Async test completed with 0 but without $DONE() call",
                        "duration_ms": elapsed_ms,
                        "negative": False,
                        "timed_out": False
                    }
            elif proc.returncode == 0:
                return {
                    "path": str(test_path),
                    "passed": True,
                    "skipped": False,
                    "reason": "Passed",
                    "duration_ms": elapsed_ms,
                    "negative": False,
                    "timed_out": False
                }
            else:
                return {
                    "path": str(test_path),
                    "passed": False,
                    "skipped": False,
                    "reason": f"Uncaught exception (exit {proc.returncode}): {combined_output[:160]}",
                    "duration_ms": elapsed_ms,
                    "negative": False,
                    "timed_out": False
                }
    finally:
        if temp_file.exists():
            try:
                temp_file.unlink()
            except Exception:
                pass

def main():
    parser = argparse.ArgumentParser(description="AetherVM Official Test262 Conformance Test Runner")
    parser.add_argument("target", nargs="?", default="test/official-test262/test", help="Path to official Test262 file or directory")
    parser.add_argument("--binary", default="build/Release/aether.exe", help="Path to AetherVM binary")
    parser.add_argument("--harness", default="test/official-test262/harness", help="Path to official Test262 harness directory")
    parser.add_argument("--filter", default="", help="Filter tests matching substring")
    parser.add_argument("--workers", "-j", type=int, default=1, help="Number of concurrent worker threads")
    parser.add_argument("--timeout", type=float, default=5.0, help="Per-test timeout in seconds")
    parser.add_argument("--report", default="", help="Optional JSON report output file path")
    parser.add_argument("--verbose", "-v", action="store_true", help="Print detailed failure info")
    parser.add_argument("--unsupported-features", default="tail-call-optimization", help="Comma-separated list of unsupported TC39 features to skip")
    parser.add_argument("--allow-skips", action="store_true", help="Allow unsupported-feature skips without failing the suite")
    args = parser.parse_args()

    binary_path = Path(args.binary).resolve()
    if not binary_path.exists():
        print(f"Error: AetherVM binary not found at '{binary_path}'")
        sys.exit(1)

    harness_dir = Path(args.harness).resolve()
    target_path = Path(args.target).resolve()

    if target_path.is_file():
        test_files = [target_path]
    elif target_path.is_dir():
        test_files = sorted(target_path.rglob("*.js"))
    else:
        print(f"Error: Target path '{target_path}' does not exist.")
        sys.exit(1)

    if args.filter:
        test_files = [f for f in test_files if args.filter.lower() in str(f).lower()]

    if not test_files:
        print("No test files found matching criteria.")
        sys.exit(1)

    unsupported_features = set(x.strip() for x in args.unsupported_features.split(",") if x.strip())

    print("=" * 76)
    print(" AetherVM Test262 ECMAScript Conformance Test Runner")
    print("=" * 76)
    print(f" Target Directory : {target_path}")
    print(f" Binary Executable: {binary_path}")
    print(f" Harness Dir      : {harness_dir}")
    print(f" Workers          : {args.workers}")
    if unsupported_features:
        print(f" Skip Features    : {', '.join(sorted(unsupported_features))}")
        print(f" Allow Skips      : {args.allow_skips}")
    print(f" Total Tests Found: {len(test_files)}")
    print("-" * 76)

    # Pre-cache common harness files
    load_harness(harness_dir, "assert.js")
    load_harness(harness_dir, "sta.js")

    results = []
    passed = 0
    failed = 0
    skipped = 0
    timed_out = 0
    total_time = 0.0

    start_suite_time = time.perf_counter()

    if args.workers <= 1:
        for idx, test_file in enumerate(test_files, 1):
            rel_name = test_file.name
            try:
                rel_name = test_file.relative_to(target_path)
            except ValueError:
                pass

            res = run_single_test_file(binary_path, test_file, target_path, harness_dir, args.timeout, 0, unsupported_features)
            res["rel_path"] = str(rel_name)
            results.append(res)
            total_time += res["duration_ms"]

            if res.get("skipped"):
                skipped += 1
                status_tag = "[SKIP]"
            elif res["passed"]:
                passed += 1
                status_tag = "[PASS]"
            else:
                failed += 1
                if res["timed_out"]:
                    timed_out += 1
                status_tag = "[FAIL]"

            print(f" {status_tag} ({idx:3d}/{len(test_files)}) {str(rel_name):<50} ({res['duration_ms']:5.1f} ms)")
            if not res["passed"] and not res.get("skipped"):
                print(f"         Reason: {res['reason']}")
            elif res.get("skipped") and args.verbose:
                print(f"         Reason: {res['reason']}")
    else:
        print(f" Executing {len(test_files)} tests using {args.workers} worker threads...")
        with ThreadPoolExecutor(max_workers=args.workers) as executor:
            future_to_file = {}
            for i, tf in enumerate(test_files):
                worker_id = i % args.workers
                future = executor.submit(run_single_test_file, binary_path, tf, target_path, harness_dir, args.timeout, worker_id, unsupported_features)
                future_to_file[future] = tf

            completed_count = 0
            for future in as_completed(future_to_file):
                tf = future_to_file[future]
                completed_count += 1
                rel_name = tf.name
                try:
                    rel_name = tf.relative_to(target_path)
                except ValueError:
                    pass

                try:
                    res = future.result()
                except Exception as exc:
                    res = {
                        "path": str(tf),
                        "passed": False,
                        "skipped": False,
                        "reason": f"Worker crashed: {exc}",
                        "duration_ms": 0.0,
                        "negative": False,
                        "timed_out": False
                    }

                res["rel_path"] = str(rel_name)
                results.append(res)
                total_time += res["duration_ms"]

                if res.get("skipped"):
                    skipped += 1
                    status_tag = "[SKIP]"
                elif res["passed"]:
                    passed += 1
                    status_tag = "[PASS]"
                else:
                    failed += 1
                    if res["timed_out"]:
                        timed_out += 1
                    status_tag = "[FAIL]"

                if not res["passed"] or args.verbose:
                    print(f" {status_tag} ({completed_count:3d}/{len(test_files)}) {str(rel_name):<50} ({res['duration_ms']:5.1f} ms)")
                    if not res["passed"]:
                        print(f"         Reason: {res['reason']}")
                elif completed_count % 50 == 0 or completed_count == len(test_files):
                    rate = (passed / completed_count) * 100.0 if completed_count > 0 else 0.0
                    print(f" Progress: {completed_count}/{len(test_files)} ({rate:.1f}% pass, {passed} passed, {failed} failed, {skipped} skipped)")

    suite_wall_time_s = time.perf_counter() - start_suite_time
    pass_rate = (passed / len(test_files)) * 100.0 if test_files else 0.0

    print("=" * 76)
    print(" TEST262 RUN SUMMARY:")
    print(f" Total Test Cases : {len(test_files)}")
    print(f" Passed           : {passed}")
    print(f" Failed           : {failed}")
    if skipped > 0:
        print(f" Skipped          : {skipped}")
    if timed_out > 0:
        print(f" Timed Out (> {args.timeout}s): {timed_out}")
    print(f" Conformance Rate : {pass_rate:.1f}%")
    print(f" Wall Clock Time  : {suite_wall_time_s:.2f} s")
    print(f" Sum Test Time    : {total_time / 1000.0:.2f} s")
    print("=" * 76)

    if args.report:
        report_data = {
            "total": len(test_files),
            "passed": passed,
            "failed": failed,
            "skipped": skipped,
            "timed_out": timed_out,
            "conformance_rate": pass_rate,
            "wall_clock_seconds": suite_wall_time_s,
            "results": results
        }
        report_file = Path(args.report).resolve()
        report_file.write_text(json.dumps(report_data, indent=2), encoding="utf-8")
        print(f" JSON report saved to: {report_file}")

    # Skips are not conformance. They are only permitted when the caller has
    # explicitly opted into a partial/unsupported-feature run.
    sys.exit(0 if failed == 0 and (args.allow_skips or skipped == 0) else 1)

if __name__ == "__main__":
    main()
