# Two backends, one bit pattern: the Metal ↔ software differential

canvas2d composites through a narrow ABI ([../src/compositor.h](../src/compositor.h)):
the canvas hands the compositor *finished premultiplied RGBA16F tiles*, and the
compositor blends them onto the target under a `globalCompositeOperation` and a clip
mask. Two backends implement that ABI — the Metal GPU shim
([../src/compositor_metal.m](../src/compositor_metal.m) +
[../shaders/compositor.metal](../shaders/compositor.metal)) and a software
compositor ([../src/compositor_cpu.c](../src/compositor_cpu.c)) — and the project
leans on them being interchangeable. This is the story of checking *how*
interchangeable, and of making them **bit-for-bit identical**.

## The tool

`ninja`'s `backenddiff` gate renders a fixed set of scenes through the public API
on both backends and compares the `getImageData` bytes per channel
([../diff/diff_render.c](../diff/diff_render.c) renders + dumps raw RGBA8,
[../diff/diff_compare.c](../diff/diff_compare.c) diffs them and fails past a
tolerance). The scenes are chosen to stress compositing: all 26 composite
operations in a grid, gradients, image sampling, and two clip scenes —
translucent fills under a mask, and opaque overdrawn fills under partial
coverage (the second exists because the first missed a real divergence; see
"Seam two" below).

The leverage comes from the architecture: geometry, adaptive flattening,
analytic-coverage antialiasing, gradient evaluation, and the read-back
*un*premultiply all live in backend-agnostic CPU code and run identically for both
backends. So a pixel that differs between the two dumps can *only* be the
compositor — the blend math plus the `float → _Float16` store.

## What the differential found

The two backends agreed **exactly** on gradients, `drawImage` sampling, and any
op that, over an opaque backdrop, reduces to a pass-through (`source-in`,
`source-out`, `destination-over`, `copy` — `co = s`, `0`, or `d` left untouched).
They differed by **at most 1/255**, on roughly 1% of pixels, only where the blend
does real arithmetic with fractional source alpha (the antialiased disc edges and
translucent fills under each mode):

| scene | max Δ |
|---|---|
| `gradient`, `image` | 0 (bit-exact) |
| `modes` (26 ops, AA discs) | 1 |

The split is the tell. The modes that *don't* diverge are exactly those with no
arithmetic to round; every mode that evaluates `fa·s + fb·d` or
`s·(1−da) + d·(1−sa) + T` lands ±1 in ~1% of channels. Same inputs (the half tiles
are byte-identical on both sides), same formulas (the shader and `compositor_cpu.c`
mirror each other line for line) — so the difference is purely in how the float
arithmetic rounds into the `_Float16` target.

## Whodunit: ruling out the usual suspects

The residual survived every knob that *should* have explained it:

| hypothesis | experiment | result |
|---|---|---|
| FMA contraction | CPU `-ffp-contract=off / on / fast` | no change |
| GPU fast-math | Metal `mathMode` Fast → Safe (precise) | no change¹ |
| fixed-function ROP | route source-over through the precise shader, not the blend ROP | no change |
| GPU half-precision blend | CPU blend computed in `_Float16` | *worse* |
| CPU imprecision | CPU blend computed in `double` (exact real value) | no change |

¹ `MTLCompileOptions.fastMathEnabled` is deprecated and silently ignored on
macOS 15+; `mathMode = MTLMathModeSafe` is the live API, and even it left the output
identical — for the pure +,−,× of the blend, "fast" and "safe" agree.

The `double` result is the decisive one: computing the blend in double precision
(the exact real value, then rounded to `_Float16`) left the **same** divergence
versus the GPU. So the software backend was already producing the *correctly
rounded* answer — **the GPU is the deviant.** Metal carries up to ~1-half-ULP of
error in its compositing relative to the mathematically exact result.

## The actual rule

Isolating `source-over` with controlled inputs (a known opaque backdrop, a 1px
fill swept across all 256 source alphas) made the pattern obvious: the deviation
was **strictly one-directional** — Metal was *always* 1 low, never high, on ~2% of
samples. That is not random last-bit noise; it is the signature of **rounding
toward zero**.

> Metal's RGBA16Float store truncates (rounds toward zero); C's `(_Float16)` cast
> rounds to nearest-even. The ~1% of pixels that differ are the ones where the
> exact blend value sits just far enough above a half-grid point that nearest-even
> rounds up and truncation rounds down — and that one-half-ULP gap occasionally
> flips the final `round(255 · co)` by one.

## Matching it

So matching Metal is one rule, not 26 hand-tuned cases: **truncate every half
store** in the software blend ([../src/compositor_cpu.c](../src/compositor_cpu.c),
`to_half_rtz`):

```c
static _Float16 to_half_rtz(float v) {   // v >= 0 here, so toward-zero == floor
    _Float16 n = (_Float16)v;
    if ((float)n > v) {                  // nearest-even overshot — step one ULP down
        uint16_t bits; memcpy(&bits, &n, sizeof bits); bits -= 1; memcpy(&n, &bits, sizeof bits);
    }
    return n;
}
```

With that one change the software backend reproduced Metal **bit-for-bit** across
all 26 composite operations, gradients, image sampling, and clipping — on every
scene then gated. `backenddiff` is locked at tolerance **0**: any per-channel
difference fails the build. (It wasn't the whole story: clipping hides a second
seam the original scenes never reached — see "Seam two" below.)

## The trade-off (this is a deliberate hack)

The code is clean; the semantics are the cost, and they are worth stating plainly:

- **It makes the software compositor less accurate on purpose.** It was the
  correctly-rounded backend; truncation introduces up to ~1-half-ULP of downward
  bias so it mirrors the GPU's error. We bent the *right* answer to match the
  *convenient* one — chosen because the GPU is the less-controllable reference, and
  byte-identical backends are worth more here than a fraction of an LSB of accuracy.
- **It couples the output to this GPU's store rounding.** A Metal device that
  rounds-to-nearest would now make the *software* backend the one that's "wrong,"
  and `backenddiff` would fail. That is a footgun (a green build depends on Apple
  GPU behaviour) and a feature (the gate catches the drift the instant it appears).
- **The compute cost is negligible** — a predictable branch and a rare two-byte
  twiddle per channel, in the software backend only; Metal is untouched.

## Seam two: clip attenuation (found by gallery/clip.png)

The story above held until a committed artifact falsified it: `gallery/clip.png`
rendered differently on the two backends — 20 of 144,000 channels off by exactly
1/255 — while `backenddiff` stayed green. The gate's clip scene composites
*translucent* fills under the mask; the gallery's draws *opaque* stripes that
overdraw each other under partial coverage, and that shape reaches a second
rounding seam:

> **The CPU rounded the clip-attenuated source to `_Float16`** before blending —
> an artifact of storing it back into a half struct — where both Metal shaders
> hand their blend math the *unrounded* float32 `tile × clip` product.

The fix keeps the attenuated source in float through the blend on both CPU paths,
and `diff_render.c` now carries a `clipstripes` scene (opaque overlapping fills
under antialiased clip edges, mirroring the gallery's) that reproduces the
divergence under the gate.

## What a raw-half differential then found

The gate compares unpremultiplied RGBA8 readback, and that 8-bit quantization
forgives last-ULP errors in the RGBA16F target. Driving `compositor.h` directly
on both backends — dense pseudo-random premultiplied half tiles, two overdrawn
layers, a clip mask covering all 256 coverage bytes, raw target halves compared
bitwise — showed how much it had been forgiving: with a fractional clip in play,
*every* mode diverged on ~10–15% of half-channels. With the clip open: bit-exact
everywhere.

That split explains why the whole edifice held before clipping arrived. Tiles,
targets, and coverage are halves — 11-bit mantissas, whose products fit exactly
in float32's 24. Exact products leave nothing for rounding *or contraction* to
disagree on. A fractional clip multiplies the source by `k = byte/255`, a
full-mantissa float; from then on every product rounds, and it starts to matter
how each side fuses multiplies into adds:

| site | Metal's shape (measured against the sweep) |
|---|---|
| source-over (fixed-function ROP) | fused: `co = fma(1−sa, d, s)` |
| Porter-Duff `co = fa·s + fb·d` | *not* fused: rounded multiplies, then add |
| blend-mode epilogue `co = s·(1−da) + d·(1−sa) + T` | right-nested chain: `fma(s, 1−da, fma(d, 1−sa, T))` |
| blend-mode `ao = sa + da·(1−sa)` | `fma(da, 1−sa, sa)` |

[../src/compositor_cpu.c](../src/compositor_cpu.c) writes the ROP and epilogue
shapes explicitly (`__builtin_elementwise_fma` / `fmaf`); the rest matches
because the C kernel mirrors the shader expression for expression and both
compilers are LLVM contracting the same trees the same way — an invariant the
gate enforces.

## The floor: division

With those shapes matched, the raw-half sweep agrees bit-for-bit across
source-over, all ten Porter-Duff operators, and the eight polynomial blend modes
— millions of randomized samples, every coverage byte. What remains, and will
not yield, is division: a compute-kernel probe measured Metal's float divide
not-correctly-rounded on 28% of random [0,1] operand pairs (≤ 2 ULP; `sqrt` 32%).
The seven modes whose math divides (color-dodge, color-burn, soft-light, and the
four HSL modes) can land one half-ULP off the GPU under fractional clip — about
1 in 10⁴ pixels — and source-over/screen/exclusion keep a ~1-in-10⁵ single-ULP
tail from ROP datapath minutiae. Matching that would mean reverse-engineering
Apple's reciprocal hardware, and would overfit one GPU generation.

## The claim, scoped

So "bit-for-bit identical" means, precisely:

- **Gated at tolerance 0:** the RGBA8 readback of the five diff scenes — and in
  practice every committed gallery rendering (all 32 byte-identical across
  backends).
- **With the clip open** (no `clip()` in effect): bit-exact at the raw RGBA16F
  level — every product is exact in float32 and both backends round identically
  into the half store.
- **Under fractional clip coverage:** bit-exact at the raw level for source-over,
  Porter-Duff, and the polynomial blend modes, up to the measured ~10⁻⁵ tail;
  the divide-using modes carry the GPU divider's last half-ULP at ~10⁻⁴. None of
  this has ever surfaced through the gate's 8-bit readback.

## Using it

`ninja` runs `backenddiff` as part of the default build (silent on success). To see
the per-scene divergence report by hand:

```sh
ninja backenddiff
./build/diff/diff_compare build/release/diffdump build/release-cpu/diffdump 0
```

A non-zero exit means the backends drew different bytes — read the report for the
offending scene and worst pixel. For the `modes` montage a worst pixel at `(x, y)`
lives in grid cell `(x/56, y/56)` → operation `row·6 + col`.
