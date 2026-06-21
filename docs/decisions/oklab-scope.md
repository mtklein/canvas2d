# Memo: Oklab's scope — interpolation only, or inputs and images too?

**Scope read:** `include/canvas.h` (the `canvas2d_color_space` enum doc; the gradient-interp
and image-source space parameters), `src/canvas.c` (`canvas2d_in_space` working-space
validation, `intern_color`, `gradient_interp_ok`, `sample_to_working`), `src/canvas2d_color.{h,c}`
(`canvas2d_linear_srgb_to_oklab` / `canvas2d_oklab_to_linear_srgb`), `tests/test_gradient_interp.c`,
`tests/test_colorspace.c`.

## The question

`CANVAS2D_CS_OKLAB` can name a colour in four roles:

1. **input** — `set_fill_rgba` / `set_stroke_rgba` speaking a colour in Oklab;
2. **gradient interpolation** — the space colour stops blend in;
3. **image source** — `canvas2d_image_*` / `draw_bitmap` tagging an image's pixels;
4. **working space** — `canvas2d_in_space`, the space the surface stores and composites in.

The choice was minimal ("Oklab interpolates gradients, nothing else") vs maximal ("Oklab
everywhere, working surfaces included").

## Ruling (2026-06-20)

Oklab is valid as an **input, gradient-interpolation, and image-source** space (roles 1–3),
and is **not** a working/compositing space (role 4). `canvas2d_in_space(CANVAS2D_CS_OKLAB)`
returns NULL.

## Why

- Roles 1–3 are all places a caller *authors* colour: the value enters, converts once
  through linear sRGB into the working space, and is gone. Oklab earns its place there —
  a perceptually even gradient ramp, an Oklab-authored image deposited correctly. The
  conversion is the same pivot (`*_to_linear_srgb`, then the working-space encode) that
  `intern_color` and `sample_to_working` already share for sRGB and linear inputs.
- A working space is different in kind: the surface stores in it and every blend composites
  in it, over and over. Alpha compositing assumes linear light, so compositing in Oklab is
  not physical, and an Oklab surface would pay the cube-root transfer on every store. The
  compositing spaces are sRGB and extended linear sRGB; Oklab is an authoring convenience
  that resolves into one of them. This is the header's "a perceptual interpolation space,
  not a compositing space".
- Images and gradients are the same case, not two. Sampling/filtering an image tap and
  interpolating a gradient stop are both a weighted blend of colour, so a space that
  interpolates gradients interpolates image taps too. The dividing line is authoring (any
  of roles 1–3) vs compositing (role 4), not gradients vs images.

## Implementation & coverage

- **input:** `intern_color` (`src/canvas.c`), `case CANVAS2D_CS_OKLAB`.
- **gradient:** `gradient_interp_ok` (`src/canvas.c`); premultiplied-Oklab stop lerp,
  covered by `tests/test_gradient_interp.c`.
- **image:** `sample_to_working` (`src/canvas.c`), `case CANVAS2D_CS_OKLAB`; covered by
  `tests/test_colorspace.c` `oklab_image_sample_deposit` (an Oklab f16 image deposited
  through both the sRGB and linear arms).
- **not a working space:** `canvas2d_in_space` returns NULL for Oklab; asserted in the same
  test.
