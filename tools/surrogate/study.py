#!/usr/bin/env python3
"""Can a model predict Life Duel battles well enough to steer the search?

Reads the battles recorded by `life_arena train --log-matches P` (and the army
bitmaps from `life_arena raster`), trains predictors of the battle score on the
older battles and measures them on the newer ones, with the numbers that
matter for the evolutionary search:

  group rank   Spearman correlation, inside one search generation, between the
               true fitness of the candidates (mean score against the opponents
               they all played) and the predicted one
  best kept    how often the generation's best candidate survives if only the
               predicted top quarter is simulated (random screening: 0.25)
  siblings     how often the better of two near-identical candidates (at most
               two pieces moved, added, removed or swapped) is predicted to be
               the better one (coin flip: 0.5); this is where chaos hurts most

Predictors: a bag-of-pieces model (piece counts with a learnt rock-paper-
scissors matrix), a small CNN on the armies' cells, and, as the non-learnt
alternative, short battles (the score after 50 ... 1200 generations).

Usage: tools/surrogate/study.py [--prefix data/matches/open] [--epochs 4]
"""

import argparse
import json
import math
import time
from pathlib import Path

import numpy as np
import pandas as pd
import torch
import torch.nn as nn
from scipy.stats import spearmanr

TRACE = [50, 100, 200, 400, 800, 1200]


# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------
def load(prefix):
    meta = json.loads(Path(prefix + ".raster.json").read_text())
    raster = np.fromfile(prefix + ".raster.bin", dtype=np.uint8)
    per = (meta["zoneWidth"] * meta["height"] + 7) // 8
    raster = raster[: len(raster) // per * per].reshape(-1, per)

    tokens = []
    with open(prefix + ".armies.tsv") as f:
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) == 2:
                tokens.append(parts[1].split(";") if parts[1] else [])
    n = min(len(tokens), len(raster))

    df = pd.read_csv(prefix + ".matches.csv", on_bad_lines="skip").dropna()
    df = df[(df.red < n) & (df.blue < n)].astype({"batch": np.int64, "red": np.int64, "blue": np.int64})
    return meta, raster[:n], tokens[:n], df.reset_index(drop=True)


def short_score(df, g):
    """The battle score as if it had been decided at generation g."""
    r, b = df[f"red{g}"].to_numpy(float), df[f"blue{g}"].to_numpy(float)
    total = r + b
    margin = np.where(total > 0, (r - b) / np.maximum(total, 1), 0)
    return 0.7 * np.sign(r - b) + 0.3 * margin


def candidate_groups(df):
    """Candidates of one search generation: the red armies of a batch that
    played the same set of opponents. Returns a frame indexed by (batch, red)
    with the group id and the true fitness."""
    by = df.groupby(["batch", "red"])
    opp = by["blue"].agg(lambda s: hash(tuple(sorted(s.tolist()))))
    cand = pd.DataFrame({"opp": opp, "n_opp": by["blue"].size(), "fit": by["score"].mean()})
    cand["group"] = cand.groupby([cand.index.get_level_values(0), "opp"]).ngroup()
    size = cand.groupby("group")["fit"].transform("size")
    return cand[(size >= 8) & (cand.n_opp >= 3)]


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------
def group_metrics(df, cand, pred, tokens, keep=0.25):
    """`pred` holds one predicted score per battle of df."""
    pfit = pd.Series(pred, index=df.index).groupby([df.batch, df.red]).mean()
    c = cand.join(pfit.rename("pfit"), how="inner")
    rhos, kept = [], []
    sib_hit = sib_n = 0
    for _, g in c.groupby("group"):
        fit, pf = g.fit.to_numpy(), g.pfit.to_numpy()
        if np.ptp(fit) > 0 and np.ptp(pf) > 0:
            rhos.append(spearmanr(fit, pf).statistic)
        k = max(1, math.ceil(keep * len(g)))
        kept.append(float(np.argmax(fit) in np.argsort(-pf, kind="stable")[:k]))
        reds = g.index.get_level_values(1).to_numpy()
        sets = [set(tokens[r]) for r in reds]
        for i in range(len(reds)):
            for j in range(i + 1, len(reds)):
                if abs(fit[i] - fit[j]) > 0.05 and len(sets[i] ^ sets[j]) <= 4:
                    sib_n += 1
                    sib_hit += (fit[i] > fit[j]) == (pf[i] > pf[j])
    return {
        "groups": len(kept),
        "group_rank": float(np.mean(rhos)) if rhos else float("nan"),
        "best_kept": float(np.mean(kept)) if kept else float("nan"),
        "sibling_pairs": sib_n,
        "siblings": sib_hit / sib_n if sib_n else float("nan"),
    }


def battle_metrics(y, p):
    return {
        "rmse": float(np.sqrt(np.mean((y - p) ** 2))),
        "corr": float(np.corrcoef(y, p)[0, 1]) if np.std(p) > 0 else 0.0,
        "sign": float(np.mean(np.sign(y) == np.sign(p))),
    }


# ---------------------------------------------------------------------------
# Models (both antisymmetric: f(a, b) = -f(b, a), like the game)
# ---------------------------------------------------------------------------
class PieceModel(nn.Module):
    """Piece counts c: f = tanh(w.(ca - cb) + ca^T (M - M^T) cb)."""

    def __init__(self, counts):
        super().__init__()
        self.register_buffer("counts", counts)
        k = counts.shape[1]
        self.w = nn.Parameter(torch.zeros(k))
        self.m = nn.Parameter(torch.zeros(k, k))

    def forward(self, a, b):
        ca, cb = self.counts[a], self.counts[b]
        anti = self.m - self.m.T
        return torch.tanh((ca - cb) @ self.w + ((ca @ anti) * cb).sum(1))


class CellModel(nn.Module):
    """A CNN encodes each army's red zone into one feature vector per band of
    4 board rows (keeping the coarse x position); a 1-D head compares the two
    armies band by band. Blue's army is given in its own red-zone coordinates,
    which (for mirror symmetry) are exactly how it faces Red."""

    def __init__(self, raster, height, zone_width, flip_y, width=48):
        super().__init__()
        self.register_buffer("raster", raster)
        self.h, self.zw, self.flip_y = height, zone_width, flip_y
        self.shifts = torch.arange(7, -1, -1, dtype=torch.uint8)
        xs = math.ceil(zone_width / 4)
        self.encoder = nn.Sequential(
            nn.Conv2d(16, width, 3, padding=1), nn.ReLU(),
            nn.Conv2d(width, width, 3, padding=1), nn.ReLU(),
            nn.Conv2d(width, width, 3, padding=1), nn.ReLU(),
            nn.Conv2d(width, 24, 1), nn.ReLU(),
        )
        bands = height // 4
        self.head = nn.Sequential(
            nn.Conv1d(2 * 24 * xs, 96, 3, padding=1), nn.ReLU(),
            nn.Conv1d(96, 96, 3, padding=1), nn.ReLU(),
            nn.Conv1d(96, 64, 3, padding=1, stride=2), nn.ReLU(),
            nn.Flatten(),
            nn.Linear(64 * ((bands + 1) // 2), 128), nn.ReLU(),
            nn.Linear(128, 1),
        )

    def cells(self, ids, flip):
        packed = self.raster[ids]  # (n, bytes)
        bits = (packed.unsqueeze(-1) >> self.shifts.to(packed.device)) & 1
        x = bits.flatten(1)[:, : self.h * self.zw].view(-1, 1, self.h, self.zw).float()
        if flip:  # vertical mirror of both armies: the same battle upside down
            x = x.flip(2)
        pad = (-self.zw) % 4
        x = nn.functional.pad(x, (0, pad))
        return nn.functional.pixel_unshuffle(x, 4)  # lossless: 4x4 blocks -> 16 channels

    def encode(self, ids, flip):
        f = self.encoder(self.cells(ids, flip))  # (n, 24, bands, xs)
        return f.permute(0, 1, 3, 2).flatten(1, 2)  # (n, 24*xs, bands)

    def forward(self, a, b, flip=False):
        ids, inv = torch.unique(torch.cat([a, b]), return_inverse=True)
        e = self.encode(ids, flip)
        ea, eb = e[inv[: len(a)]], e[inv[len(a):]]
        if self.flip_y:  # rotation symmetry: Blue's army is turned upside down
            ea_b, eb_b = ea.flip(2), eb.flip(2)
        else:
            ea_b, eb_b = ea, eb
        g_ab = self.head(torch.cat([ea, eb_b], 1)).squeeze(1)
        g_ba = self.head(torch.cat([eb, ea_b], 1)).squeeze(1)
        return torch.tanh(g_ab - g_ba)


def fit(model, train, test, epochs, lr, batch, device, augment=False, label=""):
    a, b, y = (torch.as_tensor(train[k], device=device) for k in ("red", "blue", "score"))
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=1e-4)
    steps = epochs * math.ceil(len(y) / batch)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, lr, total_steps=steps)
    t0 = time.time()
    for epoch in range(epochs):
        model.train()
        order = torch.randperm(len(y), device=device)
        total = 0.0
        for s in range(0, len(y), batch):
            i = order[s : s + batch]
            kw = {"flip": bool(torch.rand(()) < 0.5)} if augment else {}
            loss = ((model(a[i], b[i], **kw) - y[i].float()) ** 2).mean()
            opt.zero_grad()
            loss.backward()
            opt.step()
            sched.step()
            total += loss.item() * len(i)
        p = predict(model, test, batch * 4, device)
        print(f"  {label} epoch {epoch + 1}/{epochs}: train rmse {math.sqrt(total / len(y)):.3f}, "
              f"test rmse {np.sqrt(np.mean((test['score'] - p) ** 2)):.3f}  ({time.time() - t0:.0f}s)", flush=True)
    return predict(model, test, batch * 4, device)


@torch.no_grad()
def predict(model, data, batch, device):
    model.eval()
    a, b = (torch.as_tensor(data[k], device=device) for k in ("red", "blue"))
    out = [model(a[s : s + batch], b[s : s + batch]) for s in range(0, len(a), batch)]
    return torch.cat(out).cpu().numpy()


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prefix", default="data/matches/open")
    ap.add_argument("--epochs", type=int, default=4)
    ap.add_argument("--test", type=float, default=0.15, help="newest share of batches held out")
    ap.add_argument("--limit", type=int, default=0, help="use at most this many training battles")
    ap.add_argument("--out", default=None, help="results JSON (default PREFIX.study.json)")
    args = ap.parse_args()
    torch.manual_seed(0)
    device = "cuda" if torch.cuda.is_available() else "cpu"

    meta, raster, tokens, df = load(args.prefix)
    cut = df.batch.quantile(1 - args.test)
    train_df, test_df = df[df.batch <= cut], df[df.batch > cut].reset_index(drop=True)
    if args.limit:
        train_df = train_df.tail(args.limit)
    cand = candidate_groups(test_df)
    print(f"{len(df):,} battles, {len(raster):,} armies; train {len(train_df):,}, test {len(test_df):,} "
          f"({cand.group.nunique()} search generations, {len(cand):,} candidates)", flush=True)
    train = {k: train_df[k].to_numpy() for k in ("red", "blue", "score")}
    test = {k: test_df[k].to_numpy() for k in ("red", "blue", "score")}
    y = test["score"]
    results = {"battles": len(df), "train": len(train_df), "test": len(test_df), "armies": len(raster), "models": {}}

    def report(name, pred, cost=None):
        r = battle_metrics(y, pred) | group_metrics(test_df, cand, pred, tokens)
        if cost is not None:
            r["cost"] = cost
        results["models"][name] = r
        print(f"{name:>16}: rank {r['group_rank']:+.3f}  best kept {r['best_kept']:.2f}  siblings "
              f"{r['siblings']:.2f} ({r['sibling_pairs']} pairs)  corr {r['corr']:+.3f}  rmse {r['rmse']:.3f}"
              + (f"  cost {cost:.0%}" if cost is not None else ""), flush=True)

    rng = np.random.default_rng(0)
    report("random", rng.normal(size=len(y)))

    # Short battles: same simulator, stopped early.
    gens = test_df.generation.to_numpy()
    for g in TRACE:
        if g < meta["maxGenerations"]:
            report(f"battle to {g}", short_score(test_df, g), cost=float(np.minimum(gens, g).sum() / gens.sum()))

    # Bag of pieces.
    names = sorted({t.split("@")[0] for ts in tokens for t in ts})
    index = {n: i for i, n in enumerate(names)}
    counts = np.zeros((len(tokens), len(names) + 1), np.float32)
    for i, ts in enumerate(tokens):
        for t in ts:
            counts[i, index[t.split("@")[0]]] += 1
    counts[:, -1] = np.unpackbits(raster, axis=1).sum(1) / 100.0  # live cells
    pm = PieceModel(torch.as_tensor(counts, device=device)).to(device)
    report("pieces", fit(pm, train, test, epochs=max(args.epochs, 6), lr=0.02, batch=4096, device=device, label="pieces"))

    # CNN on the cells.
    cm = CellModel(torch.as_tensor(raster, device=device), meta["height"], meta["zoneWidth"],
                   flip_y=meta["symmetry"] == "rotation").to(device)
    print(f"  cnn: {sum(p.numel() for p in cm.parameters()):,} parameters", flush=True)
    report("cnn", fit(cm, train, test, epochs=args.epochs, lr=2e-3, batch=512, device=device, augment=True,
                      label="cnn"))
    torch.save(cm.state_dict(), args.prefix + ".cnn.pt")

    out = args.out or args.prefix + ".study.json"
    Path(out).write_text(json.dumps(results, indent=1))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
