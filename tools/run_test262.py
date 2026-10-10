#!/usr/bin/env python3
"""
AetherVM Test262 Compliance Test Suite Runner
Executes official ECMA-262 Test262 conformance test suites against AetherVM.
"""

import sys
import os
import re
import time
import argparse
import subprocess
from pathlib import Path

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
    in_negative = False

    for line in yaml_block.splitlines():
        line_strip = line.strip()
        if not line_strip:
            continue

        if line_strip.startswith("negative:"):
            meta["is_negative"] = True
            in_negative = True
            continue

        if in_negative:
            if line_strip.startswith("phase:"):
                meta["negative_phase"] = line_strip[6:].strip()
                continue
            elif line_strip.startswith("type:"):
                meta["negative_type"] = line_strip[5:].strip()
                continue
            elif not line.startswith("  ") and not line.startswith("\t"):
                in_negative = False

        if line_strip.startswith("description:"):
            meta["description"] = line_strip[12:].strip()
        elif line_strip.startswith("features:"):
            val = line_strip[9:].strip().strip("[]")
            meta["features"] = [x.strip() for x in val.split(",") if x.strip()]
        elif line_strip.startswith("flags:"):
            val = line_strip[6:].strip().strip("[]")
            meta["flags"] = [x.strip() for x in val.split(",") if x.strip()]
        elif line_strip.startswith("includes:"):
            val = line_strip[9:].strip().strip("[]")
            meta["includes"] = [x.strip() for x in val.split(",") if x.strip()]

    return meta

def load_harness(harness_dir, filename):
    """Loads a harness file from disk."""
    p = harness_dir / filename
    if p.exists():
        return p.read_text(encoding="utf-8", errors="replace")
    return ""

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

        for inc in meta["includes"]:
            if inc not in ("assert.js", "sta.js"):
                inc_text = load_harness(harness_dir, inc)
                if inc_text:
                    parts.append(inc_text)

    parts.append(source)
    return "\n".join(parts)

def run_single_test(binary_path, test_path, harness_dir, temp_file_path):
    """Runs a single Test262 test script and evaluates conformance."""
    try:
        source = test_path.read_text(encoding="utf-8", errors="replace")
    except Exception as e:
        return False, f"Failed to read file: {e}", 0.0

    meta = parse_frontmatter(source)
    bundled = bundle_test(source, meta, harness_dir)

    temp_file_path.write_text(bundled, encoding="utf-8", errors="replace")

    start_time = time.perf_counter()
    try:
        proc = subprocess.run(
            [str(binary_path), str(temp_file_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10
        )
        elapsed_ms = (time.perf_counter() - start_time) * 1000.0
    except subprocess.TimeoutExpired:
        return False, "Execution timed out (> 10s)", 10000.0
    except Exception as e:
        return False, f"Subprocess launch failed: {e}", 0.0

    combined_output = (proc.stdout + "\n" + proc.stderr).strip()

    if meta["is_negative"]:
        if proc.returncode != 0 and (meta["negative_type"] in combined_output or "SyntaxError" in combined_output):
            return True, "Passed (Expected negative error caught)", elapsed_ms
        elif proc.returncode == 0:
            return False, f"Expected negative {meta['negative_type']} but script completed with 0", elapsed_ms
        else:
            return False, f"Wrong negative error: {combined_output}", elapsed_ms
    else:
        if proc.returncode == 0:
            return True, "Passed", elapsed_ms
        else:
            return False, f"Uncaught exception (exit {proc.returncode}): {combined_output}", elapsed_ms

def main():
    parser = argparse.ArgumentParser(description="AetherVM Test262 Conformance Test Runner")
    parser.add_argument("target", nargs="?", default="test/test262/test", help="Path to test file or directory")
    parser.add_argument("--binary", default="build/Release/aether.exe", help="Path to AetherVM binary")
    parser.add_argument("--harness", default="test/test262/harness", help="Path to Test262 harness directory")
    parser.add_argument("--filter", default="", help="Filter tests matching substring")
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
        sys.exit(0)

    print("=" * 72)
    print(" AetherVM Test262 ECMAScript Conformance Test Runner")
    print("=" * 72)
    print(f" Target Directory : {target_path}")
    print(f" Binary Executable: {binary_path}")
    print(f" Total Tests Found: {len(test_files)}")
    print("-" * 72)

    temp_file = Path(".tmp_test262_eval.js").resolve()

    passed = 0
    failed = 0
    total_time = 0.0

    try:
        for idx, test_file in enumerate(test_files, 1):
            rel_name = test_file.name
            try:
                rel_name = test_file.relative_to(target_path)
            except ValueError:
                pass

            is_ok, msg, dur = run_single_test(binary_path, test_file, harness_dir, temp_file)
            total_time += dur

            status_tag = "[PASS]" if is_ok else "[FAIL]"
            print(f" {status_tag} ({idx:2d}/{len(test_files)}) {str(rel_name):<45} ({dur:5.1f} ms)")
            if not is_ok:
                print(f"         Reason: {msg}")

            if is_ok:
                passed += 1
            else:
                failed += 1

    finally:
        if temp_file.exists():
            try:
                temp_file.unlink()
            except Exception:
                pass

    pass_rate = (passed / len(test_files)) * 100.0 if test_files else 0.0

    print("=" * 72)
    print(f" TEST262 RUN SUMMARY:")
    print(f" Total Test Cases : {len(test_files)}")
    print(f" Passed           : {passed}")
    print(f" Failed           : {failed}")
    print(f" Conformance Rate : {pass_rate:.1f}%")
    print(f" Total Duration   : {total_time:.2f} ms")
    print("=" * 72)

    sys.exit(0 if failed == 0 else 1)

if __name__ == "__main__":
    main()
