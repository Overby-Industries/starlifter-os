#!/usr/bin/env python3
"""Check a fleet wire log against the OTF-1 JSON Schema.

Usage:
    python tools/check_wire_log.py fleet.otf1.jsonl [more.jsonl ...]

Each file is JSON Lines: one OTF-1 packet per line, as written by
`starlifter_fleet_sim --log`.

The C++ in this repo has its own OTF-1 encoder and decoder, and its tests
hold the two to each other. This script is the independent check: it uses
none of that code, only the published schema, so an encoder and decoder
that agree with each other but not with the spec are still caught.

The schema used is the pinned copy in protocol/. Pass --schema to check
against another one, for instance a newer telemetry-protocol checkout:

    python tools/check_wire_log.py --schema ../telemetry-protocol/schema/otf-1.schema.json fleet.otf1.jsonl
"""
import argparse
import collections
import json
import sys
from pathlib import Path

try:
    import jsonschema
except ImportError:
    print("Missing dependency: pip install jsonschema", file=sys.stderr)
    sys.exit(2)

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SCHEMA = REPO_ROOT / "protocol" / "otf-1.schema.json"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("logs", nargs="+", type=Path, help="JSON Lines files of OTF-1 packets")
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA)
    args = parser.parse_args()

    with open(args.schema, encoding="utf-8") as f:
        schema = json.load(f)
    jsonschema.Draft202012Validator.check_schema(schema)
    validator = jsonschema.Draft202012Validator(schema)

    failures = 0
    for path in args.logs:
        kinds = collections.Counter()
        with open(path, encoding="utf-8") as f:
            for number, line in enumerate(f, start=1):
                if not line.strip():
                    continue
                try:
                    packet = json.loads(line)
                except json.JSONDecodeError as exc:
                    failures += 1
                    print(f"FAIL {path}:{number}  not valid JSON: {exc}")
                    continue
                errors = sorted(validator.iter_errors(packet), key=lambda e: list(map(str, e.path)))
                # A broken rule also makes every field of that kind show up
                # as "unevaluated"; report that only when it is the whole story.
                errors = [e for e in errors if e.validator != "unevaluatedProperties"] or errors
                if errors:
                    failures += 1
                    print(f"FAIL {path}:{number}")
                    for err in errors:
                        where = "/".join(str(p) for p in err.path) or "<root>"
                        print(f"  {where}: {err.message}")
                else:
                    kinds[packet.get("kind", "event")] += 1
        total = sum(kinds.values())
        breakdown = ", ".join(f"{count} {kind}" for kind, count in sorted(kinds.items()))
        print(f"{path}: {total} valid OTF-1 packets ({breakdown})")

    if failures:
        print(f"\n{failures} packet(s) are not valid OTF-1.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
