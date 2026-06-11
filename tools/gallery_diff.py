#!/usr/bin/env python3
"""Visual review of gallery changes: ranked overview, side-by-side, swipe,
blink, glow, and a measuring heatmap.

The image equivalent of `git diff <ref> -- gallery/`: every changed gallery
PNG becomes a before/after pair in one self-contained HTML page (the PNGs are
base64-embedded, so the file works from anywhere, needs no server, and the
data: URIs keep the canvas untainted for the pixel-stats/heatmap mode).

  python3 tools/gallery_diff.py              # vs github/main (the push gap)
  python3 tools/gallery_diff.py HEAD~5       # vs any ref
  python3 tools/gallery_diff.py --no-open    # just print the output path

Keys in the page: `o` for the ranked overview, arrows or j/k switch scenes,
1/2/3/4/5 switch modes (side-by-side / swipe / blink / glow / heatmap),
space toggles blink by hand, [ and ] adjust glow/heatmap gain.

Scenes are ranked worst-first by mean absolute channel delta, idiff-style.
The glow mode is lifted from Mike's idiff (github.com/mtklein/idiff): two
stacked images under mix-blend-mode:difference and a grayscale+brightness
filter -- the browser computes the amplified diff, no JS, no decode.
"""

import argparse
import base64
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args], capture_output=True)


def png_b64(data):
    return "data:image/png;base64," + base64.b64encode(data).decode("ascii")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ref", nargs="?", default="github/main",
                    help="compare the worktree's gallery against this ref")
    ap.add_argument("--no-open", action="store_true",
                    help="don't `open` the page, just print its path")
    opts = ap.parse_args()

    # Changed = differs from the ref, including scenes the ref doesn't have
    # (new) and scenes the worktree dropped (gone).  Worktree bytes are read
    # from disk; ref bytes via `git show` so this works for any commit.
    names = git("diff", "--name-only", opts.ref, "--", "gallery/*.png")
    if names.returncode != 0:
        sys.exit(f"git diff failed: {names.stderr.decode().strip()}")
    changed = [n for n in names.stdout.decode().splitlines() if n]
    if not changed:
        print(f"gallery/ is byte-identical to {opts.ref} -- nothing to review")
        return

    scenes = []
    for path in sorted(changed):
        name = os.path.splitext(os.path.basename(path))[0]
        ref = git("show", f"{opts.ref}:{path}")
        before = png_b64(ref.stdout) if ref.returncode == 0 else None
        try:
            with open(os.path.join(ROOT, path), "rb") as f:
                after = png_b64(f.read())
        except FileNotFoundError:
            after = None
        scenes.append({"name": name, "before": before, "after": after})

    payload = json.dumps(scenes)
    html = TEMPLATE.replace("__SCENES__", payload).replace("__REF__", opts.ref)
    out = os.path.join(tempfile.gettempdir(), "gallery_diff.html")
    with open(out, "w") as f:
        f.write(html)
    print(f"{len(scenes)} changed scene(s) vs {opts.ref}: {out}")
    if not opts.no_open:
        subprocess.run(["open", out])


# One page, no dependencies.  The overview ranks every changed scene
# worst-first (idiff's mean-abs-delta score, computed client-side) with a
# glow thumbnail per row; the per-scene modes are side-by-side | swipe
# (pointer-driven divider) | blink (2 Hz, space to step by hand) | glow
# (idiff's stacked difference + grayscale/brightness, all CSS) | heatmap
# (per-pixel max channel delta with exact stats and adjustable gain).
TEMPLATE = r"""<!doctype html>
<meta charset="utf-8">
<title>gallery diff vs __REF__</title>
<style>
  body { margin:0; background:#15171c; color:#cdd3df; font:13px/1.5 -apple-system, sans-serif;
         display:grid; grid-template-columns: 250px 1fr; height:100vh; }
  #nav { overflow-y:auto; border-right:1px solid #2a2e38; padding:8px; }
  #nav h1 { font-size:13px; margin:4px 6px 10px; color:#8b93a7; font-weight:600; }
  #nav button { display:block; width:100%; text-align:left; padding:6px 8px; margin:2px 0;
                background:none; border:0; border-radius:6px; color:inherit; font:inherit; cursor:pointer;
                font-variant-numeric:tabular-nums; }
  #nav button.sel { background:#2b3242; color:#fff; }
  #nav button .score { float:right; color:#8b93a7; }
  #main { display:flex; flex-direction:column; overflow:hidden; }
  #bar { display:flex; gap:8px; align-items:center; padding:8px 12px; border-bottom:1px solid #2a2e38; }
  #bar .mode { padding:4px 10px; border-radius:6px; border:1px solid #3a4152; background:none;
               color:inherit; font:inherit; cursor:pointer; }
  #bar .mode.sel { background:#3d77d8; border-color:#3d77d8; color:#fff; }
  #stats { margin-left:auto; color:#8b93a7; font-variant-numeric:tabular-nums; }
  #view { flex:1; display:flex; align-items:flex-start; justify-content:center; overflow:auto; padding:16px; }
  .pair { display:flex; gap:12px; }
  .pair figure { margin:0; text-align:center; color:#8b93a7; }
  img, canvas { image-rendering:pixelated; background:
       repeating-conic-gradient(#23262e 0 25%, #1b1e25 0 50%) 0 0/16px 16px; border:1px solid #2a2e38; }
  .glow { position:relative; filter: grayscale(1) brightness(var(--gain, 64)); background:#000; }
  .glow img { display:block; border:0; background:none; }
  .glow img + img { position:absolute; inset:0; mix-blend-mode:difference; }
  #overlay { position:relative; cursor:ew-resize; }
  #overlay img { display:block; }
  #overlay .top { position:absolute; inset:0; }
  #overlay .top img { position:absolute; inset:0; }
  #divider { position:absolute; top:0; bottom:0; width:1px; background:#3d77d8; pointer-events:none; }
  .badge { color:#e8b34b; }
  table#over { border-collapse:collapse; }
  table#over td { padding:6px 10px; vertical-align:middle; border-bottom:1px solid #2a2e38; }
  table#over tr { cursor:pointer; }
  table#over tr:hover td { background:#1d212b; }
  table#over img, table#over .glow { max-height:96px; }
  table#over .glow img { max-height:96px; }
  table#over .num { color:#8b93a7; font-variant-numeric:tabular-nums; text-align:right; }
</style>
<div id="nav"><h1>vs __REF__ <span class="score">(o: overview)</span></h1></div>
<div id="main">
  <div id="bar">
    <button class="mode" data-m="side">1 side-by-side</button>
    <button class="mode" data-m="swipe">2 swipe</button>
    <button class="mode" data-m="blink">3 blink</button>
    <button class="mode" data-m="glow">4 glow</button>
    <button class="mode" data-m="heat">5 heatmap</button>
    <span id="stats"></span>
  </div>
  <div id="view"></div>
</div>
<script>
const scenes = __SCENES__;
let cur = -1, mode = "side", gain = 64, blinkShow = 0, blinkTimer = null;
const nav = document.getElementById("nav"), view = document.getElementById("view"),
      stats = document.getElementById("stats");

function img(src) { const e = new Image(); e.src = src; return e; }

function glowEl(s) {  // idiff's trick: the browser computes the amplified diff
  const g = document.createElement("div"); g.className = "glow";
  g.style.setProperty("--gain", gain);
  g.append(img(s.before), img(s.after));
  return g;
}

// Decode once per scene; data: URIs keep the canvas untainted.
function pixels(s, cb) {
  if (s.px) return cb(s.px);
  const a = new Image(), b = new Image(); let n = 0;
  const done = () => { if (++n < 2) return;
    const w = b.naturalWidth, h = b.naturalHeight;
    const cv = new OffscreenCanvas(w, h), cx = cv.getContext("2d", {willReadFrequently:true});
    cx.drawImage(a, 0, 0); const pa = cx.getImageData(0, 0, w, h).data;
    cx.clearRect(0, 0, w, h);
    cx.drawImage(b, 0, 0); const pb = cx.getImageData(0, 0, w, h).data;
    s.px = {w, h, pa, pb}; cb(s.px);
  };
  a.onload = done; b.onload = done; a.src = s.before; b.src = s.after;
}

// idiff's ranking: mean absolute channel delta, worst first.  Scored lazily
// at load; the nav and overview re-sort as results land.
function score(s, cb) {
  if (s.score !== undefined) return cb && cb();
  if (!s.before || !s.after) { s.score = 1; s.count = NaN; s.max = NaN; return cb && cb(); }
  pixels(s, ({w, h, pa, pb}) => {
    let sum = 0, count = 0, max = 0;
    for (let i = 0; i < pa.length; i += 4) {
      const d = Math.max(Math.abs(pa[i]-pb[i]), Math.abs(pa[i+1]-pb[i+1]),
                         Math.abs(pa[i+2]-pb[i+2]), Math.abs(pa[i+3]-pb[i+3]));
      sum += Math.abs(pa[i]-pb[i]) + Math.abs(pa[i+1]-pb[i+1])
           + Math.abs(pa[i+2]-pb[i+2]) + Math.abs(pa[i+3]-pb[i+3]);
      if (d) { count++; if (d > max) max = d; }
    }
    s.score = sum / (pa.length * 255); s.count = count; s.max = max;
    cb && cb();
  });
}

function rebuildNav() {
  const order = scenes.map((s, i) => i).sort((x, y) => (scenes[y].score ?? 0) - (scenes[x].score ?? 0));
  nav.querySelectorAll("button").forEach(b => b.remove());
  for (const i of order) {
    const s = scenes[i], b = document.createElement("button");
    b.innerHTML = s.name + (s.before ? (s.after ? "" : " <span class=badge>(gone)</span>")
                                     : " <span class=badge>(new)</span>")
      + (s.score !== undefined && s.before && s.after
         ? ` <span class=score>${(s.score * 100).toFixed(3)}%</span>` : "");
    b.onclick = () => { cur = i; render(); };
    b.classList.toggle("sel", i === cur);
    nav.appendChild(b); s.btn = b;
  }
}

function overview() {
  view.innerHTML = ""; stats.textContent = "ranked worst-first, idiff-style; click a row";
  document.querySelectorAll(".mode").forEach(b => b.classList.remove("sel"));
  const order = scenes.map((s, i) => i).sort((x, y) => (scenes[y].score ?? 0) - (scenes[x].score ?? 0));
  const t = document.createElement("table"); t.id = "over";
  for (const i of order) {
    const s = scenes[i], tr = document.createElement("tr");
    const name = document.createElement("td");
    name.innerHTML = `<b>${s.name}</b>` + (s.before && s.after ? "" : ' <span class=badge>' +
                     (s.after ? "(new)" : "(gone)") + "</span>");
    const num = document.createElement("td"); num.className = "num";
    num.textContent = s.score !== undefined && s.count >= 0
      ? `${(s.score * 100).toFixed(3)}%  ·  ${s.count.toLocaleString()} px  ·  max ${s.max}` : "";
    const gl = document.createElement("td");
    if (s.before && s.after) gl.append(glowEl(s));
    const ba = document.createElement("td");
    const small = src => { const e = img(src); return e; };
    if (s.before) ba.append(small(s.before));
    if (s.after) ba.append(small(s.after));
    tr.append(name, num, gl, ba);
    tr.onclick = () => { cur = i; mode = "swipe"; render(); };
    t.append(tr);
  }
  view.append(t);
}

function render() {
  if (cur < 0) return overview();
  const s = scenes[cur];
  scenes.forEach(x => x.btn && x.btn.classList.toggle("sel", x === s));
  document.querySelectorAll(".mode").forEach(b => b.classList.toggle("sel", b.dataset.m === mode));
  clearInterval(blinkTimer); blinkTimer = null;
  view.innerHTML = ""; stats.textContent = "";
  if (!s.before || !s.after) {  // new or deleted scene: nothing to compare
    const f = document.createElement("figure");
    f.append(img(s.after || s.before));
    f.insertAdjacentHTML("beforeend",
      `<figcaption class="badge">${s.after ? "new scene (not in __REF__)" : "deleted scene"}</figcaption>`);
    view.append(f); return;
  }
  if (mode === "side") {
    const p = document.createElement("div"); p.className = "pair";
    for (const [src, cap] of [[s.before, "__REF__"], [s.after, "worktree"]]) {
      const f = document.createElement("figure");
      f.append(img(src)); f.insertAdjacentHTML("beforeend", `<figcaption>${cap}</figcaption>`);
      p.append(f);
    }
    view.append(p);
  } else if (mode === "swipe" || mode === "blink") {
    const o = document.createElement("div"); o.id = "overlay";
    const base = img(s.before), top = document.createElement("div"); top.className = "top";
    const ti = img(s.after); top.append(ti);
    const d = document.createElement("div"); d.id = "divider";
    o.append(base, top, d); view.append(o);
    if (mode === "swipe") {
      const set = frac => { const w = base.clientWidth * frac;
        ti.style.clipPath = `inset(0 ${base.clientWidth - w}px 0 0)`; d.style.left = w + "px"; };
      base.onload = () => set(0.5); if (base.complete) set(0.5);
      o.onmousemove = e => set(Math.min(1, Math.max(0, (e.offsetX) / base.clientWidth)));
    } else {
      d.remove();
      const tick = () => { blinkShow ^= 1; top.style.visibility = blinkShow ? "visible" : "hidden";
        stats.textContent = blinkShow ? "worktree" : "__REF__"; };
      tick(); blinkTimer = setInterval(tick, 500);
    }
  } else if (mode === "glow") {
    const f = document.createElement("figure"); f.append(glowEl(s));
    f.insertAdjacentHTML("beforeend",
      `<figcaption>difference × brightness(${gain}) — all CSS, per idiff ([ and ] adjust)</figcaption>`);
    view.append(f);
    score(s, () => { stats.textContent = statline(s); });
  } else {  // heatmap: exact, JS-computed, adjustable gain
    pixels(s, ({w, h, pa, pb}) => {
      const cv = document.createElement("canvas"); cv.width = w; cv.height = h;
      const cx = cv.getContext("2d"), out = cx.createImageData(w, h);
      for (let i = 0; i < pa.length; i += 4) {
        const d = Math.max(Math.abs(pa[i]-pb[i]), Math.abs(pa[i+1]-pb[i+1]),
                           Math.abs(pa[i+2]-pb[i+2]), Math.abs(pa[i+3]-pb[i+3]));
        const v = Math.min(255, d * gain);
        out.data[i] = v; out.data[i+1] = d ? 48 : 0; out.data[i+2] = 0; out.data[i+3] = 255;
      }
      cx.putImageData(out, 0, 0);
      const f = document.createElement("figure"); f.append(cv);
      f.insertAdjacentHTML("beforeend", `<figcaption>delta ×${gain} ([ and ] adjust)</figcaption>`);
      view.append(f);
      score(s, () => { stats.textContent = statline(s); });
    });
  }
}

function statline(s) {
  const {w, h} = s.px;
  return `${s.count.toLocaleString()} px changed (${(100*s.count/(w*h)).toFixed(2)}%), `
       + `max delta ${s.max}/255, mean ${(s.score*100).toFixed(4)}%`;
}

document.querySelectorAll(".mode").forEach(b => b.onclick = () => {
  mode = b.dataset.m; if (cur < 0) cur = 0; render(); });
addEventListener("keydown", e => {
  if (e.key === "o" || e.key === "0") { cur = -1; render(); }
  else if (e.key === "ArrowRight" || e.key === "j") { cur = (cur + 1 + scenes.length) % scenes.length; render(); }
  else if (e.key === "ArrowLeft" || e.key === "k") { cur = (cur - 1 + scenes.length) % scenes.length; render(); }
  else if (e.key >= "1" && e.key <= "5") { mode = ["side","swipe","blink","glow","heat"][e.key - 1];
    if (cur < 0) cur = 0; render(); }
  else if (e.key === " ") { e.preventDefault(); if (cur < 0) return;
    if (mode !== "blink") { mode = "blink"; render(); }
    else { clearInterval(blinkTimer); blinkTimer = null;
           const top = document.querySelector("#overlay .top"); blinkShow ^= 1;
           top.style.visibility = blinkShow ? "visible" : "hidden";
           stats.textContent = blinkShow ? "worktree" : "__REF__"; } }
  else if (e.key === "]") { gain = Math.min(1024, gain * 2); if (mode === "heat" || mode === "glow" || cur < 0) render(); }
  else if (e.key === "[") { gain = Math.max(1, gain / 2); if (mode === "heat" || mode === "glow" || cur < 0) render(); }
});

// Score everything lazily at load; the nav and overview rank as results land.
rebuildNav(); render();
let pending = scenes.length;
scenes.forEach(s => score(s, () => { if (--pending % 2 === 0 || !pending) { rebuildNav(); if (cur < 0) render(); } }));
</script>
"""

if __name__ == "__main__":
    main()
