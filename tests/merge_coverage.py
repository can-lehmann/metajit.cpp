import argparse
from pathlib import Path
import subprocess
import tempfile


def normalize_functions(traces):
    locations = {}
    for trace in traces:
        filename = None
        for line in trace:
            if line.startswith("SF:"):
                filename = line[3:]
            elif line.startswith("FN:"):
                start, name = line[3:].split(",", 1)
                key = filename, name
                locations[key] = max(locations.get(key, 0), int(start))
    result = []
    for trace in traces:
        filename = None
        normalized = []
        for line in trace:
            if line.startswith("SF:"):
                filename = line[3:]
            elif line.startswith("FN:"):
                start, name = line[3:].split(",", 1)
                line = f"FN:{locations[filename, name]},{name}"
            normalized.append(line)
        result.append(normalized)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--output")
    parser.add_argument("traces", nargs="*")
    args = parser.parse_args()
    if args.test:
        test()
        return
    if not args.output or not args.traces:
        parser.error("--output and at least one trace are required")
    traces = [Path(path).read_text().splitlines() for path in args.traces]
    with tempfile.TemporaryDirectory() as directory:
        command = ["lcov", "--branch-coverage", "--filter", "blank,range",
                   "--rc", "derive_function_end_line=0",
                   "--rc", "check_data_consistency=0"]
        for index, trace in enumerate(normalize_functions(traces)):
            path = Path(directory) / f"{index}.lcov"
            path.write_text("\n".join(trace) + "\n")
            command.extend(["-a", str(path)])
        subprocess.run(command + ["-o", args.output], check=True)


def test():
    unused = ["SF:/project/a.hpp", "FN:10,foo", "FNDA:0,foo",
              "DA:10,0", "end_of_record"]
    used = ["SF:/project/a.hpp", "FN:12,foo", "FNDA:3,foo",
            "DA:12,3", "end_of_record"]
    forward = normalize_functions([unused, used])
    reverse = normalize_functions([used, unused])
    assert forward == list(reversed(reverse))
    assert forward[0] == ["SF:/project/a.hpp", "FN:12,foo",
                          "FNDA:0,foo", "DA:10,0", "end_of_record"]
    assert forward[1] == used
    traces = [["SF:/project/a.hpp", "FN:10,foo", "end_of_record"],
              ["SF:/project/b.hpp", "FN:20,foo", "end_of_record"]]
    assert normalize_functions(traces) == traces
    print("merge_coverage tests passed")


if __name__ == "__main__":
    main()
