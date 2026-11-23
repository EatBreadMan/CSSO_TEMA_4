"""CSSO 2026-2027 common experiment checker helper."""
from __future__ import annotations
from pathlib import Path
import re
import sys

class Report:
    def __init__(self, assignment: str):
        self.assignment = assignment
        self.results = []
        self.infos = []

    def info(self, text: str) -> None:
        self.infos.append(text)
        print(f"[INFO] {text}")

    def test(self, code: str, text: str, passed: bool, detail: str = "") -> None:
        self.results.append((code, text, passed, detail))
        if passed:
            print(f"[OK] {code} {text}")
        else:
            msg = f"[FAIL] {code} {text}"
            if detail:
                msg += f" — {detail}"
            print(msg)

    def summary(self) -> int:
        passed = sum(1 for _, _, ok, _ in self.results if ok)
        total = len(self.results)
        print(f"Summary: {passed}/{total} automated tests passed")
        return 0 if (total > 0 and passed == total) else 1

def integer(val) -> int | None:
    try:
        return int(val)
    except (ValueError, TypeError):
        return None

def parse_params(param_str: str) -> dict[str, str]:
    res = {}
    for part in param_str.split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            res[k.strip()] = v.strip()
    return res

def select(rows: list[dict], exp: str, **filters) -> list[dict]:
    res = []
    for r in rows:
        if r.get("experiment") != exp:
            continue
        params = r.get("_param")
        if params is None:
            params = parse_params(r.get("parameters", ""))
            r["_param"] = params
        match = True
        for k, v in filters.items():
            if params.get(k) != str(v):
                match = False
                break
        if match:
            res.append(r)
    return res

def error_codes(rows: list[dict]) -> list[int]:
    codes = []
    for r in rows:
        c = integer(r.get("error_code"))
        if c is not None:
            codes.append(c)
    return codes

def check_measurement(r: Report, code: str, rows: list[dict], exp: str, key: str, values: list[str], units: set[str], other_filters: dict | None = None) -> dict:
    if other_filters is None:
        other_filters = {}
    medians = {}
    all_ok = True
    problems = []
    for val in values:
        filt = dict(other_filters)
        filt[key] = val
        sub = select(rows, exp, **filt)
        if not sub:
            all_ok = False
            problems.append(f"no measurements for {key}={val}")
            continue
        
        reps = [r.get("repetition") for r in sub]
        if "0" not in reps:
            all_ok = False
            problems.append(f"{key}={val} missing warm-up repetition 0")
        
        measured = [r for r in sub if r.get("repetition") != "0"]
        if len(measured) < 5:
            all_ok = False
            problems.append(f"{key}={val} has {len(measured)} measurements (< 5 required)")
            
        for row in sub:
            if row.get("unit") not in units:
                all_ok = False
                problems.append(f"invalid unit {row.get('unit')}")
            if row.get("error_code") != "0":
                all_ok = False
                problems.append(f"error_code != 0: {row.get('error_code')}")
                
        vals = []
        for row in measured:
            try:
                vals.append(float(row.get("value", 0)))
            except ValueError:
                pass
        if vals:
            vals.sort()
            medians[val] = vals[len(vals) // 2]
            
    filt_desc = ";".join(f"{k}={v}" for k, v in other_filters.items())
    desc = f"E1 ({filt_desc}): warm-up + ≥5 measurements for {key} ∈ {values}"
    r.test(code, desc, all_ok and len(problems) == 0, "; ".join(problems[:3]))
    return medians

def check_cleanup(r: Report, code: str, rows: list[dict], tolerance: int, text: str) -> None:
    bad = []
    for row in rows:
        b = integer(row.get("handles_before"))
        a = integer(row.get("handles_after"))
        if b is None or a is None:
            bad.append("missing handle count")
        elif a - b > tolerance:
            bad.append(f"{a} - {b} = {a - b} > {tolerance}")
    r.test(code, text, len(bad) == 0, "; ".join(bad[:3]))
