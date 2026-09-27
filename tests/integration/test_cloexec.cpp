// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Every descriptor Cyclone opens on its cache files must be close-on-exec.
// A cache opened before fork() is still shared with the children (fork keeps
// descriptors regardless of FD_CLOEXEC), but a helper the application exec's
// must not inherit the volume: an inherited descriptor pins the file and keeps
// its byte-range locks alive (a dead writer's liveness slot would then read
// as held until the helper exits).

#include <catch2/catch_test_macros.hpp>

#ifndef _WIN32

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <set>
#include <utility>

#include "cyclone/cache.hpp"
#include "support/temp_cache.hpp"

using namespace cyclone;

namespace {

// (dev, inode) of every regular file under `dir`.
std::set<std::pair<dev_t, ino_t>> files_under(const std::filesystem::path& dir) {
  std::set<std::pair<dev_t, ino_t>> ids;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    struct stat st{};
    if (entry.is_regular_file() && ::stat(entry.path().c_str(), &st) == 0) {
      ids.emplace(st.st_dev, st.st_ino);
    }
  }
  return ids;
}

// Counts this process's open descriptors on the given files, and how many of
// them lack FD_CLOEXEC.
std::pair<int, int> scan_fds(const std::set<std::pair<dev_t, ino_t>>& ids) {
  int open_on_cache = 0;
  int inheritable = 0;
  const long max_fd = ::sysconf(_SC_OPEN_MAX);
  for (int fd = 0; fd < (max_fd > 0 ? max_fd : 1024) && fd < 65536; ++fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        ids.count({st.st_dev, st.st_ino}) == 0) {
      continue;
    }
    ++open_on_cache;
    const int fd_flags = ::fcntl(fd, F_GETFD);
    if (fd_flags < 0 || (fd_flags & FD_CLOEXEC) == 0) {
      ++inheritable;
    }
  }
  return {open_on_cache, inheritable};
}

}  // namespace

TEST_CASE("Cache descriptors are close-on-exec", "[cloexec][volume]") {
  for (const bool multi_process : {false, true}) {
    CAPTURE(multi_process);
    TempCacheDir dir("cloexec");
    CacheConfig cfg;
    cfg.set_ram_cache_size(0);
    if (multi_process) {
      cfg.set_multi_process(0, 1);  // also claims a liveness slot
    }
    auto created = Cache::create(cfg);
    REQUIRE(created.has_value());
    auto cache = std::move(*created);
    REQUIRE(cache->add_volume(dir.path(), size_t{8} << 20).has_value());
    REQUIRE(cache->start().has_value());

    const auto [open_on_cache, inheritable] = scan_fds(files_under(dir.dir()));
    CAPTURE(open_on_cache);
    REQUIRE(open_on_cache > 0);
    REQUIRE(inheritable == 0);

    cache->stop();
  }
}

#endif  // !_WIN32
