#include "tagptr.h"
#include "test_util.h"

int main(void) {
    tnode a = { .value = 42 };
    tnode b = { .value = 99 };

    // Low-bit tag: recover both the tag and a usable, deref-able pointer.
    tagged la = tag_lo(&a, 2);
    tagged lb = tag_lo(&b, 3);
    CHECK(tag_lo_get(la) == 2 && tag_lo_get(lb) == 3);
    CHECK(ptr_lo(la) == &a && ptr_lo(lb) == &b);  // untag yields the original address
    CHECK(ptr_lo(la)->value == 42 && ptr_lo(lb)->value == 99);  // and it derefs

    // High-byte (TBI) tag: the recovered pointer keeps the tag bits, so it is NOT
    // equal to &a -- but the MMU ignores the top byte, so the deref still lands.
    tagged ha = tag_hi(&a, 0xAB);
    tnode *__single pa = ptr_hi(ha);
    tnode *__single base = &a;
    CHECK(tag_hi_get(ha) == 0xAB);
    CHECK((uintptr_t)pa != (uintptr_t)base);  // tag still present in the bits
#if !__has_feature(address_sanitizer)
    // On bare hardware TBI ignores the top byte and the deref lands.  Under
    // AddressSanitizer it does NOT: ASan derives the shadow address from the tagged
    // pointer, so the deref faults.  TBI tagging is unusable in the sanitizer build.
    CHECK(pa->value == 42);
#else
    (void)pa;
#endif

    return TEST_REPORT();
}
