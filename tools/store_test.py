#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Daniel Pangerl
"""The preset / pattern store's host test (host/store_test.c): apps/beiwagen/bw_store.h on sys/storage.c over a
simulated NOR, built as the simulator builds (tools/sim.py, ASan/UBSan fatal), then run; the toast's screens go to
build/host/out/store_*.ppm (and .png with Pillow).

  tools/store_test.py [--no-sanitize]
"""
import argparse
import subprocess
import sys

import sim

build = sim.build


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-sanitize", action="store_true")
    a = ap.parse_args()
    build.set_app("beiwagen")
    out = sim.compile_host("store_test", not a.no_sanitize)
    (sim.SRC / "build" / "host" / "out").mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(out)], capture_output=True, text=True, cwd=sim.SRC)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    sim.to_png(r.stdout)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
