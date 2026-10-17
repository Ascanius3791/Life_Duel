#!/usr/bin/env python3
"""Training report: one self-contained HTML page with the curves of life_arena runs.

  tools/training_report.py [--out FILE] [--log NAME=LOG[,LOG2...]]... [LEAGUE...]

--log    a captured `life_arena train` log (stdout); several logs of resumed runs of
         the same league are joined with commas. Gives the search gain per iteration.
LEAGUE   a league file (FILE or FILE.json). Gives the Nash weights per army and, for
         twin leagues (FILE.twins/eval.csv next to it), the cross-evaluation curve.
"""
import argparse, csv, html, json, os, re, sys, time

ITER = re.compile(r"iter\s+(\d+)\s+league\s+(\d+)\s+gain\s+([+-]?[\d.]+)")


def read_log(paths):
    """[(iteration, league size, gain)], iterations of resumed logs continued."""
    rows, offset = [], 0
    for path in paths:
        text = open(path, errors="replace").read().replace("\r", "\n")
        last = 0
        for m in ITER.finditer(text):
            it = int(m.group(1))
            if rows and it + offset <= rows[-1][0]:
                continue  # progress lines repeat; keep the first summary per iteration
            rows.append((it + offset, int(m.group(2)), float(m.group(3))))
            last = it
        offset += last
    return rows


def read_league(path):
    if not path.endswith(".json"):
        path += ".json"
    data = json.load(open(path))
    name = os.path.basename(path)[: -len(".txt.json")] if path.endswith(".txt.json") else os.path.basename(path)
    s = data["settings"]
    league = {
        "name": name,
        "settings": f'{s["preset"]}, {s["width"]}x{s["height"]}, budget {s["budget"]}, limit {s["maxGenerations"]}',
        "weights": [[a["id"], round(float(a.get("weight", 0)), 4), a.get("origin", "")] for a in data["armies"]],
        "training": data.get("training", {}).get("stopReason"),
    }
    twins = path[: -len(".json")] + ".twins/eval.csv"
    if os.path.exists(twins):
        with open(twins) as f:
            league["eval"] = [{k: (float(v) if v not in ("", None) else None) for k, v in r.items()} for r in csv.DictReader(f)]
    return league


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="build/training_report.html")
    ap.add_argument("--log", action="append", default=[], metavar="NAME=LOG[,LOG...]")
    ap.add_argument("leagues", nargs="*")
    a = ap.parse_args()
    runs = []
    for spec in a.log:
        name, _, paths = spec.partition("=")
        rows = read_log([p for p in paths.split(",") if p])
        if rows:
            runs.append({"name": name, "rows": rows})
    leagues = [read_league(p) for p in a.leagues]
    data = {"runs": runs, "leagues": leagues, "generated": time.strftime("%Y-%m-%d %H:%M")}
    page = TEMPLATE.replace("/*DATA*/null", json.dumps(data))
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    open(a.out, "w").write(page)
    print(f"wrote {a.out}: {len(runs)} logs, {len(leagues)} leagues")


TEMPLATE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Training report</title>
<style>
:root { --surface:#fcfcfb; --panel:#f3f3f1; --text:#0b0b0b; --text2:#52514e; --muted:#8a8984; --grid:#e4e3df;
  --s1:#2a78d6; --s2:#eb6834; --ref:#8a8984; }
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) { --surface:#1a1a19; --panel:#232322; --text:#f0efec;
  --text2:#c3c2b7; --muted:#8f8e88; --grid:#33332f; --s1:#3987e5; --s2:#d95926; --ref:#8f8e88; } }
:root[data-theme="dark"] { --surface:#1a1a19; --panel:#232322; --text:#f0efec; --text2:#c3c2b7; --muted:#8f8e88; --grid:#33332f;
  --s1:#3987e5; --s2:#d95926; --ref:#8f8e88; }
* { box-sizing:border-box; }
body { margin:0; background:var(--surface); color:var(--text); font:14px/1.45 system-ui,sans-serif; }
main { max-width:1100px; margin:0 auto; padding:24px 16px 48px; }
h1 { font-size:22px; margin:0 0 4px; } h2 { font-size:16px; margin:32px 0 4px; }
p.note { color:var(--text2); margin:0 0 12px; max-width:760px; }
.tiles { display:grid; grid-template-columns:repeat(auto-fit,minmax(180px,1fr)); gap:12px; margin:16px 0; }
.tile { background:var(--panel); border-radius:10px; padding:12px 14px; }
.tile .k { color:var(--text2); font-size:12px; } .tile .v { font-size:24px; font-weight:600; font-variant-numeric:tabular-nums; }
.tile .s { color:var(--muted); font-size:12px; }
.grid2 { display:grid; grid-template-columns:repeat(auto-fill,minmax(min(100%,480px),1fr)); gap:16px; }
.card { background:var(--panel); border-radius:10px; padding:12px 12px 6px; position:relative; }
.card h3 { font-size:13px; margin:0 0 2px; font-weight:600; } .card .sub { color:var(--muted); font-size:12px; margin:0 0 4px; }
svg { display:block; width:100%; height:auto; overflow:visible; }
svg text { fill:var(--text2); font-size:11px; font-variant-numeric:tabular-nums; }
.legend { display:flex; gap:14px; font-size:12px; color:var(--text2); margin:2px 0 6px; flex-wrap:wrap; }
.legend i { display:inline-block; width:14px; height:3px; border-radius:2px; vertical-align:middle; margin-right:5px; }
.tip { position:absolute; pointer-events:none; background:var(--surface); color:var(--text); border:1px solid var(--grid);
  border-radius:8px; padding:6px 8px; font-size:12px; box-shadow:0 4px 14px rgba(0,0,0,.15); display:none; white-space:nowrap; z-index:2; }
.empty { color:var(--muted); padding:28px 8px; text-align:center; }
details { margin-top:8px; color:var(--text2); } table { border-collapse:collapse; font-size:12px; font-variant-numeric:tabular-nums; }
td, th { padding:2px 10px 2px 0; text-align:right; }
</style></head><body><main>
<h1>Bot training</h1>
<p class="note" id="gen"></p>
<div class="tiles" id="tiles"></div>

<h2>Twin leagues: can one league beat the other?</h2>
<p class="note"><b>x</b> = the best result any army of the other league gets against this league's Nash mixture (+1: always wins).
Falling toward 0 means the bot holds up against strategies it never trained against. Training stops at x &lt; 0.05 (converged)
or when the average of the last 3 checks stops improving (plateau).</p>
<div class="grid2" id="twins"></div>

<h2>Search gain per round</h2>
<p class="note">How strongly the round's search beat the bot's current mix with a new army. A rough measure of how exploitable the
bot is by an opponent who knows it exactly. Dots: each round; line: average of the last 10 rounds.</p>
<div class="grid2" id="gains"></div>

<h2>Which armies the bot plays</h2>
<p class="note">Nash weight per army, in the order the armies were added. Weight on late armies means later training produced
better armies; weight spread over many means the game rewards mixing.</p>
<div class="grid2" id="weights"></div>
</main>
<script>
const D = /*DATA*/null;
const $ = s => document.querySelector(s);
const css = v => getComputedStyle(document.documentElement).getPropertyValue(v).trim();
const f2 = v => (v >= 0 ? '+' : '') + v.toFixed(2);
$('#gen').textContent = `Generated ${D.generated}. Re-run tools/training_report.py to refresh.`;

// Tiles: one per league.
for (const L of D.leagues) {
  const used = L.weights.filter(w => w[1] > 0.001).length;
  const run = D.runs.find(r => L.name.startsWith(r.name) || r.name === L.name);
  const t = document.createElement('div'); t.className = 'tile';
  t.innerHTML = `<div class="k">${L.name}</div><div class="v">${L.weights.length} armies</div>` +
    `<div class="s">${used} in the mix · ${L.settings}${L.training ? ' · stopped: ' + L.training.split(':')[0] : ''}</div>`;
  $('#tiles').append(t);
}

// A line chart with a crosshair tooltip. series: [{name, color, pts:[[x,y]], dots?}]
function lineChart(host, series, o) {
  const W = 520, H = 230, P = { l: 40, r: 14, t: 10, b: 26 };
  const all = series.flatMap(s => s.pts);
  const xmax = Math.max(...all.map(p => p[0]), 1), xmin = Math.min(...all.map(p => p[0]), 0);
  const ymin = o.ymin, ymax = o.ymax;
  const X = x => P.l + (x - xmin) / (xmax - xmin || 1) * (W - P.l - P.r);
  const Y = y => P.t + (ymax - Math.max(ymin, Math.min(ymax, y))) / (ymax - ymin) * (H - P.t - P.b);
  let s = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${o.label}">`;
  for (const v of o.yticks) s += `<line x1="${P.l}" x2="${W - P.r}" y1="${Y(v)}" y2="${Y(v)}" stroke="${css('--grid')}"/>` +
    `<text x="${P.l - 6}" y="${Y(v) + 4}" text-anchor="end">${o.yfmt(v)}</text>`;
  if (o.ref != null) s += `<line x1="${P.l}" x2="${W - P.r}" y1="${Y(o.ref)}" y2="${Y(o.ref)}" stroke="${css('--ref')}" stroke-dasharray="4 4"/>` +
    `<text x="${W - P.r}" y="${Y(o.ref) - 4}" text-anchor="end">${o.refLabel}</text>`;
  s += `<text x="${P.l}" y="${H - 6}">${xmin}</text><text x="${W - P.r}" y="${H - 6}" text-anchor="end">${o.xlabel} ${xmax}</text>`;
  for (const se of series) {
    if (se.dots) for (const p of se.pts) s += `<circle cx="${X(p[0])}" cy="${Y(p[1])}" r="2.2" fill="${se.color}" opacity=".35"/>`;
    else s += `<path d="${se.pts.map((p, i) => (i ? 'L' : 'M') + X(p[0]).toFixed(1) + ' ' + Y(p[1]).toFixed(1)).join('')}" fill="none" stroke="${se.color}" stroke-width="2" stroke-linejoin="round"/>`;
  }
  s += `<line class="xh" y1="${P.t}" y2="${H - P.b}" stroke="${css('--muted')}" opacity="0"/></svg><div class="tip"></div>`;
  host.insertAdjacentHTML('beforeend', s);
  const svg = host.querySelector('svg:last-of-type'), tip = host.querySelector('.tip:last-of-type'), xh = svg.querySelector('.xh');
  svg.addEventListener('mousemove', e => {
    const r = svg.getBoundingClientRect(), x = (e.clientX - r.left) / r.width * W;
    const xv = xmin + (x - P.l) / (W - P.l - P.r) * (xmax - xmin);
    const near = se => se.pts.reduce((b, p) => Math.abs(p[0] - xv) < Math.abs(b[0] - xv) ? p : b, se.pts[0]);
    const hit = series.map(se => [se, near(se)]);
    const gx = hit[0][1][0];
    xh.setAttribute('x1', X(gx)); xh.setAttribute('x2', X(gx)); xh.setAttribute('opacity', 1);
    tip.innerHTML = `<b>${o.xlabel} ${gx}</b><br>` + hit.map(([se, p]) => `<i style="display:inline-block;width:10px;height:3px;background:${se.color};vertical-align:middle;margin-right:5px"></i>${se.name}: ${o.yfmt(p[1])}`).join('<br>');
    tip.style.display = 'block';
    const hr = host.getBoundingClientRect();
    tip.style.left = Math.min(e.clientX - hr.left + 12, hr.width - tip.offsetWidth - 4) + 'px';
    tip.style.top = (e.clientY - hr.top + 12) + 'px';
  });
  svg.addEventListener('mouseleave', () => { tip.style.display = 'none'; xh.setAttribute('opacity', 0); });
}

function card(parent, title, sub) {
  const c = document.createElement('div'); c.className = 'card';
  c.innerHTML = `<h3>${title}</h3><p class="sub">${sub}</p>`; parent.append(c); return c;
}

// Twin cross-evaluation.
const twins = D.leagues.filter(L => L.eval && L.eval.length);
if (!twins.length) $('#twins').innerHTML = '<div class="card empty">No twin run has reached its first check yet (one check every 5 rounds).</div>';
for (const L of twins) {
  const c = card($('#twins'), L.name, `${L.eval.length} checks, round ${L.eval[L.eval.length - 1].iteration}` + (L.training ? ` · ${L.training}` : ' · still training'));
  c.insertAdjacentHTML('beforeend', `<div class="legend"><span><i style="background:${css('--s1')}"></i>x A (B's armies vs A's mix)</span><span><i style="background:${css('--s2')}"></i>x B (A's armies vs B's mix)</span></div>`);
  lineChart(c, [
    { name: 'x A', color: css('--s1'), pts: L.eval.map(r => [r.iteration, r.xA]) },
    { name: 'x B', color: css('--s2'), pts: L.eval.map(r => [r.iteration, r.xB]) },
  ], { ymin: -1, ymax: 1, yticks: [-1, -0.5, 0, 0.5, 1], yfmt: f2, ref: 0.05, refLabel: 'converged below 0.05', xlabel: 'round', label: 'Twin cross-evaluation' });
  c.insertAdjacentHTML('beforeend', `<details><summary>Table</summary><table><tr><th>round</th><th>x A</th><th>x B</th><th>gain A</th><th>gain B</th><th>armies</th><th>in mix</th></tr>` +
    L.eval.map(r => `<tr><td>${r.iteration}</td><td>${f2(r.xA)}</td><td>${f2(r.xB)}</td><td>${f2(r.gainA)}</td><td>${f2(r.gainB)}</td><td>${r.leagueA + r.leagueB}</td><td>${r.supportUnion}</td></tr>`).join('') + `</table></details>`);
}

// Gain per round, one small multiple per run.
for (const R of D.runs) {
  const pts = R.rows.map(r => [r[0], r[2]]);
  const avg = pts.map((p, i) => { const w = pts.slice(Math.max(0, i - 9), i + 1); return [p[0], w.reduce((a, q) => a + q[1], 0) / w.length]; });
  const last = avg[avg.length - 1][1];
  const c = card($('#gains'), R.name, `${pts.length} rounds · last 10 average ${f2(last)}`);
  lineChart(c, [{ name: 'gain', color: css('--s1'), pts, dots: true }, { name: '10-round average', color: css('--s1'), pts: avg }],
    { ymin: 0, ymax: 1, yticks: [0, 0.25, 0.5, 0.75, 1], yfmt: f2, xlabel: 'round', label: `Search gain of ${R.name}` });
}
if (!D.runs.length) $('#gains').innerHTML = '<div class="card empty">No logs given.</div>';

// Nash weights per army.
for (const L of D.leagues) {
  const c = card($('#weights'), L.name, `${L.weights.filter(w => w[1] > 0.001).length} of ${L.weights.length} armies have weight`);
  const W = 520, H = 160, P = { l: 40, r: 8, t: 8, b: 22 }, n = L.weights.length;
  const ymax = Math.max(0.05, ...L.weights.map(w => w[1]));
  const bw = (W - P.l - P.r) / n, Y = v => P.t + (1 - v / ymax) * (H - P.t - P.b);
  let s = `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="Nash weights of ${L.name}">`;
  for (const v of [0, ymax / 2, ymax]) s += `<line x1="${P.l}" x2="${W - P.r}" y1="${Y(v)}" y2="${Y(v)}" stroke="${css('--grid')}"/><text x="${P.l - 6}" y="${Y(v) + 4}" text-anchor="end">${Math.round(v * 100)}%</text>`;
  L.weights.forEach((w, i) => {
    if (w[1] <= 0) return;
    const x = P.l + i * bw + Math.min(1, bw * 0.15), h = Math.max(1, Y(0) - Y(w[1]));
    s += `<rect x="${x.toFixed(1)}" y="${(Y(0) - h).toFixed(1)}" width="${Math.max(1, bw - Math.min(2, bw * 0.3)).toFixed(1)}" height="${h.toFixed(1)}" rx="1.5" fill="${css('--s1')}"><title>army #${w[0]} (${w[2]}): ${(w[1] * 100).toFixed(1)}%</title></rect>`;
  });
  s += `<text x="${P.l}" y="${H - 6}">first army</text><text x="${W - P.r}" y="${H - 6}" text-anchor="end">latest army #${L.weights[n - 1][0]}</text></svg>`;
  c.insertAdjacentHTML('beforeend', s);
}
</script></body></html>
"""

if __name__ == "__main__":
    main()
