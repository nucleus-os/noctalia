#!/usr/bin/env python3
"""Verify that Noctalia's C++ ABI boundary cannot fall back to libstdc++."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


class ContractError(RuntimeError):
    pass


def run(*argv: str) -> str:
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise ContractError(f"{' '.join(argv)} failed: {result.stderr.strip()}")
    return result.stdout


def needed(path: pathlib.Path) -> set[str]:
    return set(re.findall(r"Shared library: \[([^]]+)]", run("readelf", "-d", str(path))))


def validate(binary: pathlib.Path, prefix: pathlib.Path) -> None:
    libraries = {
        "libsdbus-c++.so.2": prefix / "lib/libsdbus-c++.so.2",
        "libqalculate.so.23": prefix / "lib/libqalculate.so.23",
    }
    if not binary.is_file():
        raise ContractError(f"missing executable: {binary}")
    binary_needed = needed(binary)
    required = set(libraries) | {"libc++.so.1", "libc++abi.so.1"}
    missing = sorted(required - binary_needed)
    if missing:
        raise ContractError("Noctalia is missing private ABI dependencies: " + ", ".join(missing))
    if "libstdc++.so.6" in binary_needed:
        raise ContractError("Noctalia directly depends on libstdc++.so.6")

    dynamic = run("readelf", "-d", str(binary))
    if str(prefix / "lib") not in dynamic:
        raise ContractError("Noctalia RUNPATH does not contain the private dependency directory")

    for soname, path in libraries.items():
        if not path.exists():
            raise ContractError(f"missing private library: {path}")
        deps = needed(path)
        if "libstdc++.so.6" in deps or "libc++.so.1" not in deps:
            raise ContractError(f"{soname} was not built exclusively against libc++")
        symbols = run("nm", "-D", "-C", str(path))
        if "std::__1::" not in symbols or "std::__cxx11::" in symbols:
            raise ContractError(f"{soname} exports the wrong C++ standard-library ABI")

    print(f"validated libc++ ABI contract: {binary} -> {prefix / 'lib'}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--prefix", type=pathlib.Path, required=True)
    parser.add_argument("--stamp", type=pathlib.Path)
    args = parser.parse_args()
    try:
        validate(args.binary, args.prefix)
        if args.stamp:
            args.stamp.write_text("validated\n", encoding="utf-8")
    except ContractError as error:
        print(f"C++ ABI contract error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
