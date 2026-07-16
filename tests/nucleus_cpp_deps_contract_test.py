#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import pathlib
import stat
import tempfile
import types


ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "nucleus_cpp_deps", ROOT / "tools" / "nucleus-cpp-deps.py"
)
assert SPEC is not None and SPEC.loader is not None
CONTRACT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTRACT)


def fake_compiler(root: pathlib.Path, version: str = "21.0.0") -> pathlib.Path:
    path = root / f"clang++-{version}"
    path.write_text(
        "#!/bin/sh\n"
        f"case \"$1\" in\n"
        f"  --version) echo 'clang version {version} (contract test)' ;;\n"
        "  -dumpmachine) echo 'x86_64-unknown-linux-gnu' ;;\n"
        "  *) echo '#define _LIBCPP_VERSION 210106' ;;\n"
        "esac\n",
        encoding="utf-8",
    )
    path.chmod(path.stat().st_mode | stat.S_IXUSR)
    return path


def make_bundle(root: pathlib.Path) -> pathlib.Path:
    prefix = root / "bundle"
    files = list(CONTRACT.FIXED_ARTIFACTS) + [
        "include/sdbus-c++/sdbus-c++.h",
        "include/libqalculate/Calculator.h",
        "include/toml++/toml.hpp",
        "lib/libsdbus-c++.so.2.2.1",
        "lib/libqalculate.so.23.3.10",
    ]
    for index, relative in enumerate(files):
        path = prefix / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(f"artifact-{index}\n".encode())
    for relative, target in CONTRACT.LINKS.items():
        path = prefix / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(target)
    return prefix


def expect_failure(action, message: str) -> None:
    try:
        action()
    except CONTRACT.ContractError:
        return
    raise AssertionError(message)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="noctalia-cpp-contract-") as temporary:
        root = pathlib.Path(temporary)
        prefix = make_bundle(root)
        compiler = fake_compiler(root)
        args = types.SimpleNamespace(prefix=str(prefix), compiler=str(compiler))
        CONTRACT.generate(args)
        CONTRACT.validate(args)

        header = prefix / "include/sdbus-c++/sdbus-c++.h"
        original_header = header.read_bytes()
        header.write_bytes(original_header + b"tampered\n")
        expect_failure(lambda: CONTRACT.validate(args), "tampered artifact passed validation")
        header.write_bytes(original_header)

        link = prefix / "lib/libsdbus-c++.so.2"
        link.unlink()
        link.symlink_to("libsdbus-c++.so.2.2.9")
        expect_failure(lambda: CONTRACT.validate(args), "wrong ABI symlink passed validation")
        link.unlink()
        link.symlink_to("libsdbus-c++.so.2.2.1")

        wrong_compiler = fake_compiler(root, "21.0.1")
        wrong_args = types.SimpleNamespace(prefix=str(prefix), compiler=str(wrong_compiler))
        expect_failure(lambda: CONTRACT.validate(wrong_args), "compiler version drift passed validation")

        manifest = prefix / "share/noctalia-cpp-deps/manifest.json"
        parsed = json.loads(manifest.read_text(encoding="utf-8"))
        assert parsed["packages"] == CONTRACT.PACKAGES
        assert len(parsed["artifacts"]) == len(CONTRACT.FIXED_ARTIFACTS) + 5
    print("private C++ dependency contract rejects artifact, symlink, and compiler drift")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
