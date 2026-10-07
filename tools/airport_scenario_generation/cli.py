from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

from .generator import GenerationError, generate, inspect_generated, load_mapping, load_package, validate_generated


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="airport-scenario", description="Generate deterministic RampLab scenarios from Goal 24A canonical packages.")
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("generate", help="Generate simulator scenario, identity map, and provenance manifest.")
    build.add_argument("package", type=Path)
    build.add_argument("--mapping", type=Path, required=True)
    build.add_argument("--output", type=Path, required=True)
    build.add_argument("--seed", type=int)
    build.add_argument("--without-disruptions", action="store_true", help="Generate matched control scenario from the same canonical data.")
    check = commands.add_parser("validate", help="Validate a canonical input/mapping or generated output directory.")
    check.add_argument("package", type=Path, nargs="?")
    check.add_argument("--mapping", type=Path)
    check.add_argument("--generated", type=Path)
    inspect = commands.add_parser("inspect", help="Summarize generated scenario and provenance.")
    inspect.add_argument("generated", type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == "generate":
            result = generate(args.package, args.mapping, args.output, seed=args.seed,
                              without_disruptions=args.without_disruptions)
            validated = validate_generated(args.output)
            print(json.dumps({"generated": result["output_dir"], "validation": validated,
                              "manifest": result["manifest"]}, indent=2, ensure_ascii=False))
        elif args.command == "validate":
            if args.generated:
                print(json.dumps(validate_generated(args.generated), indent=2))
            else:
                if not args.package or not args.mapping:
                    parser.error("validate requires PACKAGE and --mapping, or --generated DIRECTORY")
                load_package(args.package)
                load_mapping(args.mapping)
                print(json.dumps({"valid": True, "canonical_package": str(args.package), "mapping": str(args.mapping)}, indent=2))
        else:
            print(json.dumps(inspect_generated(args.generated), indent=2, ensure_ascii=False))
    except (GenerationError, OSError, ValueError, TypeError, KeyError) as exc:
        print(f"airport-scenario {args.command} failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
