#pragma once

// Stub <ptrcheck.h> for the fuzz build ONLY.
//
// -fbounds-safety is an Apple-clang feature; the real <ptrcheck.h> lives in the
// Apple toolchain's resource dir.  The fuzz build (`ninja fuzz`) uses Homebrew
// clang for coverage instrumentation + ASan/UBSan, with the flag OFF, so the
// bounds annotations must expand to nothing -- exactly as they do in the
// `unsafe` variant under Apple clang.  This header is on the include path only
// for `ninja fuzz` (its cc_fuzz rule adds -Ifuzz/shim); the main build uses Apple
// clang and its real header.
//
// Pointing -I at Apple's resource dir instead would drag in a *different* clang
// version's builtin headers (stdint.h, ...) and break the compile, so we shim
// just this one header.

#define __counted_by(N)
#define __counted_by_or_null(N)
#define __sized_by(N)
#define __sized_by_or_null(N)
#define __single
#define __bidi_indexable
#define __indexable
#define __unsafe_indexable
#define __null_terminated
#define __terminated_by(E)
#define __null_terminated_by(E)
#define __ptrcheck_abi_assume_single()
#define __ptrcheck_abi_assume_unsafe_indexable()

// Forges / conversions: plain casts or identities when bounds-safety is off.
#define __unsafe_forge_single(T, P) ((T)(P))
#define __unsafe_forge_bidi_indexable(T, P, S) ((T)(P))
#define __unsafe_forge_null_terminated(T, P) ((T)(P))
#define __unsafe_forge_terminated_by(T, P, E) ((T)(P))
#define __null_terminated_to_indexable(P) (P)
#define __unsafe_null_terminated_from_indexable(P) (P)
