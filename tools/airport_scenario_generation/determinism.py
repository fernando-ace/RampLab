"""Create a four-check repeat-run report for Goal 22 from Goal 19 artifacts."""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .run_artifacts import write_determinism_report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control", required=True, type=Path)
    parser.add_argument("--control-repeat", required=True, type=Path)
    parser.add_argument("--disruption", required=True, type=Path)
    parser.add_argument("--disruption-repeat", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        report = write_determinism_report(
            args.control, args.control_repeat, args.disruption, args.disruption_repeat, args.output
        )
        print(json.dumps({name: value["status"] for name, value in report["determinism"].items()}, indent=2))
        return 0
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"scenario-determinism: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
