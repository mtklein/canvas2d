#pragma once

// Pointer bit-stealing under -fbounds-safety: stash a small tag in a pointer's
// spare bits and recover it.  The round-trip through uintptr_t drops the bound (the
// pointer comes back __unsafe_indexable), so recovering a usable pointer needs a
// forge -- bit-stealing is a deliberate step outside the checked domain, the
// soft-contract cousin of the hard fault a CHERI capability would take on a poked
// tag.  Two places to hide a tag: the low alignment bits, and -- on arm64, where the
// MMU's Top-Byte-Ignore drops bits 56-63 on access -- the top byte.
//
// static inline because these are one or two instructions each; out-of-lining a
// pointer OR would be absurd.  The checked alternative to all of this is simply a
// separate tag field next to a __counted_by/__single pointer: no forge, one extra
// word.

#include <ptrcheck.h>
#include <stdint.h>

typedef struct { int value; } tnode;  // >= 4-aligned, so low bits 0-1 are free
typedef uintptr_t tagged;

// Low (alignment) bits: a 2-bit tag.
static inline tagged tag_lo(tnode *__single p, unsigned tag) {
    return (uintptr_t)p | (tag & 3u);
}
static inline unsigned tag_lo_get(tagged t) {
    return (unsigned)(t & 3u);
}
static inline tnode *__single ptr_lo(tagged t) {
    // Untag, then re-assert "points to one tnode" -- the bound the int cast lost.
    return __unsafe_forge_single(tnode *, (void *)(t & ~(uintptr_t)3));
}

// High byte (arm64 TBI): an 8-bit tag the hardware ignores on access.
static inline tagged tag_hi(tnode *__single p, unsigned tag) {
    return (uintptr_t)p | ((uintptr_t)(tag & 0xFFu) << 56);
}
static inline unsigned tag_hi_get(tagged t) {
    return (unsigned)((t >> 56) & 0xFFu);
}
static inline tnode *__single ptr_hi(tagged t) {
    // Keep the tag bits; the MMU strips the top byte on deref.  Forge around the
    // tagged address so the bound stays consistent with what we actually access.
    return __unsafe_forge_single(tnode *, (void *)t);
}
