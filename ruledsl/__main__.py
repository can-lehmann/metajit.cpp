# Copyright 2026 Can Joshua Lehmann
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

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
    parser.add_argument("--include-simplify", action="store_true")
    parser.add_argument("--simplify-rules", type=Path)
    parser.add_argument("--simplify-folds", action="store_true")
    args = parser.parse_args(argv)
    try:
        source = args.rules.read_text()
        result = generate(source, str(args.rules), args.handwritten, args.timeout_ms,
                          include_simplify=args.include_simplify,
                          simplify_source=args.simplify_rules.read_text() if args.simplify_rules else None,
                          simplify_filename=str(args.simplify_rules),
                          simplify_folds=args.simplify_folds)
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
