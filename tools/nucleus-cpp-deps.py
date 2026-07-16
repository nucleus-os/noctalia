#!/usr/bin/env python3
"""Generate and validate Noctalia's private libc++ dependency bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
from typing import Any


SCHEMA_VERSION = 1
PACKAGES = {
    "sdbus-cpp": "2.2.1",
    "libqalculate": "5.9.0",
    "tomlplusplus": "3.4.0",
}
SOURCES = {
    "sdbus-cpp": {
        "url": "https://github.com/Kistler-Group/sdbus-cpp/archive/refs/tags/v2.2.1.tar.gz",
        "sha256": "da69a0104beb6e51415a59f1571a47beb1eacc65cc6027b250eb1cf13ff4f802",
    },
    "libqalculate": {
        "url": "https://github.com/Qalculate/libqalculate/releases/download/v5.9.0/libqalculate-5.9.0.tar.gz",
        "sha256": "94d734b9303b3b68df61e4255f2eddeee346b66ec4b6e134f19e1a3cc3ff4a09",
    },
    "tomlplusplus": {
        "url": "https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz",
        "sha256": "8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155",
    },
}
HEADER_ROOTS = ["include/sdbus-c++", "include/libqalculate", "include/toml++"]
FIXED_ARTIFACTS = [
    "lib/libc++.so",
    "lib/libc++.so.1",
    "lib/libc++abi.so",
    "lib/libc++abi.so.1",
    "lib/libunwind.so",
    "lib/libunwind.so.1",
]
LINKS = {
    "lib/libsdbus-c++.so": "libsdbus-c++.so.2",
    "lib/libsdbus-c++.so.2": "libsdbus-c++.so.2.2.1",
    "lib/libqalculate.so": "libqalculate.so.23.3.10",
    "lib/libqalculate.so.23": "libqalculate.so.23.3.10",
}


class ContractError(RuntimeError):
    pass


def run(argv: list[str], *, input_text: str | None = None) -> str:
    try:
        result = subprocess.run(
            argv, input=input_text, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, check=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", "") or str(error)
        raise ContractError(f"command failed: {' '.join(argv)}: {detail.strip()}") from error
    return result.stdout.strip()


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def compiler_identity(compiler: str) -> dict[str, Any]:
    first_line = run([compiler, "--version"]).splitlines()[0]
    match = re.search(r"clang version\s+([^\s]+)", first_line)
    if not match:
        raise ContractError(f"private dependencies require Clang; got {first_line!r}")
    macros = run(
        [compiler, "-dM", "-E", "-x", "c++", "-include", "version", "-"],
        input_text="\n",
    )
    libcxx = re.search(r"^#define _LIBCPP_VERSION\s+(\d+)\s*$", macros, re.MULTILINE)
    if not libcxx:
        raise ContractError("private dependencies require libc++; _LIBCPP_VERSION is absent")
    return {
        "id": "clang",
        "version": match.group(1),
        "version_line": first_line,
        "target_triple": run([compiler, "-dumpmachine"]),
        "libcxx": {"version": int(libcxx.group(1)), "abi_namespace": "__1"},
    }


def artifact_paths(prefix: pathlib.Path) -> list[str]:
    paths = list(FIXED_ARTIFACTS)
    for relative_root in HEADER_ROOTS:
        root = prefix / relative_root
        if not root.is_dir():
            raise ContractError(f"private dependency bundle is missing header tree {relative_root}")
        paths.extend(
            path.relative_to(prefix).as_posix()
            for path in sorted(root.rglob("*"))
            if path.is_file()
        )
    for pattern, label in (
        ("libsdbus-c++.so.*.*.*", "sdbus-c++ shared library"),
        ("libqalculate.so.*.*.*", "libqalculate shared library"),
    ):
        matches = sorted(path.name for path in (prefix / "lib").glob(pattern) if path.is_file())
        if len(matches) != 1:
            raise ContractError(f"expected one {label}, found: {matches}")
        paths.append("lib/" + matches[0])
    return sorted(paths)


def artifact_inventory(prefix: pathlib.Path) -> list[dict[str, str]]:
    inventory = []
    for relative in artifact_paths(prefix):
        path = prefix / relative
        if not path.is_file():
            raise ContractError(f"private dependency bundle is missing {relative}")
        inventory.append({"file": relative, "sha256": sha256(path)})
    return inventory


def validate_links(prefix: pathlib.Path) -> None:
    for relative, expected_target in LINKS.items():
        path = prefix / relative
        if not path.is_symlink():
            raise ContractError(f"private dependency bundle requires symlink {relative}")
        if path.readlink() != pathlib.Path(expected_target):
            raise ContractError(
                f"{relative} points to {path.readlink()}, expected {expected_target}"
            )
        if not path.resolve().is_file():
            raise ContractError(f"private dependency symlink is broken: {relative}")


def require_equal(label: str, actual: Any, expected: Any) -> None:
    if actual != expected:
        raise ContractError(f"{label} mismatch: bundle has {expected!r}, build has {actual!r}")


def generate(args: argparse.Namespace) -> None:
    prefix = pathlib.Path(args.prefix).resolve()
    validate_links(prefix)
    manifest = {
        "schema_version": SCHEMA_VERSION,
        "bundle": "noctalia-cpp-deps",
        "compiler": compiler_identity(args.compiler),
        "packages": PACKAGES,
        "sources": SOURCES,
        "artifacts": artifact_inventory(prefix),
        "links": LINKS,
    }
    output = prefix / "share" / "noctalia-cpp-deps" / "manifest.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(output)


def validate(args: argparse.Namespace) -> None:
    prefix = pathlib.Path(args.prefix).resolve()
    manifest_path = prefix / "share" / "noctalia-cpp-deps" / "manifest.json"
    if not manifest_path.is_file():
        raise ContractError(f"private dependency bundle is unversioned: missing {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ContractError(f"invalid private dependency manifest: {error}") from error
    require_equal("manifest schema", SCHEMA_VERSION, manifest.get("schema_version"))
    require_equal("bundle kind", "noctalia-cpp-deps", manifest.get("bundle"))
    identity = compiler_identity(args.compiler)
    recorded = manifest.get("compiler", {})
    for key in ("id", "version", "target_triple"):
        require_equal(f"compiler {key}", identity.get(key), recorded.get(key))
    require_equal("libc++ identity", identity.get("libcxx"), recorded.get("libcxx"))
    require_equal("dependency versions", PACKAGES, manifest.get("packages"))
    require_equal("source provenance", SOURCES, manifest.get("sources"))
    require_equal("symlink contract", LINKS, manifest.get("links"))
    validate_links(prefix)
    require_equal("artifact inventory", artifact_inventory(prefix), manifest.get("artifacts"))
    print(f"validated private libc++ dependency bundle: {prefix}")


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="action", required=True)
    for action in ("generate", "validate"):
        command = subparsers.add_parser(action)
        command.add_argument("--prefix", required=True)
        command.add_argument("--compiler", required=True)
    args = parser.parse_args()
    try:
        (generate if args.action == "generate" else validate)(args)
    except ContractError as error:
        print(f"private C++ dependency error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
