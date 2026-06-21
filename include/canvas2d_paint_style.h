#pragma once

// Fill and stroke style enums, shared between the public API (canvas2d.h, which
// re-includes this) and the leaf rasterizer/stroker modules that consume them
// directly (canvas2d_cover.h, canvas2d_stroke.h) without pulling in the full
// public surface.  One definition each: the values are the wire/API contract.

enum canvas2d_fill_rule { CANVAS2D_NONZERO, CANVAS2D_EVENODD };
enum canvas2d_line_join { CANVAS2D_JOIN_MITER, CANVAS2D_JOIN_ROUND, CANVAS2D_JOIN_BEVEL };
enum canvas2d_line_cap { CANVAS2D_CAP_BUTT, CANVAS2D_CAP_ROUND, CANVAS2D_CAP_SQUARE };
