// Core Text shaping shim.  Built without -fbounds-safety (configure.py BOUNDARY_C)
// to bind the un-annotated CoreText headers; it shapes UTF-8 into glyph runs and
// copies each run into checked-owned struct cnvs_glyph_run arrays for the checked core.
// See docs/text-boundary.md.

#include "cnvs_text.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <stdlib.h>
#include <string.h>

// UTF-8 bytes -> CFString.  The checked side hands every string across this
// boundary as a counted (bytes, len) slice -- never a NUL contract -- and
// CFStringCreateWithBytes takes exactly that, so the shim reads len bytes and
// not one more.  NULL on invalid UTF-8, like CFStringCreateWithCString was.
static CFStringRef str_from_bytes(char const *bytes, int len) {
    if (!bytes || len < 0) {
        return NULL;
    }
    return CFStringCreateWithBytes(NULL, (UInt8 const *)bytes, (CFIndex)len,
                                   kCFStringEncodingUTF8, false);
}

// Map the CSS 100..900 weight axis onto Core Text's kCTFontWeightTrait axis,
// [-1.0, 1.0] with 0.0 == regular.  The documented anchors -- 400 -> 0.0 (the
// system regular weight UIFontWeightRegular), 700 -> ~0.4 (UIFontWeightBold) --
// give a slope of 0.4/300 per CSS step on the 400..700 segment; the same slope
// extends linearly to either end (100 -> -0.4, 900 -> ~0.667), which stays
// inside the axis and lets the descriptor matcher pick the nearest real face or
// synthesize when none is near.
static CGFloat ct_weight_from_css(int css) {
    return (CGFloat)((double)(css - 400) * (0.4 / 300.0));
}

// A synthetic-oblique slant: the x-shear (per unit of y) used to fake italic
// when the matched face has no real italic.  0.25 is roughly the slope a typical
// oblique runs at; it matches the visual weight CTFontDrawGlyphs uses for its own
// synthetic obliques.  The shear rides the FONT MATRIX, so CTFontCreatePathForGlyph
// returns the skewed outline (the curves the Libian model records) -- not a
// raster-only effect.
#define CNVS_SYNTH_ITALIC_SHEAR 0.25

// Build a descriptor font for (family, size, weight, italic) through the family
// name plus a traits dictionary (the CSS weight on the CT weight axis, plus the
// italic symbolic trait when italic).  `matrix` is baked into the font (NULL ==
// identity).  The descriptor matcher resolves to the nearest real face of the
// family.  NULL when the descriptor can't be built.
static CTFontRef font_descriptor(CFStringRef family, CGFloat size,
                                 int weight, bool italic,
                                 CGAffineTransform const *matrix) {
    CGFloat const wt = ct_weight_from_css(weight);
    CTFontSymbolicTraits sym = italic ? kCTFontItalicTrait : 0;
    CFNumberRef wnum = CFNumberCreate(NULL, kCFNumberCGFloatType, &wt);
    CFNumberRef snum = CFNumberCreate(NULL, kCFNumberSInt32Type, &sym);
    CTFontRef out = NULL;
    if (wnum && snum) {
        CFStringRef tkeys[2] = { kCTFontWeightTrait, kCTFontSymbolicTrait };
        const void *tvals[2] = { wnum, snum };
        CFDictionaryRef traits = CFDictionaryCreate(NULL, (const void **)tkeys,
            tvals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (traits) {
            CFStringRef akeys[2] = { kCTFontFamilyNameAttribute,
                                     kCTFontTraitsAttribute };
            const void *avals[2] = { family, traits };
            CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void **)akeys,
                avals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if (attrs) {
                CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
                if (desc) {
                    out = CTFontCreateWithFontDescriptor(desc, size, matrix);
                    CFRelease(desc);
                }
                CFRelease(attrs);
            }
            CFRelease(traits);
        }
    }
    if (wnum) { CFRelease(wnum); }
    if (snum) { CFRelease(snum); }
    return out;
}

// Build the shaping/metrics font for (family, size, weight, italic).  The weight
// trait picks the nearest real face on the family's weight axis (a real bold is
// matched here, with its own heavier outlines).  For italic, the descriptor first
// tries to match a real italic face; when the resolved face is NOT actually
// italic (the family has none), the font is rebuilt with a slant baked into the
// FONT MATRIX so the outline is synthesized -- a real synthetic oblique that the
// curve-recording (Libian) model captures, rather than the regular face.  Bold
// without a real heavier face cannot be emboldened at the outline level (Core
// Text only synthesizes bold weight at raster time), so a synth-bold row falls
// back to the family's nearest real weight; the italic synthesis is what reads as
// "synthesized" in the outline model.  Falls back to CTFontCreateWithName when no
// descriptor can be built.  NULL on failure.
static CTFontRef font_with_traits(CFStringRef family, CGFloat size,
                                  int weight, bool italic) {
    CTFontRef out = font_descriptor(family, size, weight, italic, NULL);
    if (out && italic &&
        (CTFontGetSymbolicTraits(out) & kCTFontItalicTrait) == 0) {
        // The family had no real italic face: synthesize one via a slant matrix
        // so the recorded outline is actually oblique.
        CGAffineTransform m = { 1.0, 0.0, CNVS_SYNTH_ITALIC_SHEAR, 1.0, 0.0, 0.0 };
        CTFontRef synth = font_descriptor(family, size, weight, italic, &m);
        if (synth) {
            CFRelease(out);
            out = synth;
        }
    }
    if (!out) {
        out = CTFontCreateWithName(family, size, NULL);  // descriptor unavailable
    }
    return out;
}

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

void cnvs_shaped_free(struct cnvs_shaped *s) {
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
static bool copy_run(CTRunRef run, struct cnvs_glyph_run *dst) {
    CFIndex gc = CTRunGetGlyphCount(run);
    dst->count = (int)gc;
    dst->rtl = (CTRunGetStatus(run) & kCTRunStatusRightToLeft) != 0;
    // The run's font (font fallback): retain it so the opaque handle outlives the
    // CTLine, store it as void* for the checked core to pass back to the boundary.
    CFDictionaryRef ra = CTRunGetAttributes(run);
    CTFontRef rf = ra ? CFDictionaryGetValue(ra, kCTFontAttributeName) : NULL;
    dst->font = rf ? (void *)CFRetain(rf) : NULL;
    // Color (emoji) runs are flagged here, once, so the checked walks never need
    // a per-run boundary query; the live run resolves its interned name id later.
    dst->is_color = rf &&
        (CTFontGetSymbolicTraits(rf) & kCTFontTraitColorGlyphs) != 0;
    dst->name_id = -1;
    dst->glyph = malloc((size_t)gc * sizeof *dst->glyph);
    dst->xadv = malloc((size_t)gc * sizeof *dst->xadv);
    dst->cluster = malloc((size_t)gc * sizeof *dst->cluster);
    CGGlyph *tg = malloc((size_t)gc * sizeof *tg);
    CGSize *ta = malloc((size_t)gc * sizeof *ta);
    CFIndex *ti = malloc((size_t)gc * sizeof *ti);
    bool const ok = dst->glyph && dst->xadv && dst->cluster && tg && ta && ti;
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
    enum cnvs_glyph_verb *verb;
    cnvs_vec2 *pt;
    int vcap, pcap;
    int nv, np;       // true counts, kept past the caps so the caller can grow
    double to_units;  // CT point-size px -> font units
};

static void put(struct curves *c, enum cnvs_glyph_verb v, CGPoint const *p, int n) {
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
                       enum cnvs_glyph_verb *verb, int vcap,
                       cnvs_vec2 *pt, int pcap,
                       int *nverbs, int *npts, float *units_per_em) {
    *nverbs = 0;
    *npts = 0;
    *units_per_em = 0.0f;
    if (!font) {
        return;
    }
    unsigned const upem = CTFontGetUnitsPerEm((CTFontRef)font);
    double const size = CTFontGetSize((CTFontRef)font);
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

void *cnvs_font_resized(void *font, float size_px) {
    if (!font || size_px <= 0.0f) {
        return NULL;
    }
    return (void *)CTFontCreateCopyWithAttributes((CTFontRef)font,
                                                  (CGFloat)size_px, NULL, NULL);
}

void cnvs_font_release(void *font) {
    if (font) {
        CFRelease(font);
    }
}

void cnvs_run_vmetrics(void const *font, float *asc1, float *desc1) {
    *asc1 = 0.0f;
    *desc1 = 0.0f;
    if (!font) {
        return;
    }
    double const size = CTFontGetSize((CTFontRef)font);
    if (size <= 0.0) {
        return;
    }
    *asc1 = (float)(CTFontGetAscent((CTFontRef)font) / size);
    *desc1 = (float)(CTFontGetDescent((CTFontRef)font) / size);
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

struct cnvs_shaped *cnvs_shape_text(char const *name, int name_len, float size_px, bool rtl,
                        int weight, bool italic, char const *text, int text_len) {
    CFStringRef cfname = str_from_bytes(name, name_len);
    CFStringRef str = str_from_bytes(text, text_len);
    CTFontRef font = cfname ? font_with_traits(cfname, size_px, weight, italic) : NULL;
    if (cfname) {
        CFRelease(cfname);
    }
    // The paragraph base writing direction: always explicit (never CT's
    // first-strong "natural" default), because the canvas direction attribute
    // -- not the text -- resolves the base level.  CTLine orders the runs and
    // resolves neutrals against it.
    int8_t const wd = rtl ? kCTWritingDirectionRightToLeft
                          : kCTWritingDirectionLeftToRight;
    CTParagraphStyleSetting const ps_set[1] = {
        { kCTParagraphStyleSpecifierBaseWritingDirection, sizeof wd, &wd },
    };
    CTParagraphStyleRef ps = CTParagraphStyleCreate(ps_set, 1);
    struct cnvs_shaped *out = NULL;
    if (font && str && ps) {
        CFStringRef keys[2] = { kCTFontAttributeName,
                                kCTParagraphStyleAttributeName };
        const void *vals[2] = { font, ps };
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void **)keys, vals, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef astr = CFAttributedStringCreate(NULL, str, attrs);
        CTLineRef line = CTLineCreateWithAttributedString(astr);
        CFArrayRef runs = CTLineGetGlyphRuns(line);
        CFIndex nruns = CFArrayGetCount(runs);

        out = calloc(1, sizeof *out);
        if (out) {
            out->utf16s = (int)CFStringGetLength(str);
            out->size_px = size_px;
            out->weight = weight;
            out->italic = italic;
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
    if (ps) {
        CFRelease(ps);
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

struct cnvs_font *cnvs_font(char const *name, int name_len, float size_px,
                            int weight, bool italic) {
    CFStringRef cfname = str_from_bytes(name, name_len);
    if (!cfname) {
        return NULL;
    }
    CTFontRef font = font_with_traits(cfname, size_px, weight, italic);
    CFRelease(cfname);
    if (!font) {
        return NULL;
    }
    struct cnvs_font *f = calloc(1, sizeof *f);
    if (!f) {
        CFRelease(font);
        return NULL;
    }
    f->font = font;
    return f;
}

void cnvs_font_free(struct cnvs_font *f) {
    if (!f) {
        return;
    }
    CFRelease(f->font);
    free(f);
}

void cnvs_font_vmetrics(struct cnvs_font *f, float *ascent, float *descent) {
    *ascent = (float)CTFontGetAscent(f->font);
    *descent = (float)CTFontGetDescent(f->font);
}
