#!/usr/bin/env python3
"""Visual review of gallery changes: ranked overview, blink, heatmap, swipe,
side-by-side, and a Digital Color Meter-style loupe.

The image equivalent of `git diff <ref> -- gallery/`: every changed gallery
PNG becomes a before/after pair in one self-contained HTML page (the PNGs are
base64-embedded, so the file works from anywhere, needs no server, and the
data: URIs keep the canvas untainted for pixel work).

  python3 tools/gallery_diff.py              # vs github/main (the push gap)
  python3 tools/gallery_diff.py HEAD~5       # vs any ref
  python3 tools/gallery_diff.py --no-open    # just print the output path

Keys in the page: `o` for the ranked overview; j/k scan scenes in the nav's
ranked order (arrows are left to the browser, for scrolling); 1/2/3/4 switch modes (blink / heatmap / swipe / side-by-side), space pauses
the blink clock and steps it by hand (Enter resumes), [ and ] adjust heatmap
gain, + and - adjust loupe zoom, m toggles the loupe, and a left-click pins
the loupe in place (another click releases it back to following the mouse).

Scenes rank worst-first by weighted change -- %-of-pixels-changed dominating,
per-pixel magnitude crediting with diminishing (sqrt) returns.  Hovering any
image positions the loupe, and the loupe is itself a tiny blink view: one
global clock alternates it between ref and worktree in EVERY mode (space
pauses/steps it, Enter resumes), its position persisting across mode flicks --
line up on a pixel, toggle heatmap and back, never lose the spot.  The readout
gives both sides' center pixel as unorm8 hex and float plus the delta.  The overview's
thumbnails use idiff's CSS difference trick (github.com/mtklein/idiff) --
stacked images under mix-blend-mode:difference and grayscale+brightness --
kept there because it renders before any decode lands.
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
# worst-first by weighted change with a heatmap thumbnail per row (the same
# renderer and gain as mode 2); the per-scene modes are blink (default; 2 Hz, space
# steps by hand) | heatmap (per-pixel max channel delta, exact stats,
# adjustable gain) | swipe (pointer-driven divider) | side-by-side.  The
# loupe magnifies around the cursor in every mode and stays live through
# blink; its readout gives both sides' center pixel as #RRGGBBAA and float.
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
  figure { margin:0; text-align:center; color:#8b93a7; }  /* kill the UA 1em
       figure margin: modes must not shift the image when swapped (lining up
       heatmap against blink is the whole point of mode-flicking) */
  img, canvas { image-rendering:pixelated; background:
       repeating-conic-gradient(#23262e 0 25%, #1b1e25 0 50%) 0 0/16px 16px; border:1px solid #2a2e38; }
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
  table#over img, table#over canvas { max-height:96px; }
  table#over .num { color:#8b93a7; font-variant-numeric:tabular-nums; text-align:right; }
  #loupe { position:fixed; right:16px; bottom:16px; background:#10131a; border:1px solid #3a4152;
           border-radius:8px; padding:8px; display:none; z-index:9; box-shadow:0 4px 24px #000a; }
  #loupe canvas { display:block; border:1px solid #2a2e38; background:#000; }
  #loupe pre { margin:6px 0 0; font:11px/1.5 ui-monospace, monospace; color:#cdd3df; }
  #loupe pre .on { color:#fff; font-weight:600; }
  #loupe pre .dim { color:#8b93a7; }
</style>
<div id="nav"><h1>vs __REF__ <span class="score">(o: overview)</span></h1></div>
<div id="main">
  <div id="bar">
    <button class="mode" data-m="blink">1 blink</button>
    <button class="mode" data-m="heat">2 heatmap</button>
    <button class="mode" data-m="swipe">3 swipe</button>
    <button class="mode" data-m="side">4 side-by-side</button>
    <span id="stats"></span>
  </div>
  <div id="view"></div>
</div>
<div id="loupe"><canvas width="200" height="200"></canvas><pre></pre></div>
<script>
const scenes = __SCENES__;
let cur = -1, mode = "blink", gain = 16, blinkShow = 0, blinkTimer = null,
    paused = false, zoom = 8, loupeOn = true, swipeFrac = 0.5, pinned = false,
    hover = null;  // {x, y} in image space; persists across modes and wiggles
const nav = document.getElementById("nav"), view = document.getElementById("view"),
      stats = document.getElementById("stats"),
      loupe = document.getElementById("loupe"),
      lcv = loupe.querySelector("canvas"), lpre = loupe.querySelector("pre");

function img(src) { const e = new Image(); e.src = src; return e; }

function heatCanvas(s) {  // the one heatmap renderer: mode 2 and the overview
  const {w, h, pa, pb} = s.px;
  const cv = document.createElement("canvas"); cv.width = w; cv.height = h;
  const cx = cv.getContext("2d"), out = cx.createImageData(w, h);
  for (let i = 0; i < pa.length; i += 4) {
    const d = Math.max(Math.abs(pa[i]-pb[i]), Math.abs(pa[i+1]-pb[i+1]),
                       Math.abs(pa[i+2]-pb[i+2]), Math.abs(pa[i+3]-pb[i+3]));
    const v = Math.min(255, d * gain);
    out.data[i] = v; out.data[i+1] = d ? 48 : 0; out.data[i+2] = 0; out.data[i+3] = 255;
  }
  cx.putImageData(out, 0, 0);
  return cv;
}

// Decode once per scene; data: URIs keep the canvas untainted.  Alongside the
// raw pixel arrays, keep each side as a canvas the loupe can drawImage from.
function pixels(s, cb) {
  if (s.px) return cb(s.px);
  const a = new Image(), b = new Image(); let n = 0;
  const done = () => { if (++n < 2) return;
    const w = b.naturalWidth, h = b.naturalHeight;
    const cv = new OffscreenCanvas(w, h), cx = cv.getContext("2d", {willReadFrequently:true});
    cx.drawImage(a, 0, 0); const pa = cx.getImageData(0, 0, w, h).data;
    cx.clearRect(0, 0, w, h);
    cx.drawImage(b, 0, 0); const pb = cx.getImageData(0, 0, w, h).data;
    const mk = p => { const c = new OffscreenCanvas(w, h);
      c.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(p), w, h), 0, 0); return c; };
    s.px = {w, h, pa, pb, cva: mk(pa), cvb: mk(pb)}; cb(s.px);
  };
  a.onload = done; b.onload = done; a.src = s.before; b.src = s.after;
}

// Rank worst-first by weighted change: %-of-pixels-changed dominates,
// magnitude credits with diminishing (sqrt) returns.
function score(s, cb) {
  if (s.w !== undefined) return cb && cb();
  if (!s.before || !s.after) { s.w = 1; s.pct = 1; s.count = NaN; s.max = NaN; return cb && cb(); }
  pixels(s, ({w, h, pa, pb}) => {
    let wsum = 0, count = 0, max = 0;
    for (let i = 0; i < pa.length; i += 4) {
      const d = Math.max(Math.abs(pa[i]-pb[i]), Math.abs(pa[i+1]-pb[i+1]),
                         Math.abs(pa[i+2]-pb[i+2]), Math.abs(pa[i+3]-pb[i+3]));
      if (d) { count++; if (d > max) max = d; wsum += Math.sqrt(d / 255); }
    }
    s.count = count; s.max = max; s.pct = count / (w * h); s.w = wsum / (w * h);
    cb && cb();
  });
}

function ranked() {  // nav display order: worst first; arrows scan this order
  return scenes.map((s, i) => i).sort((x, y) => (scenes[y].w ?? 0) - (scenes[x].w ?? 0));
}

function step(delta) {  // move through the RANKED order, as the nav reads
  const order = ranked();
  const at = Math.max(0, order.indexOf(cur));
  cur = order[(at + delta + order.length) % order.length];
  render();
}

function rebuildNav() {
  const order = ranked();
  nav.querySelectorAll("button").forEach(b => b.remove());
  for (const i of order) {
    const s = scenes[i], b = document.createElement("button");
    b.innerHTML = s.name + (s.before ? (s.after ? "" : " <span class=badge>(gone)</span>")
                                     : " <span class=badge>(new)</span>")
      + (s.pct !== undefined && s.before && s.after
         ? ` <span class=score>${(s.pct * 100).toFixed(2)}%</span>` : "");
    b.onclick = () => { cur = i; render(); };
    b.classList.toggle("sel", i === cur);
    nav.appendChild(b); s.btn = b;
  }
}

function overview() {
  view.innerHTML = ""; hideLoupe(); clearInterval(blinkTimer); blinkTimer = null;
  stats.textContent = `ranked by weighted change; heat ×${gain} ([ and ] adjust); click a row`;
  document.querySelectorAll(".mode").forEach(b => b.classList.remove("sel"));
  const order = ranked();
  const t = document.createElement("table"); t.id = "over";
  for (const i of order) {
    const s = scenes[i], tr = document.createElement("tr");
    const name = document.createElement("td");
    name.innerHTML = `<b>${s.name}</b>` + (s.before && s.after ? "" : ' <span class=badge>' +
                     (s.after ? "(new)" : "(gone)") + "</span>");
    const num = document.createElement("td"); num.className = "num";
    num.textContent = s.pct !== undefined && s.count >= 0
      ? `${(s.pct * 100).toFixed(2)}% changed  ·  ${s.count.toLocaleString()} px  ·  max ${s.max}` : "";
    const gl = document.createElement("td");
    if (s.before && s.after && s.px) gl.append(heatCanvas(s));
    const ba = document.createElement("td");
    if (s.before) ba.append(img(s.before));
    if (s.after) ba.append(img(s.after));
    tr.append(name, num, gl, ba);
    tr.onclick = () => { cur = i; mode = "blink"; render(); };
    t.append(tr);
  }
  view.append(t);
}

// --- the loupe: a Digital Color Meter that is always a tiny blink view ------
// One global clock (below) alternates it between ref and worktree in EVERY
// mode -- the loupe always shows exactly what blink would show -- and its
// position persists across mode flicks and scene switches (clamped), so
// lining up on a pixel survives toggling 1<->2.  Space pauses/steps the clock.

function hideLoupe() { loupe.style.display = "none"; }

function imgXY(el, e) {  // event -> image-space pixel, or null off-image
  const s = scenes[cur]; if (!s.px) return null;
  const r = el.getBoundingClientRect();
  // Displayed at natural size (border 1px); map to pixel coords directly.
  const x = Math.floor((e.clientX - r.left - 1) * s.px.w / (r.width - 2));
  const y = Math.floor((e.clientY - r.top  - 1) * s.px.h / (r.height - 2));
  return (x < 0 || y < 0 || x >= s.px.w || y >= s.px.h) ? null : {x, y};
}

function watch(el) {  // arm an element as a loupe position source
  el.addEventListener("mousemove", e => {
    if (!loupeOn || cur < 0 || pinned) return;  // a pinned loupe stays put
    const at = imgXY(el, e); if (!at) return;
    hover = at;
    drawLoupe();
  });
  el.addEventListener("click", e => {  // click pins the loupe; click frees it
    if (!loupeOn || cur < 0) return;
    pinned = !pinned;
    if (!pinned) { const at = imgXY(el, e); if (at) hover = at; }  // resume here
    drawLoupe();
  });
}

function drawLoupe() {
  if (cur < 0 || !loupeOn) return;
  const s = scenes[cur]; if (!s.px) return;
  const {w, h, pa, pb, cva, cvb} = s.px;
  if (!hover) hover = {x: w >> 1, y: h >> 1};  // visible before any mousemove
  const x = Math.min(hover.x, w - 1), y = Math.min(hover.y, h - 1);
  const side = blinkShow ? "after" : "before";
  const cx = lcv.getContext("2d");
  cx.imageSmoothingEnabled = false;
  // DCM-style: fixed panel, the aperture shrinks as zoom grows (odd, centered).
  const n = Math.max(3, Math.floor(200 / zoom) | 1), half = n >> 1;
  cx.clearRect(0, 0, 200, 200);
  cx.drawImage(blinkShow ? cvb : cva, x - half, y - half, n, n, 0, 0, n * zoom, n * zoom);
  cx.strokeStyle = pinned ? "#e8b34b" : "#3d77d8"; cx.lineWidth = 2;
  cx.strokeRect(half * zoom, half * zoom, zoom, zoom);  // the metered pixel
  const i = 4 * (y * w + x);
  const hex = p => "#" + [0,1,2,3].map(k => p[i+k].toString(16).padStart(2, "0")).join("").toUpperCase();
  const flt = p => [0,1,2,3].map(k => (p[i+k] / 255).toFixed(4)).join(" ");
  const dmax = Math.max(...[0,1,2,3].map(k => Math.abs(pa[i+k] - pb[i+k])));
  const row = (tag, p, on) =>
    `<span class="${on ? "on" : "dim"}">${tag}  ${hex(p)}  ${flt(p)}</span>`;
  lpre.innerHTML = [
    row("ref", pa, side === "before"),
    row("wt ", pb, side === "after"),
    // Fixed-width fields throughout: the panel must not relayout when the
    // text changes (blink/pause flips, cursor moves) -- that jars the eye
    // off the pixels.  Status is always present; numbers pad to max width.
    `<span class="dim">Δmax ${String(dmax).padStart(3)}/255 (${String(x).padStart(4)},${String(y).padStart(4)})`
      + ` ${String(zoom).padStart(2)}x ${paused ? "paused" : "auto  "}`
      + ` ${pinned ? "pinned" : "      "}</span>`,
  ].join("\n");
  loupe.style.display = "block";
}

// One clock drives every blinking thing: the loupe in all modes, plus the
// main overlay when in blink mode.  Space pauses it and steps by hand.
function applyPhase() {
  const top = document.querySelector("#overlay .top");
  if (mode === "blink" && top) {
    top.style.visibility = blinkShow ? "visible" : "hidden";
    stats.textContent = (blinkShow ? "worktree" : "__REF__") + (paused ? " · paused" : "");
  }
  drawLoupe();
}
function tick() { blinkShow ^= 1; applyPhase(); }
function startClock() {
  clearInterval(blinkTimer); blinkTimer = null;
  if (!paused && cur >= 0) blinkTimer = setInterval(tick, 500);
}

function render() {
  if (cur < 0) return overview();
  const s = scenes[cur];
  scenes.forEach(x => x.btn && x.btn.classList.toggle("sel", x === s));
  document.querySelectorAll(".mode").forEach(b => b.classList.toggle("sel", b.dataset.m === mode));
  view.innerHTML = ""; stats.textContent = "";
  if (!s.before || !s.after) {  // new or deleted scene: nothing to compare
    const f = document.createElement("figure");
    f.append(img(s.after || s.before));
    f.insertAdjacentHTML("beforeend",
      `<figcaption class="badge">${s.after ? "new scene (not in __REF__)" : "deleted scene"}</figcaption>`);
    view.append(f); hideLoupe(); clearInterval(blinkTimer); blinkTimer = null; return;
  }
  pixels(s, () => { applyPhase(); });  // warm the loupe; draw it immediately
  startClock();
  if (mode === "side") {
    const p = document.createElement("div"); p.className = "pair";
    for (const [src, cap] of [[s.before, "__REF__"], [s.after, "worktree"]]) {
      const f = document.createElement("figure");
      const e = img(src); watch(e); f.append(e);
      f.insertAdjacentHTML("beforeend", `<figcaption>${cap}</figcaption>`);
      p.append(f);
    }
    view.append(p);
  } else if (mode === "swipe" || mode === "blink") {
    const o = document.createElement("div"); o.id = "overlay";
    const base = img(s.before), top = document.createElement("div"); top.className = "top";
    const ti = img(s.after); top.append(ti);
    const d = document.createElement("div"); d.id = "divider";
    o.append(base, top, d); view.append(o);
    watch(o);
    if (mode === "swipe") {
      const set = frac => { swipeFrac = frac; const w = base.clientWidth * frac;
        ti.style.clipPath = `inset(0 ${base.clientWidth - w}px 0 0)`; d.style.left = w + "px"; };
      base.onload = () => set(0.5); if (base.complete) set(0.5);
      o.addEventListener("mousemove",
        e => set(Math.min(1, Math.max(0, e.offsetX / base.clientWidth))));
    } else {
      d.remove();
      applyPhase();  // the shared clock (startClock above) does the blinking
    }
  } else {  // heatmap: exact, JS-computed, adjustable gain
    pixels(s, () => {
      const cv = heatCanvas(s);
      const f = document.createElement("figure"); f.append(cv);
      f.insertAdjacentHTML("beforeend", `<figcaption>delta ×${gain} ([ and ] adjust)</figcaption>`);
      view.append(f); watch(cv);
      score(s, () => { stats.textContent = statline(s); });
    });
  }
}

function statline(s) {
  const {w, h} = s.px;
  return `${s.count.toLocaleString()} px changed (${(100*s.count/(w*h)).toFixed(2)}%), max delta ${s.max}/255`;
}

document.querySelectorAll(".mode").forEach(b => b.onclick = () => {
  mode = b.dataset.m; if (cur < 0) cur = 0; render(); });
addEventListener("keydown", e => {
  if (e.key === "o" || e.key === "0") { cur = -1; render(); }
  else if (e.key === "j") { step(+1); }  // scenes are j/k (ranked order);
  else if (e.key === "k") { step(-1); }  // arrows stay the browser's, to scroll
  else if (e.key >= "1" && e.key <= "4") { mode = ["blink","heat","swipe","side"][e.key - 1];
    if (cur < 0) cur = 0; render(); }
  else if (e.key === " ") { e.preventDefault(); if (cur < 0) return;
    paused = true; clearInterval(blinkTimer); blinkTimer = null; tick(); }
  else if (e.key === "Enter") { if (cur < 0) return; paused = false; startClock(); }
  else if (e.key === "]") { gain = Math.min(256, gain * 2); if (mode === "heat" || cur < 0) render(); }
  else if (e.key === "[") { gain = Math.max(1, gain / 2); if (mode === "heat" || cur < 0) render(); }
  else if (e.key === "+" || e.key === "=") { zoom = Math.min(32, zoom * 2); drawLoupe(); }
  else if (e.key === "-") { zoom = Math.max(2, zoom / 2); drawLoupe(); }
  else if (e.key === "m") { loupeOn = !loupeOn; loupeOn ? drawLoupe() : hideLoupe(); }
});

// Score everything lazily at load; the nav and overview rank as results land.
rebuildNav(); render();
let pending = scenes.length;
scenes.forEach(s => score(s, () => { if (--pending % 2 === 0 || !pending) { rebuildNav(); if (cur < 0) render(); } }));
</script>
"""

if __name__ == "__main__":
    main()
