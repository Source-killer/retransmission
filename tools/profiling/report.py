"""Summarize block wall times, preserving per-thread rows in the source CSV."""

import argparse
import csv
from pathlib import Path


def summarize(path):
    totals = {}
    with Path(path).open(newline="") as stream:
        for row in csv.DictReader(stream):
            item = totals.setdefault(row["scope"], {"calls": 0, "total_ms": 0.0, "self_ms": 0.0, "max_ms": 0.0})
            item["calls"] += int(row["calls"])
            for field in ("total_ms", "self_ms"):
                item[field] += float(row[field])
            item["max_ms"] = max(item["max_ms"], float(row["max_ms"]))
    lines = [f"{Path(path).name}: wall time summed across threads",
             f"{'Scope':40} {'Calls':>10} {'Total ms':>12} {'Self ms':>12} {'Max ms':>12}"]
    for name, row in sorted(totals.items(), key=lambda item: item[1]["self_ms"], reverse=True):
        lines.append(f"{name:40} {row['calls']:10d} {row['total_ms']:12.3f} {row['self_ms']:12.3f} {row['max_ms']:12.3f}")
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profiles", nargs="+", type=Path)
    args = parser.parse_args()
    for profile in args.profiles:
        print(summarize(profile), end="\n\n")
