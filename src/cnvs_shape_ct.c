// Core Text shaping shim.  Built without -fbounds-safety (configure.py BOUNDARY_C)
// to bind the un-annotated CoreText headers; it shapes UTF-8 into glyph runs and
// copies each run into checked-owned cnvs_glyph_run arrays for the checked core.
// See docs/text-boundary.md.

#include "cnvs_shape.h"

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

// Glyph-outline walk (same shape as cnvs_font_ct.c): Core Text glyph space is y-up,
// baseline-relative; flip y, place at the pen origin, map user->device.
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
