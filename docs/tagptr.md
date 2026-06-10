# Pointer bit-stealing under `-fbounds-safety`

Stuffing a few bits of metadata into a pointer's spare bits is a classic reflex —
low (alignment) bits, or, on arm64, the top byte the MMU ignores (Top-Byte-Ignore).
The question for `-fbounds-safety` is whether the bound survives the bit-twiddling,
and how it compares to capability hardware like CHERI that treats a poked pointer as
invalid. `tagptr.h` (since retired — see the epilogue) was a small demonstrator (a
2-bit low tag and an 8-bit TBI tag on a pointer-to-object); `test_tagptr.c` exercised
both.

This isn't a performance axis — bit-stealing is about saving a word, and the cost it
incurs under the flag isn't cycles, it's *checking*. So there's no benchmark here,
just what compiles, what runs, and what it gives up.

## The round-trip drops the bound — bit-stealing forces a forge

To tag a pointer you go through an integer:

```c
uintptr_t t = (uintptr_t)p | tag;     // ok: pointer -> integer
int *q = (int *)(t & ~mask);          // ERROR
//   initializing 'int *__bidi_indexable' with 'int *__unsafe_indexable' casts away
//   '__unsafe_indexable'; use __unsafe_forge_single / __unsafe_forge_bidi_indexable
```

An integer carries no bound, so casting back yields an `__unsafe_indexable` pointer,
and the flag refuses to silently promote it to something you can index or deref. You
must say so explicitly with a forge:

```c
int *__single q = __unsafe_forge_single(int *, (void *)(t & ~mask));   // I assert: one int
```

That is the whole story in one line: **bit-stealing is a deliberate step out of the
checked domain.** It is *not* a hard fault the way CHERI invalidates a capability when
you disturb its tag bit — under `-fbounds-safety` the pointer is still a plain machine
pointer and the bound is separate metadata you re-assert by hand. So it works, but the
forge is you signing for the bound the compiler can no longer vouch for.

The checked alternative needs no forge at all: keep the pointer `__single`/
`__counted_by` and put the tag in a **separate field**. The only cost is one extra
word — which is exactly the word bit-stealing was trying to save.

## Both placements work — forge around the base you actually index

With the forge, both tricks run correctly:

- **Low (alignment) bits.** Untag first, then forge the bound around the *real* base.
- **High byte (TBI).** Forge the bound around the *tagged* base and deref it directly;
  the MMU drops the top byte on access. The bound is expressed relative to the tagged
  address, so the bounds-check arithmetic stays consistent.

The subtlety is that last point: forge around the base you actually dereference. Forge
the bound around the untagged base but then deref the *tagged* pointer and the
bounds check false-traps — the tagged address is numerically far past the bound.

## TBI tagging breaks AddressSanitizer

The sharp practical finding: the TBI top-byte tag works under bare `-fbounds-safety`
(the `release` build derefs it fine) but **faults under AddressSanitizer** (the
`debug` build). The MMU ignores the top byte, but ASan doesn't — it computes the
shadow-memory address from the *tagged* pointer, so the shadow lookup lands in the
weeds and SEGVs. `test_tagptr` guards the TBI deref under
`__has_feature(address_sanitizer)` for that reason.

So in a project that runs ASan in its debug build (this one does), high-byte pointer
tagging is effectively off the table: it would pass release and crash every
sanitized run. Low-bit tagging is unaffected — the masked address is a real address,
so ASan's shadow math is correct. (HWASan is the sanitizer that *does* understand tag
bits, by design; plain ASan does not.)

## Takeaway

Bit-stealing is the one trick in this exploration that genuinely costs you something
under the flag: a forge (you leave the checked domain at the untag site), and — for
the TBI variant — sanitizer compatibility. None of the earlier register-residency
wins (channels, `vtbl` tables) needed a forge; they stayed fully checked. If you want
to play strictly by the rules, the separate-tag-field spelling is free of all of
this and costs one word. Bit-stealing is where "by the rules" and "the fast/compact
reflex" finally diverge.

## Epilogue: the probe is retired

The demonstrator (`tagptr.h` and `test_tagptr.c`) was a self-contained probe, never
wired into the renderer — and the *only* legitimate `__unsafe_forge` user in the
checked tree. With its question answered (bit-stealing forces a forge, and the TBI
variant breaks ASan), the code has been retired from the tree. Its departure leaves
the checked sources forge-free, which a default-build gate now enforces: a bare
`ninja` greps `src include tests examples` and fails if any `__unsafe_` escape hatch
reappears. The finding stands without the code; git history holds the module.
