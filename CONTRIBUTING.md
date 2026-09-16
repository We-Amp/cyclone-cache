# Contributing to Cyclone Cache

Thank you for your interest in contributing to Cyclone Cache!

## Development Setup

### Prerequisites

- CMake 3.20 or later
- C++23 compatible compiler:
  - GCC 13 or later
  - Clang 16 or later
  - MSVC 2022 or later
- OpenSSL development headers (or build with `-DCYCLONE_USE_BUNDLED_SHA256=ON`)

The lint gate pins LLVM 20 (`clang++-20`, `clang-tidy-20`, `clang-format-20`).
To reproduce it locally you need LLVM 20 specifically — see CLAUDE.md's
"Linting (required gate)" section for the exact commands.

### Building

```bash
# Clone and build
git clone https://github.com/We-Amp/cyclone-cache
cd cyclone-cache
cmake -B build
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Run benchmarks
./build/cache_benchmark 50 500 2048
./build/performance_baseline
```

### Pre-commit hooks

```bash
pip install pre-commit && pre-commit install
```

The hooks (`.pre-commit-config.yaml`) run `clang-format` (pinned to v20.1.8 to
match the CI clang-format-20 gate) and enforce the required Apache-2.0 SPDX header
on every `.cpp`/`.hpp`/`.h`. `pre-commit install` now wires up **both** a
commit-time hook and a **pre-push** clang-format gate, so a divergently
formatted commit is caught before it leaves your machine.

#### One entry point: `tools/format.sh`

`tools/format.sh` is the single, CI-faithful formatter. It resolves a
clang-format **20.x** binary — a local `clang-format-20`, else a pinned
`clang-format==20.1.8` pip wheel it bootstraps into a shared cache venv (the
same wheel `mirrors-clang-format` uses; verified byte-identical to the CI
`clang-format-20`) — and formats/checks exactly the scope CI lints.

```bash
tools/format.sh                    # format in place (whole CI scope)
tools/format.sh --check            # verify only, like CI (clang-format --dry-run --Werror)
tools/format.sh --check --changed  # fast changed-files check (what the pre-push hook runs)
```

**Rule:** if your `clang-format` is not **20.x** (a Homebrew `llvm` typically
ships a newer major that reformats differently), do **not** run it directly —
run `tools/format.sh`, which always uses the pinned version. No remote formatter
box is required.

### License audit

CI also runs [Apache RAT](https://creadur.apache.org/rat/) over the whole tree
(the `License audit (Apache RAT)` job) and fails on any file without a recognised
license. Every file must carry the Apache-2.0 SPDX header, or be listed with a
reason in `.rat-excludes` (tool configuration, Markdown documentation, fuzz seed
corpora). RAT classifies `.json` and other data/image extensions as binary and
never audits them, so a foreign license in such a file is invisible to the gate.
Reproduce locally with `bash tools/ci/rat.sh` (needs Java 8 or newer and curl).

## Code Style

### C++ Guidelines

- **Standard**: C++23 (no features from later standards)
- **Style**: Google-based, enforced by `.clang-format` (`BasedOnStyle: Google`).
  `.clang-format` is the single source of truth; CI rejects non-conforming code
  via `clang-format-20 --dry-run --Werror`.
- **Indentation**: 2 spaces, no tabs
- **Line length**: 80 columns (enforced by `.clang-format`, `ColumnLimit: 80`)
- **Braces**: Opening brace on same line
- **Naming**:
  - Classes: `CamelCase` (e.g., `CacheKey`, `WriteHandle`)
  - Functions/methods: `snake_case` (e.g., `read_sync`, `bucket_hash`)
  - Member variables: `_prefix` (e.g., `_mutex`, `_entries`)
  - Constants: `kCamelCase` (e.g., `kHeaderSize`, `kMaxChainDepth`)

### Error Handling

Always use `std::expected<T, CacheError>` for operations that can fail:

```cpp
// Good
std::expected<void, CacheError> do_something()
{
  if (error_condition) {
    return std::unexpected(CacheError::InvalidArgument);
  }
  return {};
}

// Bad - don't use exceptions
void do_something()
{
  if (error_condition) {
    throw std::runtime_error("error");  // NO!
  }
}
```

### Memory Safety

- Use RAII for resource management
- Prefer `std::unique_ptr` and `std::shared_ptr`
- Validate sizes before allocation
- Check for integer overflow in size calculations
- Add bounds checking for deserialization

### Performance

- Avoid allocations in hot paths
- The read hot path is lock-free — don't add locks (or writes to shared cache
  lines) to it; see the concurrency invariants in `CLAUDE.md` and
  `doc/architecture.md#concurrency-model`
- Pre-allocate buffers when size is known
- Consider cache-friendly data layouts

## Testing

### Running Tests

```bash
# All tests
ctest --test-dir build --output-on-failure

# Specific category
./build/cyclone-tests "[directory]"
./build/cyclone-tests "[security]"
./build/cyclone-tests "[edge]"

# Verbose output
./build/cyclone-tests "Test Name" --reporter console
```

### Writing Tests

Use Catch2 with appropriate tags:

```cpp
TEST_CASE("Description of test", "[component][category]")
{
  // Arrange
  Directory dir(100);
  CacheKey key("test-key");

  // Act
  bool result = dir.insert(key, 1000, 512);

  // Assert
  REQUIRE(result);
  REQUIRE(dir.count() == 1);
}
```

**Tag conventions:**
- `[directory]`, `[document]`, `[key]` - Component tags
- `[security]` - Security-related tests
- `[edge]` - Edge cases and boundary conditions
- `[plugin]`, `[http]` - Plugin system tests
- `[lru]`, `[clfus]` - RAM cache tests

### Security Tests

Always add security tests for:
- Deserialization with malicious/corrupted data
- Integer overflow conditions
- Unbounded loops or recursion

```cpp
TEST_CASE("Handle malicious input", "[component][security]")
{
  std::vector<std::byte> malicious_data = /* craft bad data */;
  auto result = Component::deserialize(malicious_data);
  REQUIRE(result.empty());  // Should handle gracefully
}
```

## Pull Request Process

1. **Create a feature branch** from `main`
2. **Write tests** for new functionality
3. **Ensure all tests pass**: `ctest --test-dir build`
4. **Run benchmarks** if performance-related: `./build/performance_baseline`
5. **Update documentation** if adding new features
6. **Submit PR** with clear description

### Commit Message Format

```
Short summary (50 chars or less)

More detailed explanation if needed. Wrap at 72 characters.
Explain what and why, not how.

- Bullet points are okay
- Use present tense ("Add feature" not "Added feature")
```

## Architecture Overview

```
                    ┌─────────────────┐
                    │   Cache API     │
                    └────────┬────────┘
                             │
         ┌───────────────────┼───────────────────┐
         ▼                   ▼                   ▼
┌─────────────────┐  ┌─────────────┐  ┌─────────────────┐
│ Plugin Manager  │  │  RAM Cache  │  │     Volumes     │
└─────────────────┘  └─────────────┘  └────────┬────────┘
                                               │
                     ┌─────────────────────────┼─────────────────────────┐
                     ▼                         ▼                         ▼
              ┌──────────┐              ┌──────────┐              ┌──────────┐
              │ Stripe 0 │              │ Stripe 1 │              │ Stripe N │
              │Directory │              │Directory │              │Directory │
              │  Data    │              │  Data    │              │  Data    │
              └──────────┘              └──────────┘              └──────────┘
```

### Key Design Decisions

1. **10-byte directory entries**: Compact format from ATS, supports 512TB per stripe
2. **CLFUS algorithm**: Scan-resistant caching prevents streaming evictions
3. **Memory-mapped I/O**: OS handles paging, enables zero-copy reads
4. **Lock-free reads**: readers take no stripe lock — per-bucket seqlock
   directories, commit ordering, CRC validation, and read leases carry
   correctness; only the writer's `commit_write` takes the stripe mutex
   (exclusively)
5. **Plugin system**: Extensible without core modifications

## Getting Help

- Read the documentation in `doc/`
- Check existing tests for examples
- Open an issue for questions

## License

By contributing, you agree that your contributions will be licensed under the
Apache License 2.0 (see LICENSE).
