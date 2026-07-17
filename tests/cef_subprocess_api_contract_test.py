#!/usr/bin/env python3

"""Verify that the CEF subprocess wrapper matches the selected libcef."""

from pathlib import Path
import subprocess
import sys


def main() -> int:
    helper = Path(sys.argv[1])
    result = subprocess.run(
        [helper],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    # With no Chromium process type CefExecuteProcess returns -1 (255 as a
    # process status). A wrapper/libcef API mismatch aborts instead, normally
    # with SIGABRT, before reaching this return.
    if result.returncode != 255:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        sys.stderr.write(
            f"CEF helper returned {result.returncode}; expected 255 from "
            "CefExecuteProcess without a subprocess type\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
