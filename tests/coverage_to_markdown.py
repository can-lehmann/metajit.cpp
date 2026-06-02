#!/usr/bin/env python3
import sys

def parse_row(line):
    parts = line.split()
    if len(parts) < 13:
        return None
    filename = parts[0]
    # columns: file, regions, missed_regions, region_cover,
    #          functions, missed_functions, func_cover,
    #          lines, missed_lines, line_cover,
    #          branches, missed_branches, branch_cover
    try:
        return {
            "file": filename,
            "line_cover": parts[9],
            "func_cover": parts[6],
            "region_cover": parts[3],
            "branch_cover": parts[12],
        }
    except IndexError:
        return None

def emoji(pct):
    if pct == "-":
        return ""
    val = float(pct.rstrip("%"))
    if val >= 90:
        return "🟢"
    if val >= 70:
        return "🟡"
    return "🔴"

rows = []
total = None

for line in sys.stdin:
    line = line.rstrip()
    if line.startswith("---") or line.startswith("Filename") or not line:
        continue
    if line.startswith("Files which contain no functions"):
        continue
    if not (line.endswith("%") or line.endswith("-")):
        continue
    row = parse_row(line)
    if row is None:
        continue
    if row["file"] == "TOTAL":
        total = row
    else:
        name = row["file"]
        for prefix in ("metajit.cpp/", "lwir.cpp/", "unittest.cpp/"):
            if name.startswith(prefix):
                name = name[len(prefix):]
                break
        rows.append((name, row))

print("| File | Lines | Functions | Regions | Branches |")
print("|------|------:|----------:|--------:|---------:|")
for name, row in rows:
    lc, fc, rc, bc = row["line_cover"], row["func_cover"], row["region_cover"], row["branch_cover"]
    print(f"| `{name}` | {emoji(lc)} {lc} | {emoji(fc)} {fc} | {emoji(rc)} {rc} | {emoji(bc)} {bc} |")
if total:
    lc, fc, rc, bc = total["line_cover"], total["func_cover"], total["region_cover"], total["branch_cover"]
    print(f"| **TOTAL** | **{emoji(lc)} {lc}** | **{emoji(fc)} {fc}** | **{emoji(rc)} {rc}** | **{emoji(bc)} {bc}** |")
