// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// A wall-clock bound that follows the load on the host.
//
// Several cases in this suite assert a bound or a hang guard whose value is
// calibrated for a host the case has to itself: the case's own threads and
// hogs are the load it measures against, and the bound already allows for
// them.  When several test binaries run at once (ctest -j), or the host is
// busy for other reasons, the same correct code needs more wall time and a
// fixed bound fails without anything being wrong.
//
// wall_load_factor() estimates how much slower ordinary CPU work runs right
// now than it would with the machine to itself: it times a fixed stretch of
// deterministic arithmetic on one thread, then the same stretch on every
// usable CPU at once (a container CPU quota can leave fewer usable than
// visible).  One thread keeps a core whatever else runs, so the ratio of
// the two timings is close to 1 on an idle host and grows with the load.
//
// The probe is coarse, and it is cheap.  Take it ONCE per case, BEFORE the
// case's own threads and hogs exist, so it scales a bound for load the case
// did not create and never for the load the case is there to produce:
// measuring with a case's own oversubscription in flight would scale the
// bound by the very slowdown the case exists to detect.  The factor is
// clamped to [1, 1000]: never tighter than the calibrated bound, and a
// saturated probe cannot stretch a bound past the point where a real hang
// would outlive the whole run.

#ifndef CYCLONE_TESTS_SUPPORT_WALL_LOAD_HPP
#define CYCLONE_TESTS_SUPPORT_WALL_LOAD_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// Where the probe's results go, so the timed work cannot be folded away.
inline std::atomic<uint64_t> s_wall_load_sink{0};

// The timed work: a dependent multiply-xorshift chain.  ~2M rounds is a few
// milliseconds of one core -- long enough to time, too short to notice.
inline uint64_t wall_load_probe_work(uint64_t state, uint64_t rounds) {
  for (uint64_t i = 0; i < rounds; ++i) {
    state *= 0x9E3779B97F4A7C15ull;
    state ^= state >> 29;
  }
  return state;
}

// CPUs this process can actually keep busy at once.
inline unsigned wall_load_usable_cpus() {
  unsigned cpus = std::thread::hardware_concurrency();
  if (cpus == 0) {
    cpus = 1;
  }
#if defined(__linux__)
  // A cgroup CPU quota caps throughput below the visible processor count,
  // and a probe sized to the visible count would read an idle container as
  // busy.  cgroup v2 first ("max 100000" or "<quota> <period>"), v1 second.
  const auto read_first_line = [](const char* path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
  };
  const auto parse_number = [](const std::string& text) {
    return std::strtoll(text.c_str(), nullptr, 10);
  };
  long long quota = 0;
  long long period = 0;
  const std::string v2 = read_first_line("/sys/fs/cgroup/cpu.max");
  if (!v2.empty()) {
    const size_t space = v2.find(' ');
    if (space != std::string::npos) {
      quota = v2.compare(0, space, "max") == 0 ? 0 : parse_number(v2);
      period = parse_number(v2.substr(space + 1));
    }
  } else {
    quota =
        parse_number(read_first_line("/sys/fs/cgroup/cpu/cpu.cfs_quota_us"));
    period =
        parse_number(read_first_line("/sys/fs/cgroup/cpu/cpu.cfs_period_us"));
  }
  if (quota > 0 && period > 0) {
    // A quota below one CPU still leaves one worker: a probe with none
    // could not measure anything.
    cpus = std::min(cpus, std::max(1u, static_cast<unsigned>(quota / period)));
  }
#endif
  return std::max(cpus, 1u);
}

// See the file comment.  Costs one single-thread probe and one probe per
// usable CPU, in the low tens of milliseconds on typical hardware.
inline double wall_load_factor() {
  constexpr uint64_t kRounds = 2'000'000;
  const unsigned workers = wall_load_usable_cpus();

  const auto timed = [&](uint64_t seed) {
    const auto start = std::chrono::steady_clock::now();
    const uint64_t done = wall_load_probe_work(seed, kRounds);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    s_wall_load_sink.fetch_add(done, std::memory_order_relaxed);
    return elapsed;
  };

  const auto alone = timed(0x51ED270Bull);
  std::vector<std::chrono::nanoseconds> together(workers);
  std::vector<std::thread> probes;
  probes.reserve(workers);
  for (unsigned i = 0; i < workers; ++i) {
    probes.emplace_back([&, i] { together[i] = timed(0x9E3779B9ull + i); });
  }
  for (auto& probe : probes) {
    probe.join();
  }
  const auto slowest = *std::max_element(together.begin(), together.end());

  const double alone_ns = static_cast<double>(alone.count());
  if (alone_ns <= 0.0) {
    return 1.0;
  }
  const double factor = static_cast<double>(slowest.count()) / alone_ns;
  return std::clamp(factor, 1.0, 1000.0);
}

// Scales a calibrated wall-clock bound by a fresh load measurement, rounded
// up so scaling can only loosen the bound, never tighten it.  Call it where
// wall_load_factor() should be taken: before the case's own threads and
// hogs exist (see the file comment).
template <typename Rep, typename Period>
std::chrono::duration<Rep, Period> scaled_by_load(
    std::chrono::duration<Rep, Period> bound) {
  const double scaled =
      std::chrono::duration<double, Period>(bound).count() * wall_load_factor();
  return std::chrono::duration<Rep, Period>(
      static_cast<Rep>(std::ceil(scaled)));
}

#endif  // CYCLONE_TESTS_SUPPORT_WALL_LOAD_HPP
