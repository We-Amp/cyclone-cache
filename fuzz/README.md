# Fuzzing the read gauntlet

Coverage-guided [libFuzzer](https://llvm.org/docs/LibFuzzer.html) harnesses for
the paths that consume untrusted bytes: on-disk volume/directory
deserialization, document/alternate parsing, and the public C API. The
invariant every target enforces is the same — **adversarial bytes must produce
a clean miss / `Corrupted` / error, never a successful serve of garbage, a
crash, or undefined behaviour.**

## Targets

| Target | Consumes | What it exercises |
|--------|----------|-------------------|
| `fuzz_document_parse` | raw document bytes | `Document::deserialize`, `DocumentReader::{header,content,payload}`, checksum verify, and the 10-byte `DirEntry` accessors over arbitrary bit patterns. Asserts every returned span stays inside the input. |
| `fuzz_volume_open` | a whole volume file | overlays the fuzz bytes on the front of a real mmap-mode volume, then `Volume::open()` + reads — `VolumeHeader` parse, `MmapDirectory::open` (magic/version/`num_buckets` overflow guard), and `map_document()` on the read path. |
| `fuzz_c_api` | key + value byte streams | a persistent cache driven through `cyclone_cache_{write,read,read_data,exists,delete,write_tier,stats}` — the whole write→read stack under the sanitizers, including `CacheKey` construction and document round-trips. A shadow key→last-written-value map asserts every read HIT serves exactly the last committed bytes (evictions may turn a hit into a miss, never into a wrong hit). |

## Build & run (local)

Requires Clang (for `-fsanitize=fuzzer`). On Linux the sanitizer runtime works
out of the box; the CI lane uses `clang++-20`.

```bash
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCYCLONE_USE_BUNDLED_SHA256=ON \
  -DCYCLONE_BUILD_FUZZERS=ON \
  -DCYCLONE_BUILD_TESTS=OFF -DCYCLONE_BUILD_EXAMPLES=OFF -DCYCLONE_BUILD_BENCHMARKS=OFF
cmake --build build-fuzz -j"$(nproc)"

# seed the run from the checked-in corpus (copied so libFuzzer can extend it)
mkdir -p run && cp fuzz/corpus/fuzz_document_parse/* run/
ASAN_OPTIONS=abort_on_error=1 \
LSAN_OPTIONS=suppressions=tools/lsan_suppressions.txt \
  ./build-fuzz/fuzz/fuzz_document_parse -max_total_time=60 run
```

Reproduce a crasher: `./build-fuzz/fuzz/<target> <crash-file>`.

> **macOS note:** the llvm@21 ASan runtime deadlocks at process init on current
> macOS builds. Run the fuzzers on Linux (the CI target). The pure targets can
> still be built on macOS without `,address` for quick iteration.

## Seed corpus

`make_corpus` builds real, well-formed inputs (a valid volume with a few
entries + an alternate chain, documents built with `DocumentBuilder`, and
encoded C-API op sequences) so the fuzzer starts from valid structures. Seeds
are checked into `fuzz/corpus/<target>/` and kept to a few KB each.

The volume seed (and the in-memory template `fuzz_volume_open` overlays every
input onto) is normalized to a pure function of the build inputs — the two
wall-clock fields a fresh volume carries (`VolumeHeader::creation_time` and
each document's `last_access`) are zeroed by `volume_template.hpp`. That is
what makes `make_corpus` byte-for-byte idempotent and lets a crash artifact
honor libFuzzer's repro contract (`./fuzz_volume_open <crashfile>` rebuilds
the exact template the crashing run saw).

```bash
./build-fuzz/fuzz/make_corpus fuzz/corpus   # idempotent; refreshes the seeds
```

## Sanitizers

The whole library is instrumented with
`-fsanitize=fuzzer-no-link,address,undefined` (with the `vptr` carve-out,
mirroring the ASan CI job) — ASan covers heap-buffer-overflow /
use-after-free, UBSan covers signed overflow and misaligned access on
corrupted offsets. **LSan stays enabled**: `fuzz_c_api` tears its persistent
cache down via `atexit`, and runs use
`LSAN_OPTIONS=suppressions=tools/lsan_suppressions.txt` for the one
intentional shutdown-thread leak.

## CI

CI runs a 60s-per-target fuzz smoke on push/PR (fails on
crash / sanitizer error / slow-unit timeout) and a longer randomized run
nightly, uploading any crasher as an artifact. The nightly corpus persists
across runs via a per-target cache (prefix restore), so coverage
compounds; the checked-in seeds are the cold-start baseline. The same job carries the
randomized-schedule concurrency **stress lane** (`[nightly]` Catch2 tests in
`tests/integration/test_stress_nightly.cpp`).
