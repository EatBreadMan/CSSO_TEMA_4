#!/usr/bin/env python3
"""Runner for CSSO experiment checks."""
import argparse
import csv
import importlib
import sys
from pathlib import Path

# Add script directory and C:\T4 to sys.path
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
if Path(r"C:\T4").exists() and r"C:\T4" not in sys.path:
    sys.path.append(r"C:\T4")

import experiment_common as ec

def main():
    parser = argparse.ArgumentParser(description="Experiment checker")
    parser.add_argument("assignment", help="Assignment name, e.g. T4")
    parser.add_argument("--dir", default=None, help="Directory containing experiments.tsv")
    parser.add_argument("--md", default=None, help="Path to EXPERIMENTS.md")
    args = parser.parse_args()

    try:
        rules = importlib.import_module("experiment_rules")
    except ImportError as e:
        print(f"[FAIL] Could not import experiment_rules: {e}")
        return 1

    exp_dir = Path(args.dir) if args.dir else Path(rules.DEFAULT_DIR)
    tsv_path = exp_dir / "experiments.tsv"
    
    r = ec.Report(args.assignment)
    if not tsv_path.exists():
        print(f"[FAIL] Missing {tsv_path}")
        return 1

    rows = []
    with open(tsv_path, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            rows.append(row)

    rules.check(r, rows, exp_dir)

    if args.md:
        md_path = Path(args.md)
        if not md_path.exists():
            r.test("T4.ED1", f"EXPERIMENTS.md exists at {md_path}", False, "file missing")
        else:
            text = md_path.read_text(encoding="utf-8")
            has_pred = "prediction" in text.lower() or "predictie" in text.lower()
            has_exp = "explanation" in text.lower() or "explicat" in text.lower()
            r.test("T4.ED1", "EXPERIMENTS.md has prediction and explanation sections", has_pred and has_exp, "sections missing")

    return r.summary()

if __name__ == "__main__":
    sys.exit(main())
