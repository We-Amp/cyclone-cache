// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Shared RAII temp-directory helper for cache tests.
//
// Motivation: Cache::add_volume() structural-fingerprints the volume filename
// ("cyclone.dat" -> "cyclone-7-<16hex>.dat", plus a ".small" sibling) inside
// add_volume_locked(), so the file the library actually creates does NOT match
// the raw path a test passed in. Tests that cleaned up with std::remove(raw)
// therefore left the fingerprinted files behind, and on persistent-/tmp CI
// runners those stale files caused cross-run / cross-SECTION contamination.
//
// TempCacheDir sidesteps that entirely: it owns a UNIQUE temp DIRECTORY and
// remove_all()s the whole directory on destruction, so every file the library
// creates inside it (whatever it names them) is reclaimed automatically. It
// honors std::filesystem::temp_directory_path() (i.e. $TMPDIR), which is the
// mechanism the isolation test relies on -- do not hardcode /tmp.

#ifndef CYCLONE_TESTS_SUPPORT_TEMP_CACHE_HPP
#define CYCLONE_TESTS_SUPPORT_TEMP_CACHE_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

// Unique temp directory for a cache test. Created in the constructor and
// recursively removed in the destructor (never throws). Non-copyable and
// move-only so the owned directory always has exactly one owner.
class TempCacheDir {
  // `inline` (via static-inline data member) => exactly ONE counter
  // program-wide. A plain namespace-scope `static` in a header would have
  // internal linkage and give each translation unit its own counter, so two
  // TUs constructing at the same instant could collide.
  static inline std::atomic<uint64_t> s_counter{0};

 public:
  explicit TempCacheDir(std::string_view tag = {}) {
    namespace fs = std::filesystem;
    // pid + monotonic counter + timestamp => unique even across concurrent
    // processes and rapid same-process construction.
    auto count = s_counter.fetch_add(1);
#ifdef _WIN32
    auto pid = static_cast<uint64_t>(_getpid());
#else
    auto pid = static_cast<uint64_t>(getpid());
#endif
    auto now = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count() &
        0xFFFFFFFF);

    std::string name = "cyc_";
    if (!tag.empty()) {
      name += std::string(tag);
      name += '_';
    }
    name += std::to_string(pid) + "_" + std::to_string(count) + "_" +
            std::to_string(now);

    dir_ = fs::temp_directory_path() / name;
    fs::create_directories(dir_);
  }

  ~TempCacheDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  TempCacheDir(const TempCacheDir &) = delete;
  TempCacheDir &operator=(const TempCacheDir &) = delete;

  // Movable so a test fixture can own and hand back a TempCacheDir by value.
  // The moved-from object holds an empty path; remove_all("") is a no-op, so
  // only the live owner removes the directory.
  TempCacheDir(TempCacheDir &&other) noexcept : dir_(std::move(other.dir_)) {
    other.dir_.clear();
  }
  TempCacheDir &operator=(TempCacheDir &&other) noexcept {
    if (this != &other) {
      std::error_code ec;
      std::filesystem::remove_all(dir_, ec);
      dir_ = std::move(other.dir_);
      other.dir_.clear();
    }
    return *this;
  }

  // Path to a (not-yet-created) file inside the temp dir. Defaults to the
  // conventional volume filename; pass a distinct name for a second volume.
  [[nodiscard]] std::string path(std::string_view name = "cyclone.dat") const {
    return (dir_ / std::filesystem::path(name)).string();
  }

  [[nodiscard]] const std::filesystem::path &dir() const { return dir_; }

 private:
  std::filesystem::path dir_;
};

#endif  // CYCLONE_TESTS_SUPPORT_TEMP_CACHE_HPP
