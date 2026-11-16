#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Daniel Pangerl
"""The sequencer's host test (host/seq_test.c): apps/beiwagen/bw_seq.h on a simulated TIMER5, its MIDI out checked
(timing, swing, MIDI clock in / out, the arp, recording, blobs, no hanging notes). ASan/UBSan fatal, as tools/sim.py.

  tools/seq_test.py [--no-sanitize]
"""
import argparse
import os
import subprocess
import sys

import sim


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-sanitize", action="store_true")
    a = ap.parse_args()
    sim.build.set_app("beiwagen")
    out = sim.HOST / "seq_test"
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", *sim.host_flags(not a.no_sanitize),
                    f"-I{sim.build.app_dir()}", str(sim.SRC / "host" / "seq_test.c"), "-o", str(out)], check=True)
    return subprocess.run([str(out)], cwd=sim.SRC).returncode


if __name__ == "__main__":
    sys.exit(main())
