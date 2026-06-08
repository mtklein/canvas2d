// Core Text glyph-outline shim.  Built WITHOUT -fbounds-safety (see configure.py
// BOUNDARY_C) so it can bind the un-annotated CoreText/CoreGraphics headers as
// plain C: no opaque-handle forges, no fight with the strict function-pointer
// check on CGPathApply's callback.  It hands the checked core finished
// device-space cnvs_paths, so the unsafety stops at this file -- the same
// arrangement as the Metal compositor, but in C rather than Objective-C.

#include "cnvs_font.h"

#include "cnvs_math.h"
#include "cnvs_path.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <stdint.h>
#include <stdlib.h>

struct cnvs_font {
    CTFontRef font;
};

cnvs_font *cnvs_font_create(char const *name, float size_px) {
    CFStringRef cfname =
        CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    if (!cfname) {
        return NULL;
    }
    CTFontRef font = CTFontCreateWithName(cfname, size_px, NULL);
    CFRelease(cfname);
    if (!font) {
        return NULL;
    }
    cnvs_font *f = calloc(1, sizeof *f);
    if (!f) {
        CFRelease(font);
        return NULL;
    }
    f->font = font;
    return f;
}

void cnvs_font_destroy(cnvs_font *f) {
    if (!f) {
        return;
    }
    CFRelease(f->font);
    free(f);
}

// CGPathApply callback context: where to append, and the glyph-space -> device
// mapping (baseline origin already folded into `ox`/`oy`).
struct walk {
    cnvs_path *out;
    cnvs_mat to_device;
    float ox, oy;
    float tol;
};

// Core Text glyph space is px-at-size, baseline-relative, y up.  Flip y, place at
// the pen origin (user space), then map to device.
static cnvs_vec2 gpt(struct walk const *w, CGPoint p) {
    cnvs_vec2 u = { w->ox + (float)p.x, w->oy - (float)p.y };
    return cnvs_mat_apply(w->to_device, u);
}

static void emit(void *info, CGPathElement const *e) {
    struct walk *w = info;
    switch (e->type) {
        case kCGPathElementMoveToPoint:
            cnvs_path_move_to(w->out, gpt(w, e->points[0]));
            break;
        case kCGPathElementAddLineToPoint:
            cnvs_path_line_to(w->out, gpt(w, e->points[0]));
            break;
        case kCGPathElementAddQuadCurveToPoint:
            cnvs_path_quad_to(w->out, gpt(w, e->points[0]),
                              gpt(w, e->points[1]), w->tol);
            break;
        case kCGPathElementAddCurveToPoint:
            cnvs_path_cubic_to(w->out, gpt(w, e->points[0]),
                               gpt(w, e->points[1]), gpt(w, e->points[2]), w->tol);
            break;
        case kCGPathElementCloseSubpath:
            cnvs_path_close(w->out);
            break;
    }
}

// Decode one UTF-8 sequence at *p (NUL-terminated) and advance *p past it.
// Malformed bytes decode as U+FFFD; the scan never reads past the terminator.
static uint32_t utf8_next(unsigned char const **p) {
    unsigned char const *s = *p;
    uint32_t c = s[0];
    if (c < 0x80) {
        *p = s + 1;
        return c;
    }
    int n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { n = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { n = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { n = 3; cp = c & 0x07; }
    else { *p = s + 1; return 0xFFFD; }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) {  // truncated or invalid (stops at the NUL)
            *p = s + i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    *p = s + n + 1;
    return cp;
}

// Map a Unicode code point to a glyph, composing a surrogate pair for astral
// code points (CJK is in the BMP, so the single-unit path covers it).
static CGGlyph glyph_for_cp(CTFontRef font, uint32_t cp) {
    if (cp > 0xFFFF) {
        cp -= 0x10000;
        UniChar pair[2] = { (UniChar)(0xD800 + (cp >> 10)),
                            (UniChar)(0xDC00 + (cp & 0x3FF)) };
        CGGlyph g[2] = { 0, 0 };
        CTFontGetGlyphsForCharacters(font, pair, g, 2);
        return g[0];
    }
    UniChar uc = (UniChar)cp;
    CGGlyph g = 0;
    CTFontGetGlyphsForCharacters(font, &uc, &g, 1);
    return g;
}

float cnvs_font_outline(cnvs_font *f, char const *text, float ox, float oy,
                        cnvs_mat to_device, float tol, cnvs_path *out) {
    float pen = 0.0f;
    for (unsigned char const *s = (unsigned char const *)text; *s;) {
        CGGlyph g = glyph_for_cp(f->font, utf8_next(&s));

        CGPathRef path = CTFontCreatePathForGlyph(f->font, g, NULL);  // NULL for blanks
        if (path) {
            struct walk w = { .out = out, .to_device = to_device,
                              .ox = ox + pen, .oy = oy, .tol = tol };
            CGPathApply(path, &w, emit);
            CGPathRelease(path);
        }

        CGSize adv = { 0.0, 0.0 };
        CTFontGetAdvancesForGlyphs(f->font, kCTFontOrientationHorizontal, &g, &adv, 1);
        pen += (float)adv.width;
    }
    return pen;
}

float cnvs_font_advance(cnvs_font *f, char const *text) {
    float pen = 0.0f;
    for (unsigned char const *s = (unsigned char const *)text; *s;) {
        CGGlyph g = glyph_for_cp(f->font, utf8_next(&s));
        CGSize adv = { 0.0, 0.0 };
        CTFontGetAdvancesForGlyphs(f->font, kCTFontOrientationHorizontal, &g, &adv, 1);
        pen += (float)adv.width;
    }
    return pen;
}
