# Memo: perspective (projective) transforms

**Scope read:** `src/canvas2d_math.{h,c}` (`canvas2d_matrix` — the 2×3 affine CTM and its
`mul`/`apply`/`invert`/`translate`/`scale`/`rotate`), `src/canvas.c` (path baking
`canvas2d_matrix_apply` ~1069, the 8-wide sampler row DDA ~1908 "only x varies", the
device→source inverse ~4255, pattern inverse ~773, `ctm_scale` ~691,
`ctm_rotation` ~740, `set_transform` ~582, the text transform ~3360/3494),
`src/canvas2d_stroke.c`, `src/canvas2d_path.c` (flattening), `src/canvas2d_record.c` /
`src/canvas2d_replay.c`.

## Goal

Support **2D projective (perspective) transforms** — a deliberate extension beyond
the HTML Canvas 2D spec (which is affine-only), the marquee case from the
geometric-algebra discussion. The CTM becomes a 3×3 homography; affine is the
subset, and stays on a fast path so existing scenes are byte-identical and pay no
per-pixel cost.

## The model

The CTM is a 3×3 homography applied to `[x, y, 1]`:

    x' = (a·x + c·y + e) / w,  y' = (b·x + d·y + f) / w,  w = g·x + h·y + i

Affine is `(g, h, i) = (0, 0, 1)` (then `w ≡ 1`, no divide). Projective maps send
lines to lines, so straight edges stay straight — geometry is handled by
projecting vertices; only *sampling* needs the per-pixel divide.

## Decisions

1. **API: a 3×3 primitive + a quad helper.** `canvas2d_set_transform_3x3` /
   `canvas2d_transform_3x3` (the nine-element setter and concat) for the general
   homography, plus `canvas2d_set_perspective_quad` (map a source rect to four
   destination corners — computes the homography from the four correspondences)
   for the common "project onto a quad" case.
2. **Full scope, including sampling.** Both projective geometry AND
   perspective-correct sampling for gradients, images, and patterns.
3. **Recording stays simple/unconditional.** `set_transform`/`transform` always
   record nine numbers; existing affine `.canvas` gain a trailing `0 0 1` on those
   lines (no `.png` change, affine is byte-identical). No separate perspective op.

## The three pieces

- **Geometry (P1).** Extend `canvas2d_matrix` to nine terms; `apply` divides by `w`
  (fast path when affine); `mul`/`invert` become 3×3. Flatten curves in user
  space, project the vertices, scanline-fill as today. The real subtlety is
  **w≤0 clipping**: a vertex behind the projection plane projects to garbage, so
  polygons crossing `w=0` are clipped in homogeneous space (against `w=ε`) before
  the divide. Stroke geometry is built in user space and then projected (width
  constant in user space, foreshortening naturally); `ctm_scale` /
  `ctm_rotation` take the affine part.
- **Perspective-correct sampling (P2).** The sampler's per-row source-coordinate
  loop carries `u/w`, `v/w`, `1/w` linearly across the span and divides per
  pixel (gradients, images, patterns). The affine fast path keeps the current
  linear DDA unchanged.

## Phasing

- **P1** — the 3×3 CTM + ops + API + recording, projective vertex geometry + w≤0
  clip, stroke/scale handling. Solid fills/strokes/text take perspective. Gallery
  scene (a projected shape/quad outline); tests; affine scenes byte-identical.
- **P2** — perspective-correct sampling for gradients/images/patterns. Gallery
  scene (an image projected onto a quad — the classic); tests.

## Risks

- **w≤0 clipping** is the correctness crux of P1 (homogeneous clip before divide).
- The **sampler rewrite** (P2) is the deepest change; the affine fast path must
  stay byte-identical (existing scenes are the gate).
- **Flattening tolerance** under perspective: flattening in user space can
  over/under-tessellate after foreshortening. Acceptable initially (flatten as
  today, project); revisit if a scene shows artifacts.
- Determinism rides on the affine fast path leaving every existing scene
  byte-for-byte unchanged — the byte gate is the backstop.
