#!/usr/bin/env python3
"""Check that every requirement in the spec is referenced by at least one test (T-14).

Requirement ids are taken from their definitions in spec.md (lines like "- **R-VAL-01.** ...").
A requirement counts as covered when its id appears in a file under tests/, either as written
(R-VAL-01, in comments) or with underscores (R_VAL_01, in test names).

Exit status is 0 when every requirement is covered and 1 otherwise.

Usage:
    tools/spec_coverage.py                 # check, list missing ids
    tools/spec_coverage.py --markdown      # also print a requirement -> test files table
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFINITION = re.compile(r"\*\*(R-[A-Z]+-\d+)\.\*\*")
TEST_SUFFIXES = {".cpp", ".hpp", ".h"}


def requirement_ids(spec: Path) -> list[str]:
    ids = DEFINITION.findall(spec.read_text(encoding="utf-8"))
    seen = set()
    return [i for i in ids if not (i in seen or seen.add(i))]


def reference_pattern(req_id: str) -> re.Pattern:
    # R-VAL-01 or R_VAL_01, not followed by another digit (so R-VAL-1 never matches R-VAL-10).
    _, area, number = req_id.split("-")
    return re.compile(rf"\bR[-_]{area}[-_]{number}(?!\d)")


def coverage(ids: list[str], tests: Path) -> dict[str, list[str]]:
    files = sorted(p for p in tests.rglob("*") if p.is_file() and p.suffix in TEST_SUFFIXES)
    texts = {p: p.read_text(encoding="utf-8") for p in files}
    result = {}
    for req_id in ids:
        pattern = reference_pattern(req_id)
        result[req_id] = [str(p.relative_to(ROOT)) for p, t in texts.items() if pattern.search(t)]
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--spec", type=Path, default=ROOT / "specs" / "spec.md")
    parser.add_argument("--tests", type=Path, default=ROOT / "tests")
    parser.add_argument("--markdown", action="store_true", help="print a coverage table")
    args = parser.parse_args()

    ids = requirement_ids(args.spec)
    if not ids:
        print(f"no requirement ids found in {args.spec}", file=sys.stderr)
        return 1
    cov = coverage(ids, args.tests.resolve())
    missing = [i for i in ids if not cov[i]]

    if args.markdown:
        print("| Requirement | Tests |")
        print("|---|---|")
        for req_id in ids:
            files = ", ".join(f"`{Path(f).name}`" for f in cov[req_id]) or "**missing**"
            print(f"| {req_id} | {files} |")
        print()

    print(f"{len(ids) - len(missing)}/{len(ids)} requirements referenced by tests")
    if missing:
        print("missing: " + ", ".join(missing), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
