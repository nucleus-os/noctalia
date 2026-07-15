#!/usr/bin/env python3
"""Fetch and lay out the CEF (Chromium Embedded Framework) Minimal distribution.

The SDK is large (~315 MB compressed, ~1 GB extracted) and is therefore NOT
committed to the repo. This script downloads the pinned build on demand and
extracts it into third_party/cef/ so the build can link against it.

Usage:
  fetch_cef.py --check     exit 0 if the SDK is already present, non-zero otherwise
  fetch_cef.py --fetch     download + verify + extract (idempotent; no-op if present)
  fetch_cef.py --print-dir print the absolute third_party/cef dir and exit

The pinned version lives in the constants below. Bump all three together
(version string, tarball URL, sha1) when upgrading CEF.
"""

import argparse
import hashlib
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request

# --- Pinned CEF build (stable channel, linux64, Minimal distribution) ---------
CEF_VERSION = "150.0.11+gb887805+chromium-150.0.7871.115"
CEF_TARBALL = "cef_binary_150.0.11+gb887805+chromium-150.0.7871.115_linux64_minimal.tar.bz2"
# The CDN percent-encodes the '+' characters in the path.
CEF_URL = "https://cef-builds.spotifycdn.com/" + CEF_TARBALL.replace("+", "%2B")
CEF_SHA1 = "fab06dea90e96a4804b82941bdca3aecffaa8d3c"
# ------------------------------------------------------------------------------

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
CEF_DIR = os.path.join(REPO_ROOT, "third_party", "cef")
CACHE_DIR = os.path.join(CEF_DIR, ".cache")
# Presence sentinel: the shared library must exist for the SDK to be usable.
SENTINEL = os.path.join(CEF_DIR, "Release", "libcef.so")
VERSION_STAMP = os.path.join(CEF_DIR, "CEF_VERSION")


def is_present() -> bool:
    if not os.path.isfile(SENTINEL):
        return False
    # Guard against a stale extraction from a different pinned version.
    try:
        with open(VERSION_STAMP, encoding="utf-8") as fh:
            return fh.read().strip() == CEF_VERSION
    except OSError:
        return False


def sha1_of(path: str) -> str:
    h = hashlib.sha1()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def download(dest: str) -> None:
    print(f"[fetch_cef] downloading {CEF_URL}", file=sys.stderr)
    tmp = dest + ".part"
    with urllib.request.urlopen(CEF_URL) as resp, open(tmp, "wb") as out:
        total = int(resp.headers.get("Content-Length", 0))
        read = 0
        last_pct = -1
        while True:
            chunk = resp.read(1 << 20)
            if not chunk:
                break
            out.write(chunk)
            read += len(chunk)
            if total:
                pct = read * 100 // total
                if pct != last_pct and pct % 5 == 0:
                    print(f"[fetch_cef]   {pct}% ({read >> 20}/{total >> 20} MiB)", file=sys.stderr)
                    last_pct = pct
    os.replace(tmp, dest)


def ensure_tarball() -> str:
    os.makedirs(CACHE_DIR, exist_ok=True)
    tarball = os.path.join(CACHE_DIR, CEF_TARBALL)
    if os.path.isfile(tarball) and sha1_of(tarball) == CEF_SHA1:
        print("[fetch_cef] using cached tarball", file=sys.stderr)
        return tarball
    download(tarball)
    actual = sha1_of(tarball)
    if actual != CEF_SHA1:
        os.remove(tarball)
        raise SystemExit(
            f"[fetch_cef] sha1 mismatch for {CEF_TARBALL}: expected {CEF_SHA1}, got {actual}"
        )
    print("[fetch_cef] sha1 verified", file=sys.stderr)
    return tarball


def extract(tarball: str) -> None:
    # The tarball contains a single top-level dir `cef_binary_<version>_linux64_minimal/`.
    # We flatten its contents directly into third_party/cef/.
    print("[fetch_cef] extracting...", file=sys.stderr)
    with tempfile.TemporaryDirectory(dir=CEF_DIR) as staging:
        with tarfile.open(tarball, "r:bz2") as tf:
            _safe_extract(tf, staging)
        entries = [e for e in os.listdir(staging) if not e.startswith(".")]
        if len(entries) != 1:
            raise SystemExit(f"[fetch_cef] unexpected tarball layout: {entries}")
        inner = os.path.join(staging, entries[0])
        # Remove any previously-extracted payload dirs, preserving .cache.
        for name in os.listdir(CEF_DIR):
            if name in (".cache", os.path.basename(staging)):
                continue
            path = os.path.join(CEF_DIR, name)
            if os.path.isdir(path) and not os.path.islink(path):
                shutil.rmtree(path)
            else:
                os.remove(path)
        for name in os.listdir(inner):
            shutil.move(os.path.join(inner, name), os.path.join(CEF_DIR, name))
    with open(VERSION_STAMP, "w", encoding="utf-8") as fh:
        fh.write(CEF_VERSION + "\n")
    print(f"[fetch_cef] extracted CEF {CEF_VERSION} into {CEF_DIR}", file=sys.stderr)


def _safe_extract(tf: tarfile.TarFile, path: str) -> None:
    # Reject path traversal (CVE-2007-4559 style) before extracting.
    base = os.path.abspath(path)
    for member in tf.getmembers():
        target = os.path.abspath(os.path.join(path, member.name))
        if not (target == base or target.startswith(base + os.sep)):
            raise SystemExit(f"[fetch_cef] refusing unsafe path in tarball: {member.name}")
    tf.extractall(path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="exit 0 iff SDK present")
    ap.add_argument("--fetch", action="store_true", help="download+extract if absent")
    ap.add_argument("--force", action="store_true", help="re-extract even if present")
    ap.add_argument("--print-dir", action="store_true", help="print third_party/cef dir")
    args = ap.parse_args()

    if args.print_dir:
        print(CEF_DIR)
        return 0
    if args.check:
        return 0 if is_present() else 1
    if args.fetch:
        if is_present() and not args.force:
            print("[fetch_cef] CEF already present; nothing to do", file=sys.stderr)
            return 0
        os.makedirs(CEF_DIR, exist_ok=True)
        tarball = ensure_tarball()
        extract(tarball)
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
