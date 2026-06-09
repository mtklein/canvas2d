// Core Text glyph-outline shim.  Built without -fbounds-safety (configure.py
// BOUNDARY_C) to bind the un-annotated CoreText/CoreGraphics headers as plain C; it
// hands the checked core finished device-space cnvs_paths.  See docs/bounds-safety.md.

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
