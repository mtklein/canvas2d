#pragma once

// Stub <ptrcheck.h> for the fuzz build ONLY.
//
// -fbounds-safety is an Apple-clang feature; the real <ptrcheck.h> lives in the
// Apple toolchain's resource dir.  The fuzz build uses Homebrew clang (via
// afl-clang-fast) for coverage instrumentation + ASan/UBSan, with the flag OFF,
// so the bounds annotations must expand to nothing -- exactly as they do in the
// `unsafe` variant under Apple clang.  This header is on the include path only
// for that build (fuzz/build.sh adds -Ifuzz/shim); the real ninja build never
// sees it and uses Apple's header.
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
