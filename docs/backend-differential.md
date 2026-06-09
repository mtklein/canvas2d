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
operations in a grid, gradients, image sampling, and a clip mask.

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

With that one change the software backend reproduces Metal **bit-for-bit** across
all 26 composite operations, gradients, image sampling, and clipping. `backenddiff`
is locked at tolerance **0**: any per-channel difference now fails the build.

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
