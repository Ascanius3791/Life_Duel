#!/usr/bin/env python3
"""Which army does the battle predictor believe in?

Hill-climbs armies against a league's Nash mixture, judged only by the CNN of
study.py: every step mutates each chain's army LAMBDA times (with life_arena's
own mutations, so every candidate is legal) and keeps the best mutant if the
net predicts a higher score against the mixture than the current army's. All
chains are judged in one batch. At the start and the end (and with
--real-every K every K steps) the armies also play for real, against that mixture
and against the mixture of an independently trained league (--test-league,
same battlefield and rules, none of whose armies the net has seen). The first
partly measures how specialised the league's armies are to each other; only
the second says whether the net's opinion carries over to the game.

With --fitness real the same climb is judged by real battles against the
league instead of the net: the control that shows how far an army optimised
against one league carries over to another one at all.

Each chain starts from one league army (the strongest by Nash weight first)
or from a random army. Writes OUT.csv (the curves) and OUT.json (the final
armies with net and real scores and an army code for the browser's Import).

Usage: tools/surrogate/climb.py [--league assets/leagues/open-twins-260x128.txt]
       [--test-league assets/leagues/open-260x128.txt] [--model data/matches/open.cnn.pt]
       [--top 3] [--random 3] [--steps 200] [--fitness net|real] [--device cuda|cpu]
"""

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from study import CellModel  # noqa: E402


class Oracle:
    """life_arena oracle: legal armies, their bitmaps and real scores."""

    def __init__(self, exe, league, threads):
        cmd = [exe, "oracle", "--league", league] + (["--threads", str(threads)] if threads else [])
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)

    def ask(self, *fields):
        self.p.stdin.write("\t".join(str(f) for f in fields) + "\n")
        self.p.stdin.flush()
        out = []
        while (line := self.p.stdout.readline().rstrip("\n")) != "end":
            if not line or line.startswith("error"):
                raise RuntimeError(f"oracle: {line or 'closed'}")
            out.append(line.split("\t"))
        return out

    def armies(self, *fields):
        rows = self.ask(*fields)
        return [r[-2] for r in rows], unpack([r[-1] for r in rows])

    def score(self, ids, pieces):
        return np.array(self.ask("score", ",".join(map(str, ids)), pieces)[0][0].split(), float)


def unpack(hexes):
    return torch.as_tensor(np.stack([np.frombuffer(bytes.fromhex(h), np.uint8) for h in hexes]))


class Mixture:
    """A league's Nash mixture: its support, weights, armies and an oracle on it."""

    def __init__(self, exe, league, threads):
        weights = np.array([a["weight"] for a in json.loads(Path(league + ".json").read_text())["armies"]], float)
        self.ids = np.flatnonzero(weights > 1e-6)
        self.w = weights[self.ids] / weights[self.ids].sum()
        self.weights = weights
        self.oracle = Oracle(exe, league, threads)
        self.pieces, self.packed = self.oracle.armies("league")
        assert len(self.pieces) == len(weights), f"{league} and its JSON disagree"

    def real(self, pieces):
        return self.oracle.score(self.ids, pieces)


class Judge:
    """The CNN's predicted score of armies against weighted opponents,
    averaged over the battle and (with flips=2) its upside-down copy.

    The head's first layer is linear in the two armies' encodings, so it is
    applied to each army once (its "own" and its "opponent" half) and a pair
    only adds the two halves; the rest of the head is a few small layers.
    That makes a pair about ten times cheaper than CellModel.forward."""

    def __init__(self, model_path, meta, device="cpu", flips=2):
        state = torch.load(model_path, map_location="cpu")
        self.net = CellModel(state["raster"][:1], meta["height"], meta["zoneWidth"],
                             flip_y=meta["symmetry"] == "rotation")
        state["raster"] = self.net.raster
        self.net.load_state_dict(state)
        self.net.to(device).eval()
        self.device, self.flips = device, (False, True)[:flips]
        first = self.net.head[0]
        f = first.in_channels // 2
        self.w_own, self.w_opp, self.bias = first.weight[:, :f], first.weight[:, f:], first.bias
        self.rest = self.net.head[1:]

    @torch.no_grad()
    def features(self, packed):
        """Per flip: (own half, opponent half) of the head's first layer."""
        out = []
        for flip in self.flips:
            self.net.raster = packed.to(self.device)
            e = self.net.encode(torch.arange(len(packed), device=self.device), flip)
            seen = e.flip(2) if self.net.flip_y else e  # how this army looks from the other side
            conv = lambda x, w: torch.nn.functional.conv1d(x, w, padding=1)  # noqa: E731
            out.append((conv(e, self.w_own), conv(seen, self.w_opp)))
        return out

    def opponents(self, packed, weights):
        return self.features(packed), torch.as_tensor(weights, dtype=torch.float32, device=self.device)

    @torch.no_grad()
    def __call__(self, feats, opp, chunk=4096):
        """Predicted weighted score of every army in `feats` against `opp`."""
        ofeats, w = opp
        total = 0
        for (own, seen), (o_own, o_seen) in zip(feats, ofeats):
            k = len(o_own)
            rows = max(1, chunk // k)
            parts = []
            for s in range(0, len(own), rows):
                a_own, a_seen = own[s : s + rows, None], seen[s : s + rows, None]
                c = len(a_own)
                b = self.bias[None, None, :, None]
                ab = self.rest((a_own + o_seen[None] + b).flatten(0, 1))
                ba = self.rest((o_own[None] + a_seen + b).flatten(0, 1))
                parts.append(torch.tanh(ab - ba).view(c, k) @ w)
            total = total + torch.cat(parts)
        return (total / len(feats)).cpu().numpy()

    def take(self, feats, i):
        return [(own[i : i + 1], seen[i : i + 1]) for own, seen in feats]


def army_code(pieces, meta, name):
    out = []
    for t in filter(None, pieces.split(";")):
        pat, rest = t.split("@")
        x, y, r, m = rest.split(",")
        out.append(f"{pat}.{x}.{y}.{r[1:]}.{m[1:]}")
    return f"LD1|{meta['zoneWidth']}x{meta['height']}|{name.replace(' ', '%20')}|{','.join(out)}"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--league", default="assets/leagues/open-twins-260x128.txt")
    ap.add_argument("--test-league", default="assets/leagues/open-260x128.txt",
                    help="independently trained league, only for the real check")
    ap.add_argument("--model", default="data/matches/open.cnn.pt")
    ap.add_argument("--meta", default=None, help="raster meta JSON (default: next to the model)")
    ap.add_argument("--exe", default="build/life_arena")
    ap.add_argument("--fitness", choices=["net", "real"], default="net",
                    help="what the climb maximises: the net's prediction or real battles (control)")
    ap.add_argument("--top", type=int, default=3, help="chains starting from the strongest league armies")
    ap.add_argument("--random", type=int, default=3, help="chains starting from random armies")
    ap.add_argument("--steps", type=int, default=200)
    ap.add_argument("--lam", type=int, default=64, help="mutants per chain and step")
    ap.add_argument("--real-every", type=int, default=0,
                    help="also play the current armies for real every K steps (default: only at start and end)")
    ap.add_argument("--flips", type=int, choices=[1, 2], default=2,
                    help="2: average the net over each battle and its upside-down copy")
    ap.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    ap.add_argument("--threads", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="data/climb/open")
    args = ap.parse_args()
    torch.manual_seed(0)

    meta = json.loads(Path(args.meta or args.model.replace(".cnn.pt", ".raster.json")).read_text())
    own = Mixture(args.exe, args.league, args.threads)
    test = Mixture(args.exe, args.test_league, args.threads)
    oracle, weights = own.oracle, own.weights
    judge = Judge(args.model, meta, args.device, args.flips)
    vs_own = judge.opponents(own.packed[own.ids], own.w)
    vs_test = judge.opponents(test.packed[test.ids], test.w)

    tops = np.argsort(-weights, kind="stable")[: args.top]
    rp, rk = oracle.armies("random", args.random, args.seed) if args.random else ([], None)
    chains = [{"name": f"league #{i}", "army": own.pieces[i], "packed": own.packed[i : i + 1]} for i in tops]
    chains += [{"name": f"random {j + 1}", "army": rp[j], "packed": rk[j : j + 1]} for j in range(len(rp))]

    def fitness(armies, packed):
        if args.fitness == "net":
            return judge(judge.features(packed), vs_own)
        return np.array([own.real(a) @ own.w for a in armies])

    for c, f in zip(chains, fitness([c["army"] for c in chains], torch.cat([c["packed"] for c in chains]))):
        c["fit"], c["accepted"] = float(f), 0

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    log = open(args.out + ".csv", "w")
    log.write("chain,start,step,accepted,fitness,net,real,net_test,real_test,seconds,pieces\n")
    t0 = time.time()
    climb_seconds = 0.0

    def report(step):
        feats = judge.features(torch.cat([c["packed"] for c in chains]))
        nets, nets_test = judge(feats, vs_own), judge(feats, vs_test)
        for ci, c in enumerate(chains):
            c["net"], c["netTest"] = float(nets[ci]), float(nets_test[ci])
            c["real"], c["realTest"] = float(own.real(c["army"]) @ own.w), float(test.real(c["army"]) @ test.w)
            print(f"[{c['name']}] step {step:4d}: league net {c['net']:+.3f} real {c['real']:+.3f} | "
                  f"test net {c['netTest']:+.3f} real {c['realTest']:+.3f}  ({c['accepted']} accepted)", flush=True)
            log.write(f"{ci},{c['name']},{step},{c['accepted']},{c['fit']:.4f},{c['net']:.4f},{c['real']:.4f},"
                      f"{c['netTest']:.4f},{c['realTest']:.4f},{time.time() - t0:.1f},{c['army']}\n")
        log.flush()

    report(0)
    rng = np.random.default_rng(args.seed)
    for step in range(1, args.steps + 1):
        t = time.time()
        cands = [oracle.armies("mutate", args.lam, int(rng.integers(1 << 62)), c["army"]) for c in chains]
        scores = fitness([a for armies, _ in cands for a in armies], torch.cat([p for _, p in cands]))
        for ci, (c, (armies, packed)) in enumerate(zip(chains, cands)):
            s = scores[ci * args.lam : (ci + 1) * args.lam]
            i = int(np.argmax(s))
            if s[i] > c["fit"]:
                c["army"], c["packed"], c["fit"] = armies[i], packed[i : i + 1], float(s[i])
                c["accepted"] += 1
        climb_seconds += time.time() - t
        if args.real_every and step % args.real_every == 0 and step < args.steps:
            report(step)
    report(args.steps)
    n = args.steps * args.lam * len(chains)
    print(f"climb: {n:,} candidates judged in {climb_seconds:.1f}s ({n / climb_seconds:,.0f}/s, "
          f"{args.fitness}, {args.device})")

    label = "Net" if args.fitness == "net" else "Real"
    results = [{"start": c["name"], "pieces": c["army"], "fitness": c["fit"],
                **{k: c[k] for k in ("net", "real", "netTest", "realTest")},
                "code": army_code(c["army"], meta, f"{label} climb {ci + 1}")} for ci, c in enumerate(chains)]
    Path(args.out + ".json").write_text(json.dumps({"league": args.league, "testLeague": args.test_league,
                                                    "model": args.model, "args": vars(args),
                                                    "climbSeconds": climb_seconds, "armies": results}, indent=1))
    print(f"wrote {args.out}.csv and {args.out}.json")


if __name__ == "__main__":
    main()
