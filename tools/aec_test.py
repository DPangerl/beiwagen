#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Daniel Pangerl
"""The loop canceller's host test (host/aec_test.c): apps/beiwagen's bw_aec.cc built as the simulator
builds them (tools/sim.py, ASan/UBSan fatal), then run: build/host/out/aec_*.wav.

  tools/aec_test.py [--no-sanitize]
"""
import argparse
import os
import subprocess
import sys

import sim

build = sim.build


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-sanitize", action="store_true")
    a = ap.parse_args()
    build.set_app("beiwagen")
    sanitize = not a.no_sanitize
    objs = [o for o in sim.compile_cxx(sanitize) if o.name.startswith("firmware_apps_beiwagen_") and
            o.name.endswith("bw_aec.o")]
    out = sim.HOST / "aec_test"
    inc = [f"-I{sim.SRC / d}" for d in ("firmware/sys", build.app_dir().relative_to(sim.SRC))]
    main_o = sim.HOST / "aec_test.o"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", *sim.host_flags(sanitize), *inc, "-c",
                    str(sim.SRC / "host" / "aec_test.c"), "-o", str(main_o)], check=True, cwd=sim.SRC)
    subprocess.run([os.environ.get("CXX", "c++"), *sim.host_flags(sanitize), str(main_o), *map(str, objs), "-lm",
                    "-o", str(out)], check=True)
    (sim.SRC / "build" / "host" / "out").mkdir(parents=True, exist_ok=True)
    return subprocess.run([str(out)], cwd=sim.SRC).returncode


if __name__ == "__main__":
    sys.exit(main())
