// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// Deterministic unit tests for the POSIX opener-side inode re-validation helper
// (cyclone::inodes_match), which backs Volume::open_locked's fail-closed guard
// against a cache file being unlinked/replaced out from under a live opener
// (manual `rm`/`--reset`, or a future GC of superseded fingerprinted files).
// No threads and no timing races: each case sets the inode relationship up
// explicitly.  The whole translation unit is POSIX-only (the helper is guarded
// #ifndef _WIN32 in volume.hpp/volume.cpp).

#include <catch2/catch_test_macros.hpp>

#include "core/volume.hpp"

#ifndef _WIN32

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace cyclone;
namespace fs = std::filesystem;

namespace {

std::string unique_temp_path() {
  static std::atomic<int> counter{0};
  return (fs::temp_directory_path() /
          ("cyclone_inode_test_" + std::to_string(::getpid()) + "_" +
           std::to_string(counter++) + ".dat"))
      .string();
}

// Create (or truncate) a file at `path` and return a read/write fd for it.
int create_and_open(const std::string &path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  REQUIRE(fd >= 0);
  return fd;
}

}  // namespace

TEST_CASE("inodes_match: fd and path naming the same inode -> true",
          "[volume][inode]") {
  const std::string path = unique_temp_path();
  std::remove(path.c_str());
  const int fd = create_and_open(path);

  REQUIRE(inodes_match(fd, path) == true);

  ::close(fd);
  std::remove(path.c_str());
}

TEST_CASE("inodes_match: path replaced by a different inode -> false",
          "[volume][inode]") {
  const std::string path = unique_temp_path();
  std::remove(path.c_str());

  // Open fd on the ORIGINAL inode, then unlink the name and recreate a
  // DIFFERENT file at the same path.  fd still references the old
  // (now-unlinked) inode while the name resolves to the new one -> mismatch,
  // fail closed.
  const int fd = create_and_open(path);
  REQUIRE(inodes_match(fd, path) == true);  // sanity: matched before the swap

  REQUIRE(::unlink(path.c_str()) == 0);
  const int fd2 = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  REQUIRE(fd2 >= 0);  // a brand-new inode now lives at `path`

  REQUIRE(inodes_match(fd, path) == false);
  // The NEW fd, however, does match the path it just created.
  REQUIRE(inodes_match(fd2, path) == true);

  ::close(fd);
  ::close(fd2);
  std::remove(path.c_str());
}

TEST_CASE("inodes_match: path unlinked with no replacement (ENOENT) -> false",
          "[volume][inode]") {
  const std::string path = unique_temp_path();
  std::remove(path.c_str());
  const int fd = create_and_open(path);
  REQUIRE(inodes_match(fd, path) == true);

  // Unlink with no recreate: fd is valid (nlink now 0) but stat(path) fails
  // ENOENT -> false.
  REQUIRE(::unlink(path.c_str()) == 0);
  REQUIRE(inodes_match(fd, path) == false);

  ::close(fd);
}

TEST_CASE("inodes_match: invalid fd -> false", "[volume][inode]") {
  const std::string path = unique_temp_path();
  std::remove(path.c_str());
  const int fd = create_and_open(path);
  ::close(fd);  // fd is now closed -> fstat fails

  REQUIRE(inodes_match(fd, path) == false);

  std::remove(path.c_str());
}

#endif  // !_WIN32
