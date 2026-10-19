import argparse
from pathlib import Path
import sys

from .generate import generate
from .signatures import SIGNATURES
from .source import SourceError


def main(argv=None):
    parser = argparse.ArgumentParser(description="Prove DSL rules and emit C++ builder methods")
    parser.add_argument("rules", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--handwritten", action="append", choices=sorted(SIGNATURES), default=[])
    parser.add_argument("--timeout-ms", type=int, default=120000)
    args = parser.parse_args(argv)
    try:
        source = args.rules.read_text()
        result = generate(source, str(args.rules), args.handwritten, args.timeout_ms)
        if args.output is None:
            sys.stdout.write(result)
        else:
            args.output.write_text(result)
    except (SourceError, ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
