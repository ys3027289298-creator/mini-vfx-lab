#!/usr/bin/env python3
"""Render bounce_report.json into a self-contained HTML/SVG report.

Run: python3 -I make_report.py   (reads bounce_report.json alongside it)
Charts: N(e) and T(e) coefficient curves, three-angle speed decay at the
chosen e*, and the e = 0.9 failure case. No external assets.
"""
import json
import math
import pathlib
import sys

sys.dont_write_bytecode = True

HERE = pathlib.Path(__file__).parent
R = json.loads(HERE.joinpath("bounce_report.json").read_text())

SERIES_COLORS = {
    "15": ("#2a78d6", "#3987e5"),
    "45": ("#eb6834", "#d95926"),
    "75": ("#1baf7a", "#199e70"),
}
CRITICAL = "#d03b3b"          # status color, mode-invariant per palette spec
ANGLES = ["15", "45", "75"]

W, H = 580, 330
ML, MR, MT, MB = 62, 90, 30, 52          # margins; wide right for end labels
PW, PH = W - ML - MR, H - MT - MB

CHART_DATA = {}


def sx(x, x0, x1):
    return ML + (x - x0) / (x1 - x0) * PW


def sy(y, y0, y1, log_y):
    if log_y:
        y = math.log10(max(y, 1e-12))
        y0, y1 = math.log10(y0), math.log10(y1)
    return MT + PH - (y - y0) / (y1 - y0) * PH


def log_ticks(y0, y1):
    ticks = []
    d0, d1 = math.ceil(math.log10(y0)), math.floor(math.log10(y1))
    for d in range(d0, d1 + 1):
        ticks.append(10.0 ** d)
    return ticks


def fmt(v, log_y):
    if log_y:
        return f"{v:g}"
    return f"{v:.2f}" if v < 10 else f"{v:.0f}"


def line_chart(cid, title, series, x_label, y_label, x_lim, y_lim,
               log_y=False, hlines=(), vlines=(), markers=()):
    """series: [(name, angle_key, [(x, y), ...], annotate_last)]."""
    x0, x1 = x_lim
    y0, y1 = y_lim
    parts = [f'<svg viewBox="0 0 {W} {H}" role="img" aria-label="{title}">',
             f'<clipPath id="clip-{cid}"><rect x="{ML}" y="{MT}" '
             f'width="{PW}" height="{PH}"/></clipPath>',
             f'<g clip-path="url(#clip-{cid})">']

    # horizontal grid + y ticks
    ticks = log_ticks(y0, y1) if log_y else [
        y0 + (y1 - y0) * i / 4 for i in range(5)]
    for tv in ticks:
        yy = sy(tv, y0, y1, log_y)
        parts.append(f'<line class="grid" x1="{ML}" y1="{yy:.1f}" '
                     f'x2="{ML + PW}" y2="{yy:.1f}"/>')
        parts.append(f'<text class="tick" x="{ML - 8}" y="{yy + 4:.1f}" '
                     f'text-anchor="end">{fmt(tv, log_y)}</text>')

    # x ticks
    nxt = 5
    for i in range(nxt + 1):
        xv = x0 + (x1 - x0) * i / nxt
        xx = sx(xv, x0, x1)
        parts.append(f'<line class="axisline" x1="{xx:.1f}" y1="{MT + PH}" '
                     f'x2="{xx:.1f}" y2="{MT + PH + 5}"/>')
        parts.append(f'<text class="tick" x="{xx:.1f}" y="{MT + PH + 20}" '
                     f'text-anchor="middle">{xv:.2f}</text>')

    # axes
    parts.append(f'<line class="axis" x1="{ML}" y1="{MT}" x2="{ML}" '
                 f'y2="{MT + PH}"/>')
    parts.append(f'<line class="axis" x1="{ML}" y1="{MT + PH}" '
                 f'x2="{ML + PW}" y2="{MT + PH}"/>')

    # reference lines (deadline / threshold / e*)
    for hl in hlines:
        yy = sy(hl["y"], y0, y1, log_y)
        parts.append(f'<line class="refline" x1="{ML}" y1="{yy:.1f}" '
                     f'x2="{ML + PW}" y2="{yy:.1f}"/>')
        parts.append(f'<text class="reflabel" x="{ML + PW - 4}" '
                     f'y="{yy - 6:.1f}" text-anchor="end">{hl["label"]}</text>')
    for vl in vlines:
        xx = sx(vl["x"], x0, x1)
        parts.append(f'<line class="vref" x1="{xx:.1f}" y1="{MT}" '
                     f'x2="{xx:.1f}" y2="{MT + PH}"/>')
        parts.append(f'<text class="reflabel" x="{xx + 5:.1f}" y="{MT + 12}">'
                     f'{vl["label"]}</text>')

    payload = {"x0": x0, "x1": x1, "log_y": log_y, "y0": y0, "y1": y1,
               "series": {}}
    # data lines (path coordinates clamped to the plot domain; raw values stay
    # in the tooltip payload, so post-sleep zero samples render on the bottom edge)
    for name, akey, pts, end_label in series:
        c_light, c_dark = SERIES_COLORS[akey]
        d = " ".join(
            f"{'M' if i == 0 else 'L'}{sx(x, x0, x1):.1f},"
            f"{sy(min(max(y, y0), y1), y0, y1, log_y):.1f}"
            for i, (x, y) in enumerate(pts))
        parts.append(f'<path class="series s{akey}" d="{d}"/>')
        payload["series"][name] = pts
        if end_label:
            # last in-range point for the direct label
            last = next(((x, y) for x, y in reversed(pts) if y <= y1), pts[-1])
            lx, ly = sx(last[0], x0, x1), sy(last[1], y0, y1, log_y)
            parts.append(f'<circle class="dot s{akey}" cx="{lx:.1f}" '
                         f'cy="{ly:.1f}" r="4"/>')
            parts.append(f'<text class="endlabel s{akey}-text" x="{lx + 8:.1f}" '
                         f'y="{ly + 4:.1f}">{name}</text>')

    for m in markers:
        xx = sx(m["x"], x0, x1)
        yy = sy(m["y"], y0, y1, log_y)
        parts.append(f'<circle class="marker s{m["angle"]}" cx="{xx:.1f}" '
                     f'cy="{yy:.1f}" r="4.5"/>')

    parts.append(f'<text class="title" x="{ML}" y="18">{title}</text>')
    parts.append(f'<text class="axislabel" x="{ML + PW / 2:.0f}" '
                 f'y="{H - 8}" text-anchor="middle">{x_label}</text>')
    parts.append(f'<text class="axislabel" transform="translate(16,{MT + PH / 2:.0f}) '
                 f'rotate(-90)" text-anchor="middle">{y_label}</text>')
    parts.append('<line class="crosshair" x1="0" y1="0" x2="0" y2="0"/>')
    parts.append("</g></svg>")
    CHART_DATA[cid] = payload
    return (f'<div class="panel" data-chart="{cid}">'
            + "".join(parts) + '<div class="tooltip"></div></div>')


# ------------------------------------------------------------------ data prep
e_pick = R["chosen_e"]
curves = R["curves"]
verif = {str(v["angle"]).rstrip("0").rstrip("."): v for v in R["verification"]}

# Panel A: bounce count vs e (log y). End labels omitted: the three series
# converge at the top-right and would collide; the shared legend carries identity.
series_n, markers_n = [], []
for a in ANGLES:
    rows = curves[a]
    pts = [(r["e"], max(1, r["bounces"])) for r in rows
           if math.isfinite(r["bounces"])]
    series_n.append((f"{a}°", a, pts, False))
    emax = R["feasible_e_per_angle"][a]
    cross = min(rows, key=lambda r: abs(r["e"] - emax))
    markers_n.append(dict(x=cross["e"], y=max(1, cross["bounces"]), angle=a))

# Panel B: settle time vs e (log y), with deadline and Zeno curves
series_t = []
for a in ANGLES:
    rows = curves[a]
    pts = [(r["e"], max(0.01, r["settle_time"])) for r in rows
           if math.isfinite(r["settle_time"])]
    series_t.append((f"{a}°", a, pts, False))

# Panel C: speed-ratio decay at e*, log y
series_c = []
for a in ANGLES:
    tr = verif[a]["trace"]
    series_c.append((f"{a}°", a, tr, False))

# Panel D: bad case e = 0.9 — simulate its full trace here
import importlib.util
spec = importlib.util.spec_from_file_location(
    "ba", HERE.joinpath("bounce_analysis.py"))
ba = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ba)
bad_impacts, bad_trace = ba.simulated_run(15.0, 0.9)
bc = R["bad_case"]

panels = [
    line_chart(
        "n_of_e", "弹跳次数 N(e) — 随恢复系数发散", series_n,
        "恢复系数 e", "第 n 次反弹后静止（次数，对数）",
        (0.0, 1.0), (1.0, 5000.0), log_y=True,
        vlines=[{"x": e_pick, "label": f"e* = {e_pick:.2f}"}],
        markers=markers_n),
    line_chart(
        "t_of_e", "静止时长 T(e) — 1 秒截止线反解上限", series_t,
        "恢复系数 e", "落地到静止的时间 s（对数）",
        (0.0, 1.0), (0.01, 200.0), log_y=True,
        hlines=[{"y": 1.0, "label": "截止 1.00 s"}],
        vlines=[{"x": e_pick, "label": f"e* = {e_pick:.2f}"}]),
    line_chart(
        "decay", f"e* = {e_pick:.2f}, μ = {R['params']['mu']}："
                 "三档入射角落地后的速度衰减", series_c,
        "首次落地后的时间 s", "速率 / 初速 V0（对数）",
        (0.0, 1.05), (0.005, 1.1), log_y=True,
        hlines=[{"y": 0.01, "label": "1% 静止阈值"}],
        vlines=[{"x": 1.0, "label": "1 s"}]),
    line_chart(
        "bad", f"反例 e = 0.9（15°）：第 {bc['bounces_until_sleep']} 次反弹"
               f"才停，T = {bc['settle_time']:.1f} s",
        [("15°, e=0.9", "15", bad_trace, False)],
        "首次落地后的时间 s", "速率 / 初速 V0（对数）",
        (0.0, 18.0), (0.005, 1.1), log_y=True,
        hlines=[{"y": 0.01, "label": "1% 静止阈值"}],
        vlines=[{"x": 1.0, "label": "1 s 时仍为 28%"}]),
]

# legend (three series, shared semantics)
legend = "".join(
    f'<span class="leg"><span class="sw s{a}"></span>{a}° 入射角</span>'
    for a in ANGLES)

# verification table
rows_html = []
for v in R["verification"]:
    rows_html.append(
        "<tr>" + "".join(f"<td>{c}</td>" for c in [
            f"{v['angle']:g}°", v["bounces"], f"{v['settle_time']:.4f}",
            f"{v['final_ratio']:.5f}",
            "✓" if v["meets_deadline"] else "✗",
            "✓" if v["meets_ratio"] else "✗",
            v["sim_impacts"], f"{v['sim_settle']:.4f}",
            f"{v['time_err']:.1e}", f"{v['speed_err']:.1e}"]) + "</tr>")

css = """
:root { color-scheme: light;
  --surface:#fcfcfb; --page:#f9f9f7; --ink:#0b0b0b; --ink2:#52514e;
  --muted:#898781; --grid:#e1e0d9; --axis:#c3c2b7; --border:rgba(11,11,11,.10);
  --s15:#2a78d6; --s45:#eb6834; --s75:#1baf7a; }
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) { color-scheme: dark;
  --surface:#1a1a19; --page:#0d0d0d; --ink:#f0efec; --ink2:#c3c2b7;
  --muted:#898781; --grid:#2c2c2a; --axis:#383835; --border:rgba(255,255,255,.10);
  --s15:#3987e5; --s45:#d95926; --s75:#199e70; } }
:root[data-theme="dark"] { color-scheme: dark;
  --surface:#1a1a19; --page:#0d0d0d; --ink:#f0efec; --ink2:#c3c2b7;
  --muted:#898781; --grid:#2c2c2a; --axis:#383835; --border:rgba(255,255,255,.10);
  --s15:#3987e5; --s45:#d95926; --s75:#199e70; }
* { box-sizing: border-box; }
body { margin:0; background:var(--page); color:var(--ink);
  font:14px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif; }
.wrap { max-width:1280px; margin:0 auto; padding:28px 24px 48px; }
header { display:flex; justify-content:space-between; align-items:baseline;
  gap:16px; flex-wrap:wrap; margin-bottom:6px; }
h1 { font-size:21px; margin:0; }
.sub { color:var(--ink2); margin:0 0 18px; }
button { font:inherit; padding:5px 12px; border:1px solid var(--border);
  background:var(--surface); color:var(--ink); border-radius:8px; cursor:pointer; }
.grid { display:grid; grid-template-columns:repeat(2,minmax(0,1fr)); gap:18px; }
@media (max-width:1080px){ .grid{grid-template-columns:1fr;} }
.panel { position:relative; background:var(--surface);
  border:1px solid var(--border); border-radius:12px; padding:10px 6px 4px; }
svg { width:100%; height:auto; display:block; }
.gridline, .grid { stroke:var(--grid); stroke-width:1; }
.axis { stroke:var(--axis); stroke-width:1.2; }
.axisline { stroke:var(--axis); stroke-width:1; }
.tick { fill:var(--muted); font-size:11px; }
.title { fill:var(--ink); font-size:13.5px; font-weight:600; }
.axislabel { fill:var(--ink2); font-size:12px; }
.series { fill:none; stroke-width:2; }
.s15 { stroke:var(--s15); } .s45 { stroke:var(--s45); } .s75 { stroke:var(--s75); }
.s15-text { fill:var(--s15); } .s45-text { fill:var(--s45); }
.s75-text { fill:var(--s75); }
.dot, .marker { fill:var(--surface); stroke-width:2.5; }
.dot.s15,.marker.s15 { stroke:var(--s15); }
.dot.s45,.marker.s45 { stroke:var(--s45); }
.dot.s75,.marker.s75 { stroke:var(--s75); }
.endlabel { font-size:12px; font-weight:600; }
.refline { stroke:""" + CRITICAL + """; stroke-width:1.4; stroke-dasharray:6 4; }
.vref { stroke:var(--muted); stroke-width:1.2; stroke-dasharray:4 4; }
.reflabel { fill:var(--ink2); font-size:11px; }
.crosshair { stroke:var(--muted); stroke-width:1; display:none;
  pointer-events:none; }
.tooltip { display:none; position:absolute; pointer-events:none; z-index:5;
  background:var(--surface); border:1px solid var(--border); border-radius:8px;
  padding:7px 10px; font-size:12px; box-shadow:0 4px 14px rgba(0,0,0,.14);
  min-width:130px; }
.tooltip .tt-title { font-weight:600; margin-bottom:3px; color:var(--ink); }
.tooltip .tt-row { display:flex; justify-content:space-between; gap:12px;
  color:var(--ink2); }
.tooltip .tt-row b { color:var(--ink); font-weight:600;
  font-variant-numeric:tabular-nums; }
.legend { display:flex; gap:18px; margin:14px 2px 4px; color:var(--ink2); }
.leg { display:inline-flex; align-items:center; gap:7px; font-size:13px; }
.sw { width:14px; height:3px; border-radius:2px; display:inline-block; }
table { border-collapse:collapse; width:100%; margin-top:10px; font-size:13px;
  background:var(--surface); border:1px solid var(--border);
  border-radius:12px; overflow:hidden; }
th, td { padding:8px 10px; text-align:right;
  font-variant-numeric:tabular-nums; }
th:first-child, td:first-child { text-align:left; }
thead th { color:var(--ink2); font-weight:600; border-bottom:1px solid var(--axis); }
tbody tr + tr td { border-top:1px solid var(--grid); }
h2 { font-size:16px; margin:26px 0 4px; }
.note { color:var(--ink2); font-size:13px; }
"""

js = """
const DATA = __DATA__;
const ML=%ML%, MR=%MR%, MT=%MT%, W=%W%, H=%H%;
document.getElementById('theme').addEventListener('click', () => {
  const r = document.documentElement;
  r.dataset.theme = r.dataset.theme === 'dark' ? 'light' : 'dark';
});
for (const panel of document.querySelectorAll('[data-chart]')) {
  const cid = panel.dataset.chart, d = DATA[cid];
  const svg = panel.querySelector('svg'), tip = panel.querySelector('.tooltip');
  const cross = svg.querySelector('.crosshair');
  const names = Object.keys(d.series);
  svg.addEventListener('mousemove', e => {
    const rect = svg.getBoundingClientRect();
    const vx = (e.clientX - rect.left) / rect.width * W;
    const x = d.x0 + (vx - ML) / (W - ML - MR) * (d.x1 - d.x0);
    if (x < d.x0 || x > d.x1) { tip.style.display = 'none'; cross.style.display='none'; return; }
    cross.setAttribute('x1', vx); cross.setAttribute('x2', vx);
    cross.setAttribute('y1', MT); cross.setAttribute('y2', H - %MB%);
    cross.style.display = 'block';
    let html = `<div class="tt-title">x = ${x.toFixed(3)}</div>`;
    for (const n of names) {
      const pts = d.series[n];
      let lo = 0, hi = pts.length - 1;
      while (lo < hi) { const m = (lo + hi) >> 1; pts[m][0] < x ? lo = m + 1 : hi = m; }
      const j = Math.min(lo, pts.length - 1);
      const y = d.log_y ? pts[j][1] : pts[j][1];
      html += `<div class="tt-row"><span>${n}</span><b>${y.toPrecision(3)}</b></div>`;
    }
    tip.innerHTML = html; tip.style.display = 'block';
    const pr = panel.getBoundingClientRect();
    let tx = e.clientX - pr.left + 14;
    if (tx + 160 > pr.width) tx -= 180;
    tip.style.left = tx + 'px';
    tip.style.top = (e.clientY - pr.top - 10) + 'px';
  });
  svg.addEventListener('mouseleave', () => {
    tip.style.display = 'none'; cross.style.display = 'none';
  });
}
""".replace("%ML%", str(ML)).replace("%MR%", str(MR)).replace(
    "%MT%", str(MT)).replace("%MB%", str(MB)).replace(
    "%W%", str(W)).replace("%H%", str(H)).replace(
    "__DATA__", json.dumps(CHART_DATA))

p = R["params"]
html = f"""<!doctype html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>弹跳级数与恢复系数选择</title>
<style>{css}</style>
</head>
<body>
<div class="wrap">
<header>
  <div><h1>连续反弹的闭式解、发散性与恢复系数选择</h1></div>
  <button id="theme">切换明暗主题</button>
</header>
<p class="sub">初速 V0 = {p['v0']:g} m/s，重力 g = {p['g']:g}，μ = {p['mu']}，
静止阈值 v_s = {p['v_sleep']:g} m/s（V0 的 1%），落地后 {p['deadline']:g} s 内必须静止。
角度从地面法线量起。</p>
<div class="grid">{''.join(panels)}</div>
<div class="legend">{legend}
  <span class="leg"><span class="sw" style="background:{CRITICAL}"></span>截止/阈值线</span>
</div>
<h2>三档入射角验证（闭式 vs 时间步进 dt = {p['dt']:g} s）</h2>
<table>
<thead><tr><th>入射角</th><th>闭式弹跳数</th><th>静止时间 s</th><th>末次撞击速率比</th>
<th>≤1 s</th><th>≤1%</th><th>仿真撞击数</th><th>仿真静止 s</th>
<th>时间误差</th><th>速率误差</th></tr></thead>
<tbody>{''.join(rows_html)}</tbody>
</table>
<p class="note">硬上限 e_max = {R['e_ceiling']:.4f}（15° 档最严，Zeno 充分界
{R['bounds']['sufficient_e_steep']:.4f}），工程取 e* = {e_pick:.2f} 留出余量。
e = 0.9 时 15° 档在 1 s 处速率仍为初速的 {bc['ratio_at_deadline']:.1%}，
第 {bc['bounces_until_sleep']} 次反弹后才静止（T = {bc['settle_time']:.2f} s）。</p>
</div>
<script>{js}</script>
</body>
</html>"""

HERE.joinpath("bounce_report.html").write_text(html)
print("wrote bounce_report.html")
