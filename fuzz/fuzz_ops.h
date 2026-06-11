#pragma once

// Opcodes for the canvas2d API state-machine fuzzer (Role A: a *total* byte->op
// decoder -- every byte string maps to some clamped sequence of public API
// calls, never rejected, so a coverage fuzzer reaches deep into the renderer).
// Shared by the harness (fuzz_api.c) and the seed generator (seed_gen.c).

enum fuzz_op {
    OP_BEGIN_PATH,
    OP_MOVE_TO,
    OP_LINE_TO,
    OP_RECT,
    OP_QUAD_TO,
    OP_CUBIC_TO,
    OP_ARC,
    OP_ELLIPSE,
    OP_ROUND_RECT,
    OP_ARC_TO,
    OP_CLOSE_PATH,

    OP_FILL,
    OP_STROKE,
    OP_CLIP,
    OP_FILL_RECT,
    OP_CLEAR_RECT,

    OP_SET_FILL_RGBA,
    OP_SET_STROKE_RGBA,
    OP_SET_GLOBAL_ALPHA,
    OP_SET_LINE_WIDTH,
    OP_SET_LINE_JOIN,
    OP_SET_LINE_CAP,
    OP_SET_MITER_LIMIT,
    OP_SET_FILL_RULE,
    OP_SET_COMPOSITE,
    OP_SET_LINE_DASH,
    OP_SET_DASH_OFFSET,

    OP_FILL_LINEAR_GRAD,
    OP_FILL_RADIAL_GRAD,
    OP_ADD_FILL_STOP,
    OP_STROKE_LINEAR_GRAD,
    OP_ADD_STROKE_STOP,

    OP_SAVE,
    OP_RESTORE,
    OP_TRANSLATE,
    OP_SCALE,
    OP_ROTATE,
    OP_TRANSFORM,
    OP_SET_TRANSFORM,
    OP_RESET_TRANSFORM,

    OP_GET_IMAGE_DATA,
    OP_PUT_IMAGE_DATA,
    OP_DRAW_IMAGE,

    FUZZ_OP_COUNT,
};
