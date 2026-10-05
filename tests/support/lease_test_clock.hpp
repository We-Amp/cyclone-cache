// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A clock the test owns, for the lease protocol.
//
// The read lease, the wrap gate that honours it and the anti-starvation
// ceiling all compare steady-clock timestamps.  A test that says "a 5 s
// lease holds while I flood the cache" or "sleep 1.6 s so the 1 s lease
// lapses" then depends on how fast the machine runs it: under a sanitizer
// on a loaded host the flood can take longer than the lease, and the case
// fails on an assertion that has nothing to do with what it tests.
//
// While a LeaseTestClock is alive the library reads the time from it
// instead (Volume::s_steady_clock_ns_for_test, a test-seam build hook).
// Time stands still until the test calls advance(), so
//   - a lease lasts exactly until the test moves the clock past it, and
//   - lapsing a lease or passing a ceiling takes no sleep.
//
// The clock is per process and process-wide: use it only in cases whose
// volumes are all in this process, and never while another test's cache is
// running on other threads (Catch2 runs cases one at a time).

#ifndef CYCLONE_TESTS_SUPPORT_LEASE_TEST_CLOCK_HPP
#define CYCLONE_TESTS_SUPPORT_LEASE_TEST_CLOCK_HPP

#include <atomic>
#include <chrono>
#include <cstdint>

#include "core/volume.hpp"

class LeaseTestClock {
 public:
  // Starts at the real steady-clock time, so timestamps already persisted
  // in a volume (a lease stamped before this object existed) stay in the
  // past.
  LeaseTestClock() {
    const auto real_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    _now_ns = real_ns > 0 ? static_cast<uint64_t>(real_ns) : 1;
    publish();
  }
  ~LeaseTestClock() {
    cyclone::Volume::s_steady_clock_ns_for_test.store(
        0, std::memory_order_release);
  }
  LeaseTestClock(const LeaseTestClock &) = delete;
  LeaseTestClock &operator=(const LeaseTestClock &) = delete;

  // Moves the clock forward.  Call it from the test's own thread.
  void advance(std::chrono::nanoseconds by) {
    _now_ns += static_cast<uint64_t>(by.count());
    publish();
  }

  [[nodiscard]] uint64_t now_ns() const { return _now_ns; }

 private:
  void publish() const {
    cyclone::Volume::s_steady_clock_ns_for_test.store(
        _now_ns, std::memory_order_release);
  }

  uint64_t _now_ns = 0;
};

#endif  // CYCLONE_TESTS_SUPPORT_LEASE_TEST_CLOCK_HPP
