// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A file carrying the write-lock liveness slots (WriterLiveness) of
// a bare MmapDirectory under test.  A Volume attaches its own volume file;
// a test that maps a directory on its own (anonymous shared memory, say)
// attaches one of these, so a holder that dies is proven dead by its slot
// lock as it would be in production.  POSIX only: the fork-based tests that use
// it are POSIX only.

#ifndef CYCLONE_TESTS_SUPPORT_LIVENESS_FILE_HPP
#define CYCLONE_TESTS_SUPPORT_LIVENESS_FILE_HPP

#ifndef _WIN32

#include <fcntl.h>
#include <unistd.h>

#include <string>

#include "core/mmap_directory.hpp"
#include "support/temp_cache.hpp"

class LivenessFile {
 public:
  explicit LivenessFile(cyclone::MmapDirectory &dir)
      : tmp_("liveness"), path_(tmp_.path("liveness.dat")) {
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    live_.attach(fd_, path_);
    dir.set_liveness(&live_);
  }
  ~LivenessFile() {
    live_.detach();
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  LivenessFile(const LivenessFile &) = delete;
  LivenessFile &operator=(const LivenessFile &) = delete;

  [[nodiscard]] bool ok() const { return fd_ >= 0; }
  [[nodiscard]] const std::string &path() const { return path_; }
  [[nodiscard]] cyclone::WriterLiveness &liveness() { return live_; }

 private:
  TempCacheDir tmp_;
  std::string path_;
  int fd_ = -1;
  cyclone::WriterLiveness live_;
};

#endif  // !_WIN32

#endif  // CYCLONE_TESTS_SUPPORT_LIVENESS_FILE_HPP
