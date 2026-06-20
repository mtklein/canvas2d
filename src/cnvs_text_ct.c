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

// Map the nine fontStretch keywords (canvas_font_stretch 0..8, normal == 4) onto
// Core Text's kCTFontWidthTrait axis, [-1.0, 1.0] with 0.0 == normal width.  The
// keywords are evenly spaced four steps either side of normal, so each step is
// 0.25 of the axis -- ultra-condensed -1.0, normal 0.0, ultra-expanded 1.0.  The
// descriptor matcher then resolves the nearest real WIDTH face of the family (a
// distinct named face, so the glyph cache separates it) or leaves the regular
// face when none is near -- no width is synthesized.  STRETCH_NORMAL (4) gives
// exactly 0.0, so the default adds no width trait pull beyond what the family's
// regular face already is.
static CGFloat ct_width_from_stretch(int stretch) {
    return (CGFloat)((double)(stretch - 4) * 0.25);
}

// A synthetic-oblique slant: the x-shear (per unit of y) used to fake italic
// when the matched face has no real italic.  0.25 is roughly the slope a typical
// oblique runs at; it matches the visual weight CTFontDrawGlyphs uses for its own
// synthetic obliques.  The shear rides the FONT MATRIX, so CTFontCreatePathForGlyph
// returns the skewed outline (the curves the Libian model records) -- not a
// raster-only effect.
#define CNVS_SYNTH_ITALIC_SHEAR 0.25

// fontStretch NORMAL: the centre of the width axis (canvas_font_stretch 4), the
// value at which no width trait is added so the default face resolves exactly as
// it did before fontStretch existed (a width trait of 0.0 still nudges the
// matcher, so the default path must omit the trait entirely, not pass 0.0).
#define CNVS_STRETCH_NORMAL 4

// Build a descriptor font for (family, size, weight, italic, stretch) through the
// family name plus a traits dictionary (the CSS weight on the CT weight axis, the
// italic symbolic trait when italic, and the width trait when stretch is not
// NORMAL).  `matrix` is baked into the font (NULL == identity).  The descriptor
// matcher resolves to the nearest real face of the family.  NULL when the
// descriptor can't be built.
static CTFontRef font_descriptor(CFStringRef family, CGFloat size,
                                 int weight, bool italic, int stretch,
                                 CGAffineTransform const *matrix) {
    CGFloat const wt = ct_weight_from_css(weight);
    CGFloat const wd = ct_width_from_stretch(stretch);
    CTFontSymbolicTraits sym = italic ? kCTFontItalicTrait : 0;
    CFNumberRef wnum = CFNumberCreate(NULL, kCFNumberCGFloatType, &wt);
    CFNumberRef snum = CFNumberCreate(NULL, kCFNumberSInt32Type, &sym);
    // The width trait rides the traits dict only off the NORMAL default: a width
    // of 0.0 would still steer the matcher, so the default omits it entirely to
    // keep the resolved face byte-identical to the pre-fontStretch path.
    CFNumberRef dnum = stretch != CNVS_STRETCH_NORMAL
        ? CFNumberCreate(NULL, kCFNumberCGFloatType, &wd) : NULL;
    CTFontRef out = NULL;
    if (wnum && snum && (dnum || stretch == CNVS_STRETCH_NORMAL)) {
        CFStringRef tkeys[3] = { kCTFontWeightTrait, kCTFontSymbolicTrait };
        const void *tvals[3] = { wnum, snum };
        CFIndex nt = 2;
        if (dnum) { tkeys[nt] = kCTFontWidthTrait; tvals[nt] = dnum; nt++; }
        CFDictionaryRef traits = CFDictionaryCreate(NULL, (const void **)tkeys,
            tvals, nt, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
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
    if (dnum) { CFRelease(dnum); }
    return out;
}

// Build the shaping/metrics font for (family, size, weight, italic, stretch).
// The weight
// trait picks the nearest real face on the family's weight axis (a real bold is
// matched here, with its own heavier outlines); the width trait (when stretch is
// not NORMAL) picks the nearest real WIDTH face (a real condensed/expanded face,
// with its own distinct name -- the glyph cache separates it -- or a variable
// wdth instance).  For italic, the descriptor first
// tries to match a real italic face; when the resolved face is NOT actually
// italic (the family has none), the font is rebuilt with a slant baked into the
// FONT MATRIX so the outline is synthesized -- a real synthetic oblique that the
// curve-recording (Libian) model captures, rather than the regular face.  Bold
// without a real heavier face cannot be emboldened at the outline level (Core
// Text only synthesizes bold weight at raster time), so a synth-bold row falls
// back to the family's nearest real weight; likewise no width is synthesized, so
// a family with no width face falls back to its nearest face (stretch is then a
// no-op).  The italic synthesis is what reads as
// "synthesized" in the outline model.  Falls back to CTFontCreateWithName when no
// descriptor can be built.  NULL on failure.
static CTFontRef font_with_traits(CFStringRef family, CGFloat size,
                                  int weight, bool italic, int stretch) {
    CTFontRef out = font_descriptor(family, size, weight, italic, stretch, NULL);
    if (out && italic &&
        (CTFontGetSymbolicTraits(out) & kCTFontItalicTrait) == 0) {
        // The family had no real italic face: synthesize one via a slant matrix
        // so the recorded outline is actually oblique.
        CGAffineTransform m = { 1.0, 0.0, CNVS_SYNTH_ITALIC_SHEAR, 1.0, 0.0, 0.0 };
        CTFontRef synth = font_descriptor(family, size, weight, italic, stretch, &m);
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

// The shaping toggles, mapped onto Core Text attributes.  These mirror the
// public canvas_font_kerning / canvas_text_rendering enums (passed as ints to
// keep the enum spellings out of the boundary ABI):
//   - fontKerning NONE, and textRendering OPTIMIZE_SPEED, disable kerning by
//     setting kCTKernAttributeName = 0 on the run (Core Text then lays glyphs
//     at their unkerned advances).  AUTO/NORMAL (and the other textRendering
//     values) leave the attribute unset, so Core Text's default kerning applies.
//   - textRendering OPTIMIZE_SPEED additionally disables ligatures with
//     kCTLigatureAttributeName = 0 (a pragmatic mapping: Core Text exposes no
//     single "speed" knob, so the speed hint is spelled as no kerning + no
//     ligatures).  The other values leave ligatures at the default.
// The enum values must match canvas.h (AUTO=0/NORMAL=1/NONE=2 for kerning;
// AUTO=0/OPTIMIZE_SPEED=1 for rendering); the boundary keeps no canvas.h
// dependency, so they are spelled here as the documented integers.
enum { CT_KERNING_NONE = 2, CT_RENDERING_OPTIMIZE_SPEED = 1 };

// canvas_font_variant_caps values (kept off the boundary ABI like the others):
// NORMAL == 0 (no feature), SMALL_CAPS == 1 (smcp), ALL_SMALL_CAPS == 2 (smcp +
// c2sc).
enum { CT_VARIANT_NORMAL = 0, CT_VARIANT_SMALL_CAPS = 1, CT_VARIANT_ALL_SMALL_CAPS = 2 };

// Append `font` with the small-cap OpenType features for `variant_caps` baked in:
// smcp (lowercase -> small caps) for SMALL_CAPS, smcp + c2sc (uppercase too) for
// ALL_SMALL_CAPS.  Returns a NEW retained font (the caller releases the original
// on success); NORMAL or any failure returns NULL, so the caller keeps the
// unfeatured font.  The features ride the font's descriptor
// (kCTFontFeatureSettingsAttribute, OpenType tag form), so the shaper substitutes
// the small-cap glyphs within the SAME resolved face -- a no-op on a font that
// has neither feature (Core Text leaves the unfeatured glyphs).
static CTFontRef font_with_smallcaps(CTFontRef font, int variant_caps) {
    if (!font || variant_caps == CT_VARIANT_NORMAL) {
        return NULL;
    }
    // One feature dict per tag: { kCTFontOpenTypeFeatureTag: "smcp"/"c2sc",
    // kCTFontOpenTypeFeatureValue: 1 }.  ALL_SMALL_CAPS adds c2sc to smcp.
    char const *tags[2] = { "smcp", NULL };
    int ntags = 1;
    if (variant_caps == CT_VARIANT_ALL_SMALL_CAPS) {
        tags[1] = "c2sc";
        ntags = 2;
    }
    int const one = 1;
    CFNumberRef onenum = CFNumberCreate(NULL, kCFNumberIntType, &one);
    CFMutableArrayRef feats = CFArrayCreateMutable(NULL, ntags, &kCFTypeArrayCallBacks);
    CTFontRef out = NULL;
    if (onenum && feats) {
        for (int i = 0; i < ntags; i++) {
            CFStringRef tag = CFStringCreateWithCString(NULL, tags[i],
                                                        kCFStringEncodingASCII);
            if (!tag) {
                continue;
            }
            CFStringRef fkeys[2] = { kCTFontOpenTypeFeatureTag,
                                     kCTFontOpenTypeFeatureValue };
            const void *fvals[2] = { tag, onenum };
            CFDictionaryRef one_feat = CFDictionaryCreate(NULL, (const void **)fkeys,
                fvals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            if (one_feat) {
                CFArrayAppendValue(feats, one_feat);
                CFRelease(one_feat);
            }
            CFRelease(tag);
        }
        CFStringRef akeys[1] = { kCTFontFeatureSettingsAttribute };
        const void *avals[1] = { feats };
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void **)akeys,
            avals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        if (attrs) {
            CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes(attrs);
            if (desc) {
                out = CTFontCreateCopyWithAttributes(font, CTFontGetSize(font), NULL, desc);
                CFRelease(desc);
            }
            CFRelease(attrs);
        }
    }
    if (onenum) { CFRelease(onenum); }
    if (feats) { CFRelease(feats); }
    return out;
}

struct cnvs_shaped *cnvs_shape_text(char const *name, int name_len, float size_px, bool rtl,
                        int weight, bool italic, int kerning, int rendering,
                        int variant_caps, int stretch,
                        char const *lang, int lang_len, char const *text, int text_len) {
    CFStringRef cfname = str_from_bytes(name, name_len);
    CFStringRef str = str_from_bytes(text, text_len);
    CTFontRef font = cfname ? font_with_traits(cfname, size_px, weight, italic, stretch)
                            : NULL;
    if (cfname) {
        CFRelease(cfname);
    }
    // fontVariantCaps: re-copy the resolved width/weight/style face with the
    // small-cap features baked into its descriptor, so the shaper substitutes
    // small-cap glyphs within that face (a no-op on a face without smcp/c2sc).
    if (font) {
        CTFontRef sc = font_with_smallcaps(font, variant_caps);
        if (sc) {
            CFRelease(font);
            font = sc;
        }
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
    // The shaping toggles' attributes, appended to the always-present font +
    // paragraph-style pair: kerning 0 (off), ligature 0 (off), and the lang tag.
    bool const kern_off = kerning == CT_KERNING_NONE ||
                          rendering == CT_RENDERING_OPTIMIZE_SPEED;
    bool const liga_off = rendering == CT_RENDERING_OPTIMIZE_SPEED;
    int const zero = 0;
    CFNumberRef kern0 = kern_off ? CFNumberCreate(NULL, kCFNumberIntType, &zero) : NULL;
    CFNumberRef liga0 = liga_off ? CFNumberCreate(NULL, kCFNumberIntType, &zero) : NULL;
    CFStringRef cflang = (lang_len > 0) ? str_from_bytes(lang, lang_len) : NULL;
    struct cnvs_shaped *out = NULL;
    if (font && str && ps) {
        CFStringRef keys[5] = { kCTFontAttributeName,
                                kCTParagraphStyleAttributeName };
        const void *vals[5] = { font, ps };
        CFIndex na = 2;
        if (kern0)   { keys[na] = kCTKernAttributeName;     vals[na] = kern0;   na++; }
        if (liga0)   { keys[na] = kCTLigatureAttributeName; vals[na] = liga0;   na++; }
        if (cflang)  { keys[na] = kCTLanguageAttributeName; vals[na] = cflang;  na++; }
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, (const void **)keys, vals, na,
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
    if (kern0) {
        CFRelease(kern0);
    }
    if (liga0) {
        CFRelease(liga0);
    }
    if (cflang) {
        CFRelease(cflang);
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
                            int weight, bool italic, int stretch) {
    CFStringRef cfname = str_from_bytes(name, name_len);
    if (!cfname) {
        return NULL;
    }
    CTFontRef font = font_with_traits(cfname, size_px, weight, italic, stretch);
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
