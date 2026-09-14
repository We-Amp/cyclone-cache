// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

#pragma once

#include <thread>

namespace cyclone {

// Intentionally leak a std::thread to avoid UB when the C runtime has already
// killed the thread out from under us — the true process-atexit teardown of a
// static/global, where the thread descriptor may already be invalid so both
// join() and detach() are UB.
//
// This is NARROW. It is NOT a fallback for "the join is taking a while": a
// merely-SLOW runtime thread is still alive, and abandoning it lets it keep
// dereferencing objects the caller then destroys (heap-use-after-free — the
// class of bug the staged teardown fixes removed). For runtime teardown of a
// live, owned thread — anything reached from a Cache component's
// stop()/destructor — join UNCONDITIONALLY; a cooperative loop that re-checks
// its stop flag at coarse (<=100 ms) granularity guarantees the join completes.
// The only in-tree caller that legitimately fits the atexit contract is
// Executor::global() (a process- lifetime singleton). Fork inheritance is
// handled separately by release()ing the thread-owning component in the child
// (see Cache::stop()), never here.
inline void leak_thread_on_shutdown(std::thread t) {
  new std::thread(std::move(t));
}

}  // namespace cyclone
