#!/usr/bin/env python3
"""Verify selected CTest names invoke the exact built test executables."""

import argparse
import json
import subprocess
import sys
from pathlib import Path


class RegistrationError(Exception):
    pass


def validate_registration(build, test_data, ninja_output, cases):
    build = Path(build).resolve()
    tests = test_data.get("tests", [])
    by_name = {}
    for test in tests:
        by_name.setdefault(test.get("name"), []).append(test)

    built = {}
    for line in ninja_output.splitlines():
        path, separator, rule = line.partition(": ")
        if separator and "EXECUTABLE" in rule and path.replace("\\", "/").startswith("tests/"):
            built.setdefault(Path(path).name.removesuffix(".exe"), []).append(path)

    errors = []
    for test_name, target_name in cases.items():
        matches = by_name.get(test_name, [])
        if len(matches) != 1:
            errors.append(f"{test_name}: expected exactly one CTest registration, found {len(matches)}")
            continue

        target_paths = built.get(target_name)
        if not target_paths:
            errors.append(f"{test_name}: selected executable target {target_name} was not built")
            continue

        command = matches[0].get("command")
        if not isinstance(command, list) or not command:
            errors.append(f"{test_name}: CTest registration has no executable command")
            continue

        command_executable = Path(command[0])
        if not command_executable.is_absolute():
            command_executable = build / command_executable
        command_executable = command_executable.resolve()
        resolved_targets = [(build / path).resolve() for path in target_paths]
        if command_executable not in resolved_targets:
            errors.append(
                f"{test_name}: CTest command invokes {command_executable}, expected a built {target_name}"
            )
            continue
        if not command_executable.is_file():
            errors.append(f"{test_name}: registered executable does not exist: {command_executable}")

    if errors:
        raise RegistrationError("\n".join(errors))
    return len(cases)


def main():
    parser = argparse.ArgumentParser(
        description="check exact CTest-to-executable pairing for selected built tests"
    )
    parser.add_argument("build", type=Path)
    parser.add_argument("case", nargs="+", help="CTest name=executable target pair")
    args = parser.parse_args()

    cases = {}
    for case in args.case:
        name, separator, target = case.partition("=")
        if not separator or not name or not target or name in cases:
            parser.error(f"invalid or duplicate case mapping: {case}")
        cases[name] = target

    try:
        ctest = subprocess.run(
            ["ctest", "--test-dir", str(args.build), "--show-only=json-v1"],
            capture_output=True,
            text=True,
            check=True,
        )
        ninja = subprocess.run(
            ["ninja", "-C", str(args.build), "-t", "targets", "all"],
            capture_output=True,
            text=True,
            check=True,
        )
        count = validate_registration(args.build, json.loads(ctest.stdout), ninja.stdout, cases)
    except (subprocess.CalledProcessError, json.JSONDecodeError, RegistrationError) as error:
        print(f"selected CTest/executable pairing failed: {error}", file=sys.stderr)
        return 1

    print(f"{count} selected CTest registrations invoke their exact built executable targets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
