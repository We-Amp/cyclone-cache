// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <cstdint>

namespace cyclone {

// Internal hook notified when a volume write handle commits successfully.
// A handle holds its observer through a weak reference (it may outlive the
// observer), and attaching one costs no allocation: it is stored in the
// handle the volume already allocated (see Volume::set_commit_observer).
class WriteCommitObserver {
 public:
  virtual void on_write_committed(uint64_t bytes) noexcept = 0;

 protected:
  WriteCommitObserver() = default;
  ~WriteCommitObserver() = default;
  WriteCommitObserver(const WriteCommitObserver &) = default;
  WriteCommitObserver &operator=(const WriteCommitObserver &) = default;
};

}  // namespace cyclone
