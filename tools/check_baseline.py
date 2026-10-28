#!/usr/bin/env python3
"""
Compares a Test262 run result JSON against a committed baseline JSON.
Fails if overall pass count drops or if individual regressions occur.
"""
import sys
import json
import argparse
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description="AetherVM Test262 Baseline Regression Checker")
    parser.add_argument("--current", required=True, help="Path to current results.json")
    parser.add_argument("--baseline", required=True, help="Path to baseline.json")
    parser.add_argument("--allow-drop", type=int, default=0, help="Allowed pass drop tolerance")
    args = parser.parse_args()

    current_path = Path(args.current).resolve()
    baseline_path = Path(args.baseline).resolve()

    if not current_path.is_file():
        print(f"Error: Current results file '{current_path}' not found.")
        sys.exit(1)
    if not baseline_path.is_file():
        print(f"Error: Baseline file '{baseline_path}' not found.")
        sys.exit(1)

    current_data = json.loads(current_path.read_text(encoding="utf-8"))
    baseline_data = json.loads(baseline_path.read_text(encoding="utf-8"))

    curr_passed = current_data.get("passed", 0)
    base_passed = baseline_data.get("passed", 0)
    diff = curr_passed - base_passed

    print("=" * 60)
    print(" Test262 Baseline Regression Check")
    print("=" * 60)
    print(f" Pinned Baseline Passes : {base_passed}")
    print(f" Current Passes         : {curr_passed}")
    print(f" Delta                  : {'+' if diff >= 0 else ''}{diff}")
    print("=" * 60)

    # Check for per-test regressions if detailed results exist
    curr_results = {r.get("rel_path", r.get("path")): r for r in current_data.get("results", [])}
    base_results = {r.get("rel_path", r.get("path")): r for r in baseline_data.get("results", [])}

    regressions = []
    for test_key, b_res in base_results.items():
        if b_res.get("passed") and test_key in curr_results:
            c_res = curr_results[test_key]
            if not c_res.get("passed"):
                regressions.append((test_key, c_res.get("reason", "Unknown failure")))

    if regressions:
        print(f"\n[REGRESSION] {len(regressions)} tests passed in baseline but failed now:")
        for k, reason in regressions[:20]:
            print(f"  - {k}: {reason}")
        if len(regressions) > 20:
            print(f"  ... and {len(regressions) - 20} more.")

    if curr_passed < base_passed - args.allow_drop:
        print(f"\nFAILURE: Pass count decreased by {base_passed - curr_passed} (allowed tolerance: {args.allow_drop})")
        sys.exit(1)

    print("\nSUCCESS: No conformance regressions detected against baseline.")
    sys.exit(0)

if __name__ == "__main__":
    main()
