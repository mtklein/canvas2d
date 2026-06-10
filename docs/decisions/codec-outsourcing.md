All reading done — codec sources, docs, gate, fuzz harnesses, commit history, and the record/replay serialization path. One premise in the tasking needed verification and turned out to be slightly ahead of the code, which I flag in the memo. Here it is.

---

# Memo: Should canvas2d outsource PNG/deflate to Apple's frameworks?

**Scope read:** `README.md`, `docs/bounds-safety.md`, `docs/roadmap.md`, `docs/text-boundary.md`, `src/cnvs_zlib.{h,c}` (34 + 654 lines), `src/cnvs_png.{h,c}` (48 + 450 lines), `.github/workflows/gate.yml`, commits `7f09496`, `46481a7`, `f122cdd`, `772d781`, `ee91fc1`, the fuzz harnesses (`fuzz/fuzz_inflate.c` 76 lines, `fuzz/fuzz_pngdec.c` 89, `fuzz/fuzz_png.c` 65), tests (`tests/test_zlib.c` 677, `test_png.c` 494, `test_pngload.c` 140), and the existing shims for size comparison (`src/cnvs_text_ct.c` 311 lines, `src/compositor_metal.m` 398).

**One premise correction before anything else.** The tasking says the deflate also serves ".canvas files embed compressed emoji captures." It does not — yet. `grep` shows `cnvs_zlib` has exactly one consumer pair: `cnvs_png.c` (encode and decode). Emoji captures in `.canvas` files are serialized as **raw base64** `bits` lines (`src/cnvs_record.c:134-146`, ~137 KB per 160×160 capture). The deterministic-replay argument for in-house deflate is therefore *prospective*: it binds only if/when the format compresses those blocks. I treat it that way below rather than letting it carry weight it hasn't earned.

---

## 1. The steelmanned case FOR outsourcing

Taken seriously, not as a strawman:

**a. You'd delete ~2,700 lines and the obligation to be a codec vendor.**
Outsourcing removes 1,186 lines of codec source, 1,311 lines of tests, 230 lines of fuzz harness plus two seed corpora (20 KB), and the standing maintenance duty those imply. The replacement shim is genuinely small: ImageIO is a C API (CF-based, like Core Text), so a decode-side binding is `CGImageSourceCreateWithData` → `CGImageSourceCreateImageAtIndex` → draw into a `CGBitmapContext` → copy out; encode is `CGImageDestinationCreateWithData` plus a properties dict. A both-directions shim plus CF lifetime discipline plausibly lands at 150–250 lines — *smaller* than the Core Text shim (311) it would sit beside, because there's no callback walk and no per-glyph protocol. A raw-deflate binding via system libz is smaller still: `compress2`/`uncompress` are one-shot memory-to-memory calls, maybe 40 lines behind a bounds-safe header. Net: roughly minus 2,700 lines, plus ~300, and the deleted lines include the single hardest-to-review module in the tree (a Huffman decoder).

**b. Apple patches their codec; you patch yours.**
ImageIO has a real CVE history (most famously the 2023 BLASTPASS chain), but the flip side is that those CVEs got *patched, by Apple, on every user's machine, without the app shipping anything*. Your 654-line inflate is patched exactly when you find its bugs. A 6-minute, 3.3M-exec fuzz campaign (`46481a7`) is honest hygiene but it is not Google-scale continuous fuzzing of libz, which the system library effectively gets. If a subtle inflate bug exists, the in-house path owns it forever.

**c. The performance row you're worst at simply vanishes.**
`f122cdd` is candid: bench_png went 5.8 ms → ~140 ms release (65 ms unsafe), making PNG encode the project's worst bounds-check ratio at **2.1×** and dragging end-to-end `bench` to 1.58×. System libz's assembly-tuned deflate (or libcompression) would do this work several times faster than even the *unsafe* in-house build, and with dynamic-Huffman + lazy matching would compress better than the fixed-Huffman greedy encoder — smaller committed PNGs in a repo that commits 32 of them on every rendering change. The "matcher is the next vectorization target" work item disappears instead of needing to be done.

**d. The thesis can arguably be served by binding, not writing.**
`docs/bounds-safety.md`'s "adoption asymmetry" section already argues that binding un-annotated C system headers from checked code is itself lesson material. A third boundary shim (ImageIO/libz behind a bounds-safe ABI) would be a new data point in exactly that study: zlib.h's `z_stream` with its unsafe `next_in`/`next_out` pointers is a different boundary *shape* than Core Text's opaque handles or Metal's tiles. The project's architecture diagram would stay honest: three shims, each behind a checked ABI.

**e. It's the only path to ever decoding the rest of the PNG universe.**
`canvas_load_png` rejects palette, gray, 16-bit, interlace, Sub/Avg/Paeth by design. If `drawImage`-from-arbitrary-file is ever wanted, ImageIO decode is the natural provider; nobody should hand-write Adam7 deinterlacing for a learning project about bounds checks.

## 2. The honest case against

**a. Determinism is load-bearing, and outsourcing the encoder breaks it at the foundation.**
This is the decisive dimension, and it's structural, not aesthetic:

- The 32 gallery PNGs are **build outputs of every `ninja`**, committed in-tree, reviewed via `git diff` "in lockstep" with code (README Quick start). That workflow only works if *identical pixels → identical bytes*, which `cnvs_png.h` guarantees explicitly ("Deterministic: identical pixels -> identical PNG bytes, always"). CGImageDestination's PNG output is not contractually stable across OS versions (encoder heuristics, metadata chunks, embedded color-profile behavior all move), so after any macOS update every `ninja` could dirty all 32 PNGs with zero rendering change — turning the repo's primary review signal into noise.
- `gate.yml` byte-diffs the ten text-free scenes specifically to gate **codegen reproducibility** ("fails if CI's codegen draws even one different pixel"), with `DEVELOPER_DIR` pinning Xcode 26.5 for exactly that reason. With ImageIO encode, a byte diff could mean "a pixel changed" *or* "the runner image's ImageIO changed" — the gate's signal is destroyed unless rebuilt as decode-then-pixel-compare, which is more machinery, not less.
- System libz fares better than ImageIO but still fails the bar: deflate output is deterministic for a fixed version+level+strategy but **not stable across library versions**, and macOS's libz revs with the OS. The gate currently pins one thing (the compiler); raw-libz encode would silently add a second pinned dependency (the OS runtime) that no env var can pin.
- The `.canvas` arc has the same property prospectively: `tests/test_record` pins that replay-while-recording reproduces the file **byte-for-byte** (the drift guard, per `docs/bounds-safety.md`), and `docs/text-boundary.md`'s whole design goal is recordings that replay byte-identically across machines with no fonts installed. If capture compression lands, a system-libz deflate would make the *same session on two OS versions* record different bytes. The in-house deflate's determinism clause is the only thing that keeps "compressed" compatible with that test posture.

**b. The deleted code is the thesis, not overhead.**
The project's stated purpose (README points 1 and 2) is that *the interesting work lives in bounds-checked C*, with the from-scratch zlib/PNG codec named in the README's own mission sentence. `cnvs_zlib.c`'s header comment calls inflate "the project's premier untrusted-parser probe," and the module produced real findings: the arithmetic enough-codes check before any table is indexed, the advisory-link hash-chain design, the "bounds-safety is the net, not the handler" posture, and the 2.1× row itself — which `docs/bounds-safety.md` uses as the *converse* lesson of the blit story (scalar indexed work with nothing to hide checks behind). Even the "bad" benchmark number is curriculum. The two existing shims exist because their data is **outside the program** — GPU hardware, system font tables. PNG/deflate is pure computation over bytes you already own; it is precisely the category the thesis says checked C should do in-house. Outsourcing it doesn't trim the project, it concedes its central claim for the one module that's a classic exploit-class parser.

**c. The shim would be a worse boundary than the two it joins.**
The Metal shim is irreducible (the flag rejects ObjC); the Core Text shim was chosen after measuring that checked binding "buys no real safety" since the only growing buffer is checked-owned. An ImageIO shim inverts that calculus: the buffers ImageIO writes into and parses from are the *payload itself*, and the parsing — the thing bounds checks exist for — would happen inside an opaque framework, unchecked and un-sanitized (ASan can't see into ImageIO's own heap discipline the way it instruments `cnvs_text_ct.c`'s shim code). You'd trade 1,100 lines of checked, fuzzed, `-Weverything`-clean parsing for a boundary whose unsafe side does the dangerous work.

**d. ImageIO decode has a correctness trap the strict decoder doesn't.**
`canvas_load_png` must return the exact straight-alpha RGBA8 the encoder wrote (roadmap: "reads those files back, byte-exact"; `fuzz_pngdec`'s round-trip oracle asserts `memcmp` identity). The idiomatic ImageIO extraction path draws through a `CGBitmapContext` in **premultiplied** alpha (the only RGBA8 layouts CG supports — see `kCGImageAlphaPremultipliedLast` in `cnvs_text_ct.c:172`), and premultiply→unpremultiply is lossy at low alpha and destroys RGB entirely at alpha 0. Avoiding that means raw `CGImageGetDataProvider` access plus defending against color-management transforms — exactly the fiddly, version-sensitive code a shim was supposed to spare you. The emoji path tolerates premul; PNG round-trip identity cannot.

**e. Security posture, scoped honestly, favors in-house *for this threat model*.**
The decoder accepts only its own encoder's grammar — 8-bit RGBA, non-interlaced, None/Up, every CRC checked, dimensions capped at 16384 (`cnvs_png.h`). That's a deliberately tiny attack surface with `-fbounds-safety` underneath converting any missed check into a deterministic trap rather than corruption. Routing that through ImageIO replaces a ~1,100-line auditable surface with a multi-format framework whose content sniffing and shared parsing machinery vastly *widen* what a hostile file can reach. Apple's patch cadence is an argument for ImageIO when you must accept arbitrary internet images; it's an anti-argument when you currently accept almost nothing.

**f. The performance win is real but worthless here.**
Workload math: the whole 32-scene gallery is ~14 MB of raw filtered bytes (the pre-compression PNG total from `f122cdd`); at bench_png's measured ~150 MB/s checked throughput that's roughly **0.1 s added to a full `ninja`** and the same per CI run. The 2.1× ratio is a benchmark-table embarrassment, not a felt cost — and the project already has a named, in-paradigm fix (vectorize the matcher, the blit treatment) that is itself the kind of result the docs exist to publish.

## 3. Hybrid options, worked through

| Option | Determinism gates | Thesis | Verdict |
|---|---|---|---|
| **H1: Encode in-house, decode via ImageIO** | Safe — gates hash only encoder output | Deletes the premier untrusted-parser probe (inflate + `fuzz_inflate` + `fuzz_pngdec`) while keeping the boring half (the encoder is "the most-audited code in the tree" per `fuzz_png.c`, lower-yield) | Coherent but backwards: keeps the work, discards the lesson. Decode is also where the premul/byte-exactness trap lives. Only ~830 lines of the 2,700 actually leave. |
| **H2: Both in-house, system libz only for raw deflate inside PNG/.canvas** | Broken — committed PNG bytes (and prospectively `.canvas` bytes) become a function of the OS libz version; the `git diff` workflow and `gate.yml` byte-diff churn on OS updates | Removes the 2.1× row and the matcher vectorization target; keeps PNG framing + inflate | The smallest shim (~40 lines) but it hits the determinism gate dead-center for the *encode* direction, which is the direction the gates hash. Rejected. |
| **H3: System libz for *inflate* only; keep deterministic in-house deflate** | Safe — decompression of a valid stream is bit-exact by spec regardless of library version | Deletes the premier probe (the full Huffman decoder is the thesis artifact); keeps the slow part (deflate) so no perf win either | The only hybrid that survives the gates, and it's the worst trade on offer: all thesis cost, zero performance benefit. |
| **H4: Status quo + ImageIO decode added later, behind a separate API, *if* arbitrary-PNG import is ever wanted** | Safe — own-output round-trip stays on the strict decoder; foreign files go through the shim | Additive: the strict decoder remains the probe, the shim becomes a genuine third-boundary case study with real motivation | The right shape for the future. Costs nothing today. |
| **H5: Differential testing — keep in-house, use system zlib/ImageIO as an *oracle* in tests/fuzz** | N/A (test-only dependency) | Strengthens it — `7f09496` already cross-checked against reference zlib during development; promoting that to a permanent differential fuzz harness captures outsourcing's main safety benefit (Apple's correctness) without its costs | Cheap, recommended regardless. **Implemented 2026-06-10:** `tests/test_zlib_oracle.c` + `fuzz/fuzz_zlib_diff.c` (-lz on those two link edges only). |

## 4. Recommendation

**Keep the in-house codec. Adopt H5 now; hold H4 in reserve.** The tradeoffs that drove it, in order:

1. **Determinism is architecturally load-bearing, not a preference.** Three separate mechanisms — gallery-PNGs-as-reviewed-build-outputs, the `gate.yml` codegen byte-diff, and the recorded-program byte-for-byte drift guard — all assume *content → bytes* is a pure function. Every outsourcing variant that touches the encode path breaks at least one of them on the next OS or runner-image bump, and the repair (pixel-diff gates, golden hashes per OS) costs more machinery than the codec.
2. **The project's currency inverts normal accounting.** Elsewhere, 1,100 lines of parser is liability and a 200-line shim is relief. Here the parser is the deliverable — the README names the from-scratch codec in its mission statement, and the 2.1× row plus the strict-inflate design are the field-report's freshest material. The principled boundary rule the repo already follows is *shims for capabilities, in-house for algorithms*: Metal and Core Text cross because hardware and font data live outside the program; deflate is arithmetic.
3. **Threat-model-scoped security favors small-and-checked.** A strict, fuzzed, bounds-checked decoder that accepts only its own output is a smaller real surface than ImageIO, whichever side has more CVE-fixing headcount.
4. **The honest costs of staying are payable in-paradigm:** the 2.1× matcher has a named vectorization plan; correctness assurance can be rented from Apple via differential testing (H5) without shipping their bytes; and with zero users, reversal in either direction remains cheap, so deferring H4 forfeits nothing.

Honest residue conceded to the outsourcing side: the team now owns a compressor forever, including inflate paths its own encoder never emits (dynamic-Huffman decode exists purely as probe material); the fixed-Huffman greedy encoder leaves ~10–30% compression on the table versus real zlib; and "Apple patches it for free" is a genuine ongoing benefit the in-house path permanently declines.

## 5. What would change my mind

- **The project gains real users or accepts foreign PNGs** — untrusted *arbitrary* input shifts the security calculus toward Apple's patched, sandboxable codec (do H4, decode-only, separate API).
- **The byte-identity gates are retired anyway** (e.g., CI moves to pixel-diff for unrelated reasons) — determinism stops being load-bearing and item 1 collapses.
- **The matcher vectorization fails** — if the "blit treatment" can't move 2.1× meaningfully, the row becomes a standing counterexample to the thesis instead of a work item, and a fast unchecked boundary starts looking like the honest answer the docs would have to write up anyway.
- **`.canvas` capture compression lands and file sizes actually matter** at a scale where the in-house encoder's compression deficit (vs zlib -9) is felt — though determinism still rules out system deflate for the *committed* bytes; the pressure would instead argue for improving the in-house encoder.
- **Apple ships a determinism contract** (a documented stable-bytes mode for libcompression/ImageIO PNG) — the decisive objection to H2 evaporates and the 40-line libz shim becomes genuinely attractive.
- **Differential fuzzing (H5) finds a cluster of real inflate bugs** — that would be evidence the 654 lines are not as audit-complete as the clean campaign suggested, and would re-weight the "who patches what" argument.