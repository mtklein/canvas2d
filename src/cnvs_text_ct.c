// Core Text shaping shim.  Built without -fbounds-safety (configure.py BOUNDARY_C)
// to bind the un-annotated CoreText headers; it shapes UTF-8 into glyph runs and
// copies each run into checked-owned cnvs_glyph_run arrays for the checked core.
// See docs/text-boundary.md.

#include "cnvs_text.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <stdlib.h>
#include <string.h>

// Copy a run's font name into the checked core's buffer.  The opaque CTFontRef goes
// in; CFStringGetCString fills `buf` within `cap` (so the boundary respects the
// caller's bound -- the inverse of the glyph-run hand-off).
int cnvs_run_font_name(void const *font, char *buf, int cap) {
    if (!font || cap <= 0) {
        return -1;
    }
    CFStringRef name = CTFontCopyPostScriptName((CTFontRef)font);
    if (!name) {
        return -1;
    }
    Boolean ok = CFStringGetCString(name, buf, cap, kCFStringEncodingUTF8);
    CFRelease(name);
    return ok ? (int)strlen(buf) : -1;
}

void cnvs_shaped_free(cnvs_shaped *s) {
    if (!s) {
        return;
    }
    if (s->run) {
        for (int r = 0; r < s->nruns; r++) {
            free(s->run[r].glyph);
            free(s->run[r].xadv);
            free(s->run[r].cluster);
            if (s->run[r].font) {
                CFRelease(s->run[r].font);
            }
        }
        free(s->run);
    }
    free(s);
}

// Copy one CTRun into the checked-owned dst.  The copy variants (CTRunGetGlyphs etc.)
// always work, even when CTRunGet*Ptr returns NULL -- which it does for ligature runs
// (a non-contiguous run), so the always-copy path is the robust boundary.
static bool copy_run(CTRunRef run, cnvs_glyph_run *dst) {
    CFIndex gc = CTRunGetGlyphCount(run);
    dst->count = (int)gc;
    dst->rtl = (CTRunGetStatus(run) & kCTRunStatusRightToLeft) != 0;
    // The run's font (font fallback): retain it so the opaque handle outlives the
    // CTLine, store it as void* for the checked core to pass back to the boundary.
    CFDictionaryRef ra = CTRunGetAttributes(run);
    CTFontRef rf = ra ? CFDictionaryGetValue(ra, kCTFontAttributeName) : NULL;
    dst->font = rf ? (void *)CFRetain(rf) : NULL;
    dst->glyph = malloc((size_t)gc * sizeof *dst->glyph);
    dst->xadv = malloc((size_t)gc * sizeof *dst->xadv);
    dst->cluster = malloc((size_t)gc * sizeof *dst->cluster);
    CGGlyph *tg = malloc((size_t)gc * sizeof *tg);
    CGSize *ta = malloc((size_t)gc * sizeof *ta);
    CFIndex *ti = malloc((size_t)gc * sizeof *ti);
    bool ok = dst->glyph && dst->xadv && dst->cluster && tg && ta && ti;
    if (ok) {
        CFRange all = CFRangeMake(0, gc);
        CTRunGetGlyphs(run, all, tg);
        CTRunGetAdvances(run, all, ta);
        CTRunGetStringIndices(run, all, ti);
        for (CFIndex i = 0; i < gc; i++) {
            dst->glyph[i] = tg[i];
            dst->xadv[i] = (float)ta[i].width;
            dst->cluster[i] = (int)ti[i];
        }
    }
    free(tg);
    free(ta);
    free(ti);
    return ok;
}

// Glyph-outline walk: Core Text glyph space is y-up, baseline-relative; flip y,
// place at the pen origin, map user->device.
struct walk {
    cnvs_path *out;
    cnvs_mat to_device;
    float ox, oy;
    float tol;
};

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
            cnvs_path_quad_to(w->out, gpt(w, e->points[0]), gpt(w, e->points[1]), w->tol);
            break;
        case kCGPathElementAddCurveToPoint:
            cnvs_path_cubic_to(w->out, gpt(w, e->points[0]), gpt(w, e->points[1]),
                               gpt(w, e->points[2]), w->tol);
            break;
        case kCGPathElementCloseSubpath:
            cnvs_path_close(w->out);
            break;
    }
}

void cnvs_glyph_outline(void *font, uint16_t glyph, float ox, float oy,
                        cnvs_mat to_device, float tol, cnvs_path *out) {
    if (!font) {
        return;
    }
    CGPathRef path = CTFontCreatePathForGlyph((CTFontRef)font, (CGGlyph)glyph, NULL);
    if (path) {  // NULL for blanks and for color glyphs (emoji) -- no outline
        struct walk w = { .out = out, .to_device = to_device, .ox = ox, .oy = oy, .tol = tol };
        CGPathApply(path, &w, emit);
        CGPathRelease(path);
    }
}

void cnvs_glyph_draw(void *font, uint16_t glyph, float x, float y,
                     uint8_t *px, int w, int h) {
    if (!font || w <= 0 || h <= 0) {
        return;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, (size_t)w, (size_t)h, 8, (size_t)w * 4,
                                             cs, kCGImageAlphaPremultipliedLast);
    if (ctx) {
        CGGlyph g = (CGGlyph)glyph;
        CGPoint pos = { (CGFloat)x, (CGFloat)y };
        CTFontDrawGlyphs((CTFontRef)font, &g, &pos, 1, ctx);
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(cs);
}

bool cnvs_run_is_color(void const *font) {
    if (!font) {
        return false;
    }
    return (CTFontGetSymbolicTraits((CTFontRef)font) & kCTFontTraitColorGlyphs) != 0;
}

void cnvs_glyph_bounds(void *font, uint16_t glyph, float *x0, float *y0,
                       float *x1, float *y1) {
    *x0 = 0.0f; *y0 = 0.0f; *x1 = 0.0f; *y1 = 0.0f;
    if (!font) {
        return;
    }
    CGGlyph g = (CGGlyph)glyph;
    CGRect r = CTFontGetBoundingRectsForGlyphs((CTFontRef)font,
                                               kCTFontOrientationDefault, &g, NULL, 1);
    if (CGRectIsNull(r) || CGRectIsEmpty(r)) {
        return;
    }
    *x0 = (float)CGRectGetMinX(r); *y0 = (float)CGRectGetMinY(r);
    *x1 = (float)CGRectGetMaxX(r); *y1 = (float)CGRectGetMaxY(r);
}

cnvs_shaped *cnvs_shape(char const *name, float size_px, char const *text) {
    CFStringRef cfname = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
    CFStringRef str = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    CTFontRef font = cfname ? CTFontCreateWithName(cfname, size_px, NULL) : NULL;
    if (cfname) {
        CFRelease(cfname);
    }
    cnvs_shaped *out = NULL;
    if (font && str) {
        CFStringRef keys[1] = { kCTFontAttributeName };
        const void *vals[1] = { font };
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void **)keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef astr = CFAttributedStringCreate(NULL, str, attrs);
        CTLineRef line = CTLineCreateWithAttributedString(astr);
        CFArrayRef runs = CTLineGetGlyphRuns(line);
        CFIndex nruns = CFArrayGetCount(runs);

        out = calloc(1, sizeof *out);
        if (out) {
            out->text_len = (int)CFStringGetLength(str);
            out->nruns = (int)nruns;
            out->run = calloc((size_t)nruns, sizeof *out->run);
            bool ok = out->run != NULL;
            for (CFIndex r = 0; ok && r < nruns; r++) {
                ok = copy_run((CTRunRef)CFArrayGetValueAtIndex(runs, r), &out->run[r]);
            }
            if (!ok) {
                cnvs_shaped_free(out);
                out = NULL;
            }
        }
        CFRelease(line);
        CFRelease(astr);
        CFRelease(attrs);
    }
    if (font) {
        CFRelease(font);
    }
    if (str) {
        CFRelease(str);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Font handle + the single-font measureText / advance / vmetrics path.
// ---------------------------------------------------------------------------

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

// Decode one UTF-8 sequence at *p (with `end` one past the last byte) and advance
// *p past it.  Malformed or truncated bytes decode as U+FFFD; the scan is bounded
// by `end`, so it never reads past the buffer even without a NUL terminator.
// Callers guarantee *p < end on entry.
static uint32_t utf8_next(unsigned char const **p, unsigned char const *end) {
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
        if (s + i >= end || (s[i] & 0xC0) != 0x80) {  // past end, truncated, or invalid
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

float cnvs_font_advance(cnvs_font *f, char const *text, int len) {
    float pen = 0.0f;
    unsigned char const *s = (unsigned char const *)text;
    unsigned char const *end = s + (len > 0 ? len : 0);
    while (s < end) {
        CGGlyph g = glyph_for_cp(f->font, utf8_next(&s, end));
        CGSize adv = { 0.0, 0.0 };
        CTFontGetAdvancesForGlyphs(f->font, kCTFontOrientationHorizontal, &g, &adv, 1);
        pen += (float)adv.width;
    }
    return pen;
}

void cnvs_font_measure(cnvs_font *f, char const *text, int len, cnvs_text_metrics *m) {
    CTFontRef font = f->font;

    // Font-wide metrics (independent of the text).  Core Text reports ascent and
    // descent as positive magnitudes from the baseline.
    double ascent = CTFontGetAscent(font);
    double descent = CTFontGetDescent(font);
    double size = CTFontGetSize(font);
    m->font_ascent = (float)ascent;
    m->font_descent = (float)descent;
    // Split the em square (height == size) by the ascent/descent ratio; the two
    // sum to the em size by construction.
    double denom = ascent + descent;
    double em_asc = denom > 0.0 ? size * ascent / denom : size;
    m->em_ascent = (float)em_asc;
    m->em_descent = (float)(size - em_asc);
    // Baseline offsets relative to the alphabetic baseline (the reference).
    m->alphabetic_baseline = 0.0f;
    m->hanging_baseline = (float)ascent;       // ~top of the ascenders
    m->ideographic_baseline = -(float)descent;  // ~bottom of the descenders

    // Walk the glyphs, summing advances and unioning each glyph's tight bounding
    // rect (glyph space: y up, baseline at 0) offset by the running pen.
    double pen = 0.0;
    bool any = false;
    double minx = 0.0, maxx = 0.0, miny = 0.0, maxy = 0.0;
    unsigned char const *s = (unsigned char const *)text;
    unsigned char const *end = s + (len > 0 ? len : 0);
    while (s < end) {
        CGGlyph g = glyph_for_cp(font, utf8_next(&s, end));
        CGRect r =
            CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal,
                                            &g, NULL, 1);
        if (!CGRectIsNull(r) && !CGRectIsEmpty(r)) {
            double x0 = pen + r.origin.x;
            double x1 = x0 + r.size.width;
            double y0 = r.origin.y;             // bottom (y up)
            double y1 = y0 + r.size.height;     // top
            if (!any) {
                minx = x0; maxx = x1; miny = y0; maxy = y1;
                any = true;
            } else {
                if (x0 < minx) { minx = x0; }
                if (x1 > maxx) { maxx = x1; }
                if (y0 < miny) { miny = y0; }
                if (y1 > maxy) { maxy = y1; }
            }
        }
        CGSize adv = { 0.0, 0.0 };
        CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &g, &adv, 1);
        pen += adv.width;
    }
    m->width = (float)pen;
    // Bounding-box edges -> TextMetrics sign conventions (left/ascent positive
    // toward -x / +y of the origin).
    m->actual_left = any ? (float)(-minx) : 0.0f;
    m->actual_right = any ? (float)maxx : 0.0f;
    m->actual_ascent = any ? (float)maxy : 0.0f;
    m->actual_descent = any ? (float)(-miny) : 0.0f;
}

void cnvs_font_vmetrics(cnvs_font *f, float *ascent, float *descent) {
    *ascent = (float)CTFontGetAscent(f->font);
    *descent = (float)CTFontGetDescent(f->font);
}
