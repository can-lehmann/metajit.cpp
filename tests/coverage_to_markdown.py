#!/usr/bin/env python3
import argparse
import contextlib
import io
import sys
from pathlib import Path


FIELDS = ("LF", "LH", "FNF", "FNH", "BRF", "BRH")


def read_records(lines):
    records = []
    record = {}
    for line in lines:
        line = line.strip()
        if line == "end_of_record":
            if "SF" in record:
                records.append(record)
            record = {}
        else:
            key, separator, value = line.partition(":")
            if key == "SF":
                record[key] = value
            elif key in FIELDS:
                record[key] = int(value)
    return records


def percentage(hit, found):
    if found == 0:
        return "-"
    value = 100 * hit / found
    color = "🟢" if value >= 90 else "🟡" if value >= 70 else "🔴"
    return f"{color} {value:.2f}%"


def write_row(name, record):
    values = []
    for found, hit in (("LF", "LH"), ("FNF", "FNH"), ("BRF", "BRH")):
        values.append(percentage(record.get(hit, 0), record.get(found, 0)))
    print(f"| {name} | " + " | ".join(values) + " |")


def write_report(records):
    print("| File | Lines | Functions | Branches |")
    print("|------|------:|----------:|---------:|")
    total = dict.fromkeys(FIELDS, 0)
    for record in sorted(records, key=source_name):
        write_row(f"`{Path(record['SF']).name}`", record)
        for key in FIELDS:
            total[key] += record.get(key, 0)
    write_row("**TOTAL**", total)


def source_name(record):
    return record["SF"]


def test():
    source = """SF:/project/b.hpp
FN:1,example
FNDA:3,example
DA:1,3
LF:10
LH:9
FNF:3
FNH:2
BRF:8
BRH:6
end_of_record
SF:/project/a.hpp
LF:2
LH:1
FNF:1
FNH:1
BRF:2
BRH:1
end_of_record
SF:/project/empty.hpp
end_of_record
"""
    output = io.StringIO()
    with contextlib.redirect_stdout(output):
        write_report(read_records(io.StringIO(source)))
    assert output.getvalue() == """| File | Lines | Functions | Branches |
|------|------:|----------:|---------:|
| `a.hpp` | 🔴 50.00% | 🟢 100.00% | 🔴 50.00% |
| `b.hpp` | 🟢 90.00% | 🔴 66.67% | 🟡 75.00% |
| `empty.hpp` | - | - | - |
| **TOTAL** | 🟡 83.33% | 🟡 75.00% | 🟡 70.00% |
"""
    assert percentage(0, 0) == "-"
    assert percentage(0, 10) == "🔴 0.00%"
    assert percentage(7, 10) == "🟡 70.00%"
    assert percentage(9, 10) == "🟢 90.00%"
    print("coverage_to_markdown tests passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", action="store_true")
    if parser.parse_args().test:
        test()
    else:
        write_report(read_records(sys.stdin))


if __name__ == "__main__":
    main()
