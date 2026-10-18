#!/usr/bin/env python3
"""How well do a league's (or pool's) armies do against an independent league?

Plays every army of LEAGUE (optionally only those whose origin starts with
--origin) against the Nash mixture of TEST, a league trained separately with
the same battle settings, and prints the weighted scores in blocks of --block
armies in league order, so you can see whether later armies carry over better.

Usage: tools/heldout.py LEAGUE [--test assets/leagues/open-260x128.txt] [--origin pool] [--skip N] [--block 25]
"""

import argparse
import json
import subprocess
from pathlib import Path

import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("league")
    ap.add_argument("--test", default="assets/leagues/open-260x128.txt")
    ap.add_argument("--origin", default="", help="only armies whose origin starts with this")
    ap.add_argument("--skip", type=int, default=0, help="leave out the first N armies (e.g. a seed league)")
    ap.add_argument("--block", type=int, default=25)
    ap.add_argument("--exe", default="build/life_arena")
    ap.add_argument("--csv", default="", help="also write index,origin,score per army")
    args = ap.parse_args()

    weights = np.array([a["weight"] for a in json.loads(Path(args.test + ".json").read_text())["armies"]])
    ids = np.flatnonzero(weights > 1e-6)
    w = weights[ids] / weights[ids].sum()
    armies = [line.rstrip("\n").split("\t")[1:] for line in open(args.league) if line.startswith("army\t")]
    chosen = [(i, o, p) for i, (o, p) in enumerate(armies) if i >= args.skip and o.startswith(args.origin)]

    oracle = subprocess.Popen([args.exe, "oracle", "--league", args.test], stdin=subprocess.PIPE,
                              stdout=subprocess.PIPE, text=True, bufsize=1)
    scores = []
    for _, _, pieces in chosen:
        oracle.stdin.write(f"score\t{','.join(map(str, ids))}\t{pieces}\n")
        oracle.stdin.flush()
        row = oracle.stdout.readline().split()
        assert oracle.stdout.readline().strip() == "end", "oracle error"
        scores.append(np.array(row, float) @ w)
    oracle.stdin.close()
    scores = np.array(scores)

    print(f"{len(chosen)} armies of {args.league} vs the Nash mixture of {args.test} ({len(ids)} armies)")
    for s in range(0, len(scores), args.block):
        b = scores[s : s + args.block]
        print(f"  armies {s + 1:5d}-{s + len(b):<5d} mean {b.mean():+.3f}  best {b.max():+.3f}  "
              f"beat it {np.mean(b > 0):4.0%}")
    print(f"  all: mean {scores.mean():+.3f}, best {scores.max():+.3f}")
    if args.csv:
        with open(args.csv, "w") as f:
            f.write("index,origin,score\n")
            for (i, o, _), sc in zip(chosen, scores):
                f.write(f'{i},"{o}",{sc:.4f}\n')


if __name__ == "__main__":
    main()
