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

// Canonical-curve walk: translate CGPath elements into the caller's verb + point
// arrays, converting to font units.  Core Text emits CGPathApply points at the
// font's point size (y up, baseline-relative), so multiply by units-per-em / size
// to land on the font's design grid.  No pen, no transform, no flattening -- those
// run in the checked core now (cnvs_glyph_outline in cnvs_text.c).
struct curves {
    cnvs_glyph_verb *verb;
    cnvs_vec2 *pt;
    int vcap, pcap;
    int nv, np;       // true counts, kept past the caps so the caller can grow
    double to_units;  // CT point-size px -> font units
};

static void put(struct curves *c, cnvs_glyph_verb v, CGPoint const *p, int n) {
    if (c->nv < c->vcap) {
        c->verb[c->nv] = v;
    }
    c->nv++;
    for (int i = 0; i < n; i++) {
        if (c->np < c->pcap) {
            c->pt[c->np] = (cnvs_vec2){ (float)(p[i].x * c->to_units),
                                        (float)(p[i].y * c->to_units) };
        }
        c->np++;
    }
}

static void emit(void *info, CGPathElement const *e) {
    struct curves *c = info;
    switch (e->type) {
        case kCGPathElementMoveToPoint:
            put(c, CNVS_GLYPH_MOVE, e->points, 1);
            break;
        case kCGPathElementAddLineToPoint:
            put(c, CNVS_GLYPH_LINE, e->points, 1);
            break;
        case kCGPathElementAddQuadCurveToPoint:
            put(c, CNVS_GLYPH_QUAD, e->points, 2);
            break;
        case kCGPathElementAddCurveToPoint:
            put(c, CNVS_GLYPH_CUBIC, e->points, 3);
            break;
        case kCGPathElementCloseSubpath:
            put(c, CNVS_GLYPH_CLOSE, e->points, 0);
            break;
    }
}

void cnvs_glyph_curves(void *font, uint16_t glyph,
                       cnvs_glyph_verb *verb, int vcap,
                       cnvs_vec2 *pt, int pcap,
                       int *nverbs, int *npts, float *units_per_em) {
    *nverbs = 0;
    *npts = 0;
    *units_per_em = 0.0f;
    if (!font) {
        return;
    }
    unsigned upem = CTFontGetUnitsPerEm((CTFontRef)font);
    double size = CTFontGetSize((CTFontRef)font);
    if (upem == 0 || size <= 0.0) {
        return;
    }
    *units_per_em = (float)upem;
    CGPathRef path = CTFontCreatePathForGlyph((CTFontRef)font, (CGGlyph)glyph, NULL);
    if (path) {  // NULL for blanks and for color glyphs (emoji) -- no outline
        struct curves c = { .verb = verb, .pt = pt, .vcap = vcap, .pcap = pcap,
                            .to_units = (double)upem / size };
        CGPathApply(path, &c, emit);
        CGPathRelease(path);
        *nverbs = c.nv;
        *npts = c.np;
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
            out->size_px = size_px;
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

// Full TextMetrics for a shaped line: font-wide metrics from `primary`, width and
// the actual (ink) box from the shaped runs -- each glyph's tight rect measured in
// its own (possibly fallback) font and offset by the running pen.
void cnvs_shaped_metrics(cnvs_shaped const *s, cnvs_font *primary, cnvs_text_metrics *m) {
    memset(m, 0, sizeof *m);
    if (!primary) {
        return;
    }
    CTFontRef font = primary->font;
    double ascent = CTFontGetAscent(font);
    double descent = CTFontGetDescent(font);
    double size = CTFontGetSize(font);
    m->font_ascent = (float)ascent;
    m->font_descent = (float)descent;
    // Split the em square (height == size) by the ascent/descent ratio.
    double denom = ascent + descent;
    double em_asc = denom > 0.0 ? size * ascent / denom : size;
    m->em_ascent = (float)em_asc;
    m->em_descent = (float)(size - em_asc);
    m->alphabetic_baseline = 0.0f;
    m->hanging_baseline = (float)ascent;        // ~top of the ascenders
    m->ideographic_baseline = -(float)descent;  // ~bottom of the descenders
    if (!s) {
        return;
    }
    // Walk the shaped runs, summing advances and unioning each glyph's tight rect
    // (its run's font, glyph space y up, baseline at 0) offset by the running pen.
    double pen = 0.0;
    bool any = false;
    double minx = 0.0, maxx = 0.0, miny = 0.0, maxy = 0.0;
    for (int r = 0; r < s->nruns; r++) {
        cnvs_glyph_run run = s->run[r];
        CTFontRef rf = (CTFontRef)run.font;
        for (int i = 0; i < run.count; i++) {
            CGGlyph g = (CGGlyph)run.glyph[i];
            CGRect rr = CTFontGetBoundingRectsForGlyphs(rf, kCTFontOrientationHorizontal,
                                                        &g, NULL, 1);
            if (!CGRectIsNull(rr) && !CGRectIsEmpty(rr)) {
                double x0 = pen + rr.origin.x;
                double x1 = x0 + rr.size.width;
                double y0 = rr.origin.y;
                double y1 = y0 + rr.size.height;
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
            pen += run.xadv[i];
        }
    }
    m->width = (float)pen;
    m->actual_left = any ? (float)(-minx) : 0.0f;
    m->actual_right = any ? (float)maxx : 0.0f;
    m->actual_ascent = any ? (float)maxy : 0.0f;
    m->actual_descent = any ? (float)(-miny) : 0.0f;
}

void cnvs_font_vmetrics(cnvs_font *f, float *ascent, float *descent) {
    *ascent = (float)CTFontGetAscent(f->font);
    *descent = (float)CTFontGetDescent(f->font);
}
